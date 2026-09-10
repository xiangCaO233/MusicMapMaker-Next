#include "logic/session/SessionUtils.h"

#include "common/VideoFrameDecoder.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectDraftLaneService.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/NoteIdentity.h"
#include "logic/session/SelectionState.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "mmm/timing/BpmNormalization.h"
#include <algorithm>
#include <deque>
#include <limits>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <system_error>

namespace MMM::Logic
{

/// @brief 根据背景类型探测并缓存原始尺寸。
/// @param ctx 目标会话上下文。
/// @param metadata 待探测的谱面基础元数据。
/// @param project 当前项目；为空时从谱面目录解析资源。
/// @note 只缓存尺寸，不创建 GPU 纹理或开始视频播放；失败时保持零尺寸。
/// @note 路径解析与谱面背景元数据绑定，不用当前工作目录作为相对资源基准。
/// @warning 低频资源加载路径：会访问文件系统并打开图片或视频。
void SessionUtils::updateBackgroundSize(SessionContext&         ctx,
                                        const MMM::BaseMapMeta& metadata,
                                        const ::MMM::Project*   project)
{
    // 先清空旧结果，缺失或损坏的新资源不能继续沿用上一谱面的宽高比。
    ctx.bgSize = glm::vec2(0.0f);
    // 缓存没有单独的失败标志，消费者必须允许零尺寸并采用自己的布局后备。
    if ( metadata.main_cover_path.empty() ) {
        return;
    }

    // 有项目时相对项目根目录解析，不能默认所有资源都与谱面文件同目录。
    const std::filesystem::path backgroundPath =
        project ? project->m_projectRoot / metadata.main_cover_path
                : metadata.map_path.parent_path() / metadata.main_cover_path;
    std::error_code backgroundPathError;
    // 非抛异常查询同时检查文件类型和错误码，路径不可访问也按探测失败处理。
    if ( !std::filesystem::is_regular_file(backgroundPath,
                                           backgroundPathError) ||
         backgroundPathError ) {
        return;
    }

    if ( metadata.cover_type == MMM::CoverType::VIDEO ) {
        // 类型来自谱面元数据，不以扩展名再次猜测图片或视频。
        // 视频探测失败后直接结束，避免用图片探针误解释同一资源。
        // 视频必须走媒体探针，不能用图片头解析得到伪尺寸。
        const auto videoInfo = ::MMM::Utils::probeVideoInfo(backgroundPath);
        if ( videoInfo && videoInfo->width > 0 && videoInfo->height > 0 ) {
            ctx.bgSize = glm::vec2(static_cast<float>(videoInfo->width),
                                   static_cast<float>(videoInfo->height));
        }
        return;
    }

    // 图片只读头部尺寸，避免为了布局预先解码整张像素数据。
    int width          = 0;
    int height         = 0;
    int componentCount = 0;
    // UTF-8 路径临时字符串在本次同步调用期间有效，不交给异步任务保存。
    // 通道数仅供图片头探针接收，不影响画布按宽高比布局的缓存内容。
    if ( stbi_info(Config::pathToUtf8(backgroundPath).c_str(),
                   &width,
                   &height,
                   &componentCount) ) {
        ctx.bgSize =
            glm::vec2(static_cast<float>(width), static_cast<float>(height));
    }
}

/// @brief 将谱面载入会话上下文并标记复合音频描述符待构建。
/// @param ctx 目标会话上下文。
/// @param beatmap 待载入谱面；为空时清空当前谱面状态。
/// @note 本函数会补充模型运行时身份和迁移旧批注，不是只读模型投影。
/// @note 清空旧实体和动作栈后才创建新实体，调用方不能保留旧 entt 身份继续编辑。
/// @pre
/// 模型内折线引用有效且类型标记与实际派生类型一致，加载不修复悬空模型引用。
/// @pre 其他持有方不得并发修改传入模型；会话锁不替代模型外部持有方的同步。
/// @note 此入口不读取谱面文件，beatmap 应已由格式加载层构造完成。
/// @note 非空载入的完成含义是模型与 ECS 已对应，不代表音频设备已激活。
/// @warning 共享 BeatMap 所有权在低频载入时交接，用于会话持续编辑同一模型。
/// @warning
/// 低频谱面加载路径：会访问文件系统、探测背景资源并重建 ECS，
/// 不允许放入每帧 update。
void SessionUtils::loadBeatmap(SessionContext&               ctx,
                               std::shared_ptr<MMM::BeatMap> beatmap)
{
    auto& mutex = EditorEngine::instance().getSessionMutex();
    // 将清空、模型替换与 ECS 重建包在同一次会话锁内，避免读取到半载入状态。
    // 使用现有递归锁兼容调用链已经持锁进入，不在此自行释放锁等待资源探测。
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // 先清选择索引再销毁实体，防止旧选择引用随着实体编号复用指向新物件。
    clearChartObjectSelectionIndex(ctx, ChartObjectKind::PlayerNote);
    clearChartObjectSelectionIndex(ctx, ChartObjectKind::AudioSample);
    // 两套 Registry 可包含相同的数值实体号，选择种类必须参与清理，
    // 不能把音符选择集合当作采样选择集合的替代品。
    ctx.noteRegistry.clear();
    ctx.sampleRegistry.clear();
    ctx.timelineRegistry.clear();
    // 撤销记录属于旧谱面，不能跨模型载入复用。
    ctx.actionStack.clear();
    // 不回放旧撤销命令来实现清空；旧命令可能引用即将作废的实体和模型内容。
    // 这些索引不拥有实体，Registry 清空不会替它们清除旧句柄。
    // 必须先废弃旧索引，再允许后面的创建过程复用实体编号。
    ctx.sortedNoteEntities.clear();
    // 实体顺序与结束时间前缀必须一起失效，否则裁剪查询可能借用旧下标。
    ctx.sortedNoteMaxEndPrefix.clear();
    ctx.sortedSampleEntities.clear();
    ctx.sortedSampleMaxEndPrefix.clear();
    // 音符与自动采样使用两套有序索引，清空其中一套不能代替另一套失效处理。
    // 派生显示缓存也属于旧模型，不能仅靠新实体数量变化判断是否重建。
    ctx.annotationRenderCache.clear();
    ctx.previewDensityObjectTimes.clear();
    ctx.previewDensityCache.clear();
    // 相机本身继续存在，但其上次快照时间不再代表当前谱面已生成过画面。
    ctx.lastCameraSnapshotTimes.clear();
    // 相机布局的持久化不在此入口处理，载入新谱面不写用户的停靠配置。
    // 保留相机对象是为了延续用户布局，清除时间戳则强制新模型重新提供各视口画面。
    // 标记需要重新建立的缓存；刚清空的实体表不需要额外清理失效实体。
    ctx.isNoteOrderDirty             = true;
    ctx.isNotePruneDirty             = false;
    ctx.isNoteStatsDirty             = true;
    ctx.isPreviewDensityDirty        = true;
    ctx.isSampleOrderDirty           = true;
    ctx.isSamplePruneDirty           = false;
    ctx.isAnnotationRenderCacheDirty = true;
    // 默认对象种类只是无目标状态的配套值，是否存在目标仍由 null 判定。
    // 不可仅凭恢复成 PlayerNote 就在新谱面上触发一次玩家物件操作。
    // 同时重置实体与部位索引，避免旧折线子索引被用于新谱面的悬浮反馈。
    ctx.hoveredEntity     = entt::null;
    ctx.hoveredObjectKind = ChartObjectKind::PlayerNote;
    ctx.hoveredPart       = static_cast<std::int32_t>(HoverPart::None);
    ctx.hoveredSubIndex   = -1;
    ctx.draggedEntity     = entt::null;
    ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
    ctx.draggedPart       = HoverPart::None;
    ctx.draggedSubIndex   = -1;
    // 拖动初态引用旧模型的内容，不能让未结束的手势在载入后继续提交。
    ctx.dragInitialNote.reset();
    ctx.dragInitialSample.reset();
    ctx.isDragging = false;
    // 擦除预览的待处理集合不能跨谱面保留。
    ctx.eraserState.targetEntities.clear();
    ctx.eraserState.isActive         = false;
    ctx.eraserState.targetObjectKind = ChartObjectKind::PlayerNote;
    ctx.audioTimelineDescriptor      = {};
    // 描述符清空后由后续脏标记驱动重新生成，不在旧描述上逐条覆盖资源事件。
    // 新谱面的草稿组关联与同步基线需要重新绑定，不能沿用旧组版本。
    ctx.m_draftLaneBeatmapPath.clear();
    ctx.m_draftLaneGroupRevision = 0U;
    ctx.m_draftLaneBasePayload.clear();
    ctx.m_needsDraftNotesSync         = false;
    ctx.audioTimelineTotalTime        = 0.0;
    ctx.missingAudioTimelineClipCount = 0U;
    // 缺失计数属于上一份复合音频描述，不能在新资源尚未解析时继续展示。
    // 清零只是未知的新基线，并不宣告新谱面的所有音频资源都可用。
    // 描述、激活与发布分开标记；此处不直接开始播放或假定旧音频可以复用。
    ctx.isAudioTimelineDescriptorDirty           = true;
    ctx.isAudioTimelineActivationPending         = true;
    ctx.isAudioTimelineFingerprintPublishPending = true;

    // 载入结束前先退出所有传输交互，包括本地播放、拖动定位和同步跟随。
    ctx.isPlaying                   = false;
    ctx.isSeekScrubbing             = false;
    ctx.isAudioTimelineSyncFollower = false;
    ctx.m_audioTimelineSyncSourceFingerprint.clear();
    ctx.restartPlaybackAfterFinishPending = false;
    // 加载只撤销会话的播放意图，不在这里同步等待音频回调停止。
    // 新描述的提交和设备状态切换由随后处理激活标记的流程负责。
    // 逻辑时间回零，视觉时间保留用户配置偏移，两种原点不能强行相等。
    ctx.currentTime = 0.0;
    ctx.animateTime =
        ctx.currentTime + ctx.lastConfig.visual.getEffectiveVisualOffset();
    // 目标值与当前值对齐并结束旧动画，避免从上一谱面视野缓动过来。
    ctx.animateTimeTarget          = ctx.animateTime;
    ctx.animateTimeAnimationActive = false;
    ctx.animatedTimelineZoom       = ctx.lastConfig.visual.timelineZoom;
    ctx.animatedTimelineZoomTarget = ctx.animatedTimelineZoom;
    ctx.animatedTimelineZoomAnimationActive = false;
    ctx.currentBeatmap                      = beatmap;
    // 会话保留共享模型，ECS 是编辑视图；后续保存仍需将运行期修改同步回该模型。

    if ( !beatmap ) {
        // 此分支没有“已加载模型”，后续同步应通过 currentBeatmap 的空值短路。
        // 空载入是清空请求，不执行下面的资源探测或模型迁移。
        // 事件表已清空，三个播放/预读游标同时回零并保持清洁状态。
        ctx.bgmTrackCount = 0;
        ctx.hitEvents.clear();
        ctx.nextHitIndex                = 0;
        ctx.nextPredictHitIndex         = 0;
        ctx.nextBoundSoundPrefetchIndex = 0;
        ctx.isHitEventsDirty            = false;
        return;
    }

    auto* project = EditorEngine::instance().getCurrentProject();
    // 当前项目供资源定位与草稿关联使用，不能用传入模型的共享所有权
    // 推断它拥有项目；模型载入本身也不会在这里切换全局项目。
    // 背景探针只在非空加载路径执行，清空会话不会为旧模型再访问一次磁盘。
    SessionUtils::updateBackgroundSize(
        ctx, beatmap->m_baseMapMetadata, project);

    ctx.trackCount = beatmap->m_baseMapMetadata.track_count;
    // 兼容轨数只写会话，不在载入时改写原模型的基础元数据。
    // 无有效玩家轨数时沿用加载兼容默认值；BGM 轨数允许为零。
    if ( ctx.trackCount <= 0 ) ctx.trackCount = 12;
    ctx.bgmTrackCount = std::max(0, beatmap->m_baseMapMetadata.bgm_track_count);

    // 协作逻辑标识只存在于运行时模型中；载入普通谱面时在构建 ECS 前补齐，
    // 保证房主首次发布的快照与本地操作栈引用同一逻辑物件。
    // 身份准备先于旧批注迁移，否则新记录无法稳定指向首次导入的物件。
    // 随后的组件复制也复用这些身份，不为模型与 ECS 分配两套标识。
    for ( auto& note : beatmap->m_noteData.notes ) {
        ensureNoteCollaborationIdentity(note);
    }
    for ( auto& hold : beatmap->m_noteData.holds ) {
        ensureNoteCollaborationIdentity(hold);
    }
    for ( auto& flick : beatmap->m_noteData.flicks ) {
        ensureNoteCollaborationIdentity(flick);
    }
    for ( auto& polyline : beatmap->m_noteData.polylines ) {
        ensureNoteCollaborationIdentity(polyline);
        // 模型子引用可能也出现在上面的类型容器中；ensure 只补缺失身份，
        // 同一物件经不同入口访问时必须保留同一个逻辑标识。
        for ( auto& subNote : polyline.m_subNotes ) {
            ensureNoteCollaborationIdentity(subNote.get());
        }
    }
    for ( auto& sample : beatmap->m_audioSamples ) {
        ensureSampleCollaborationIdentity(sample);
    }

    // 旧版 MMM 把单段纯文本直接挂在玩家物件上；首次进入会话时迁移为
    // 可多条共存的新批注记录，并保留旧字段以兼容尚未升级的导出链路。
    std::unordered_set<std::string> migratedLegacyAnnotationKeys;
    // 非玩家目标的记录不会抑制玩家批注迁移，即使它们恰好使用相同目标字符串。
    // 去重集合仅服务这次迁移，不作为跨会话的批注身份索引保存。
    // 先登记已有玩家批注，重复载入同一模型时不要再复制一份旧文本。
    for ( const auto& annotation : beatmap->m_annotations ) {
        if ( annotation.m_targetKind ==
             ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
            migratedLegacyAnnotationKeys.insert(annotation.m_targetId + "\n" +
                                                annotation.m_content);
        }
    }
    /// @brief 将单个旧玩家物件批注追加为新记录，保留已有记录且遵守数量上限。
    /// @param note 已补齐稳定身份的旧模型物件。
    /// @note 迁移只追加新批注，目标 ID 始终引用原物件，不改变原物件身份。
    auto migrateLegacyAnnotation = [&](const ::MMM::Note& note) {
        // 相同物件上的不同文本可形成多条记录，去重不是按目标身份覆盖旧内容。
        // 达到记录上限时保留旧字段，而不是先删旧文本再尝试追加。
        // 此次兼容迁移不驱逐已有批注，也不把被跳过的记录登记成已迁移。
        if ( note.m_annotation.empty() || note.m_collaborationId.empty() ||
             beatmap->m_annotations.size() >=
                 ::MMM::MAX_BEATMAP_ANNOTATION_COUNT ) {
            return;
        }
        // 以目标逻辑身份和内容联合去重，不合并不同物件上的同一句文本。
        const std::string key =
            note.m_collaborationId + "\n" + note.m_annotation;
        // 原文本不做裁剪或改写，否则同一模型重复载入时无法匹配已有迁移内容。
        if ( !migratedLegacyAnnotationKeys.insert(key).second ) return;
        // 新记录分配独立身份，目标仍指向原物件；不修改其旧批注字段。
        beatmap->m_annotations.push_back(
            { .m_id         = makeNoteCollaborationId(),
              .m_targetKind = ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
              .m_targetId   = note.m_collaborationId,
              .m_timestamp  = note.m_timestamp,
              .m_author     = {},
              .m_content    = note.m_annotation });
    };
    // 迁移直接使用模型时间，批注记录不经过 ECS 的秒单位换算。
    // 旧格式没有独立作者信息，因此不把当前操作者冒充为原批注作者。
    for ( const auto& note : beatmap->m_noteData.notes ) {
        migrateLegacyAnnotation(note);
    }
    for ( const auto& hold : beatmap->m_noteData.holds ) {
        migrateLegacyAnnotation(hold);
    }
    for ( const auto& flick : beatmap->m_noteData.flicks ) {
        migrateLegacyAnnotation(flick);
    }
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        // 兼容迁移只处理折线父物件的旧字段，不在此遍历子节点生成批注。
        migrateLegacyAnnotation(polyline);
    }

    // Registry 的实体清空不等于销毁 context 中的缓存对象，已有缓存仍须标脏。
    // 缓存是否创建由会话装配负责，本函数只使现存缓存失效，
    // 不在无缓存的上下文中悄悄增加一个新的系统实例。
    if ( auto* cache =
             ctx.timelineRegistry.ctx().find<System::ScrollCache>() ) {
        cache->isDirty = true;
    }

    // 根据传入的 BeatMap 构建 ECS 实体
    // 1. 加载 Timing 点
    // 效果值沿用模型已解析的结果；这里不将所有参数按 BPM 解释，
    // 否则 SV 等非节奏事件会在载入时被错误规范化。
    for ( const auto& timing : beatmap->m_timings ) {
        // 这里按模型现有顺序创建实体，不假设实体号能表达最终时间先后关系。
        // 保留各种时间线效果，不只导入 BPM；滚动系统随后按效果统一重建。
        auto  entity = ctx.timelineRegistry.create();
        auto& tc     = ctx.timelineRegistry.emplace<TimelineComponent>(
            entity,
            timing.m_timestamp / 1000.0,  // 毫秒转秒
            timing.m_timingEffect,
            timing.m_timingEffectParameter);
        // 元数据承载格式扩展与效果属性，不能只复制时间和数值丢掉解释上下文。
        tc.m_metadata = timing.m_metadata;
    }

    // 用于追踪子物件，防止在折线之外重复绘制
    // 键是模型对象地址，仅在本次加载中使用；不能跨模型容器搬移持久保存。
    std::unordered_map<const ::MMM::Note*, entt::entity> noteToEntity;
    // 通过对象地址关联模型引用，而不是时间和轨道；重叠物件也必须区分实体。

    // 2. 加载普通音符 (Notes)
    // 先建立各具体类型实体，再由折线引用将属于父容器的实体改为子物件。
    for ( const auto& note : beatmap->m_noteData.notes ) {
        auto entity = ctx.noteRegistry.create();
        // 协作身份用于跨模型识别，地址映射仅用于本次折线连接，两者不可互换。
        noteToEntity[&note] = entity;

        int track = static_cast<int>(note.m_track);

        auto& nc = ctx.noteRegistry.emplace<NoteComponent>(
            entity,
            note.m_type,
            note.m_timestamp / 1000.0,  // 毫秒转秒
            0.0,
            track);
        // ECS 拷贝元数据后可独立编辑，加载结束并不会建立字段级双向绑定。
        // 之后修改组件仍需走回写入口，不能以模型指针共享为由省略同步。
        nc.m_metadata = note.m_metadata;
        // 附加字段保留编辑与导出所需信息，不只导入当前画面需要的几何数据。
        nc.m_annotation    = note.m_annotation;
        nc.m_sampleBinding = note.getSampleBinding();
        // 通过模型访问器读取绑定，保持与其他模型消费者相同的绑定解释规则。
        nc.m_collaborationId = note.m_collaborationId;
        // 载入时解析颜色覆盖，后续渲染读取缓存，避免逐帧解析元数据字符串。
        loadNoteColorOverridesFromMetadata(nc);

        ctx.noteRegistry.emplace<TransformComponent>(
            // 初始 Transform 只是占位，正式画布投影随后根据轨道布局更新。
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 3. 加载长键 (Holds)
    // 长条起点与时长都要换算成秒，只换算起点会造成结束时间放大千倍。
    // 零时长依旧按 HOLD 类型加载，不在投影层将纯长条转换为 NOTE。
    for ( const auto& hold : beatmap->m_noteData.holds ) {
        // 长条依旧只有一个实体，持续区间由组件时长表达，不创建独立尾实体。
        auto entity         = ctx.noteRegistry.create();
        noteToEntity[&hold] = entity;

        int track = static_cast<int>(hold.m_track);

        auto& nc = ctx.noteRegistry.emplace<NoteComponent>(
            entity,
            hold.m_type,
            hold.m_timestamp / 1000.0,  // 毫秒转秒
            hold.m_duration / 1000.0,   // 毫秒转秒
            track);
        nc.m_metadata = hold.m_metadata;
        // 长条采样绑定仍挂在同一物件上，不因持续区间拆成额外自动采样实体。
        nc.m_annotation      = hold.m_annotation;
        nc.m_sampleBinding   = hold.getSampleBinding();
        nc.m_collaborationId = hold.m_collaborationId;
        loadNoteColorOverridesFromMetadata(nc);

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 4. 加载滑键 (Flicks)
    // 横向轨差保留正负号，用于箭头方向、覆盖范围和后续拖动边界。
    for ( const auto& flick : beatmap->m_noteData.flicks ) {
        // 滑键的横向跨度与时间长度无关，不能将 dtrack 当作持续时间写入。
        auto entity          = ctx.noteRegistry.create();
        noteToEntity[&flick] = entity;

        int track = static_cast<int>(flick.m_track);

        auto& nc =
            ctx.noteRegistry.emplace<NoteComponent>(entity,
                                                    flick.m_type,
                                                    flick.m_timestamp / 1000.0,
                                                    0.0,
                                                    track,
                                                    flick.m_dtrack);
        nc.m_metadata        = flick.m_metadata;
        nc.m_annotation      = flick.m_annotation;
        nc.m_sampleBinding   = flick.getSampleBinding();
        nc.m_collaborationId = flick.m_collaborationId;
        loadNoteColorOverridesFromMetadata(nc);

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 5. 加载折线 (Polylines)
    // 父组件的持续时间先置零，实际各段长度保存在内嵌子节点中。
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        auto entity = ctx.noteRegistry.create();
        // 父实体不加入叶节点地址表；模型折线连接应引用实际的具体节点对象。

        int track = static_cast<int>(polyline.m_track);

        auto& comp = ctx.noteRegistry.emplace<NoteComponent>(
            entity, polyline.m_type, polyline.m_timestamp / 1000.0, 0.0, track);
        comp.m_metadata        = polyline.m_metadata;
        comp.m_annotation      = polyline.m_annotation;
        comp.m_sampleBinding   = polyline.getSampleBinding();
        comp.m_collaborationId = polyline.m_collaborationId;
        loadNoteColorOverridesFromMetadata(comp);

        // 填充子物件并标记它们为 SubNote
        // 按模型引用顺序复制，不按类型容器顺序重新组合折线。
        // 对应实体不是节点值的拥有者；父组件内嵌数组才承载折线连接内容。
        // 后续节点编辑必须维持这两种表示的一致，不能只修改子实体几何。
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            const auto& subNote = subNoteRef.get();
            // 模型保持引用关系，ECS 父组件保存值节点，两者只在加载时建立对应。

            // 标记原始实体为 SubNote，防止独立绘制
            // 子实体仍可用于编辑定位，父组件同时保存独立的渲染节点值。
            if ( auto it = noteToEntity.find(&subNote);
                 it != noteToEntity.end() ) {
                // 一个派生实体只保存一个父身份；有效输入应避免把同一叶实体
                // 作为多个父折线的可独立编辑子实体使用。
                auto& subComp = ctx.noteRegistry.get<NoteComponent>(it->second);
                subComp.m_isSubNote      = true;
                subComp.m_parentPolyline = entity;
                // 追加前的节点数就是当前索引，必须与紧接着 push_back
                // 的顺序一致。
                subComp.m_subIndex = static_cast<int>(comp.m_subNotes.size());
            }

            // 没找到先前创建的实体时仍保留模型节点值，不凭空创建替代实体。
            // 因此关联缺失与折线内容缺失是两回事，不能将此分支改成整项跳过。
            NoteComponent::SubNote sn;
            // 子节点时间仍是谱面绝对时间，不减父折线时间构造相对偏移。
            sn.type       = subNote.m_type;
            sn.timestamp  = subNote.m_timestamp / 1000.0;
            sn.trackIndex = static_cast<int>(subNote.m_track);
            sn.duration   = 0.0;
            // 先清类型专属字段，普通节点不能带入其他节点的时长或轨差。
            sn.dtrack = 0;

            if ( subNote.m_type == ::MMM::NoteType::HOLD ) {
                // 模型类型标记决定派生类型，加载前应已保证引用与实际类型一致。
                const auto& h = static_cast<const ::MMM::Hold&>(subNote);
                sn.duration   = h.m_duration / 1000.0;
            } else if ( subNote.m_type == ::MMM::NoteType::FLICK ) {
                const auto& f = static_cast<const ::MMM::Flick&>(subNote);
                sn.dtrack     = f.m_dtrack;
            }
            sn.metadata      = subNote.m_metadata;
            sn.annotation    = subNote.m_annotation;
            sn.sampleBinding = subNote.getSampleBinding();
            // 节点绑定在此不继承父绑定，发声展开时仅为无绑定的头部提供后备。
            sn.collaborationId = subNote.m_collaborationId;
            loadNoteColorOverridesFromMetadata(sn);
            comp.m_subNotes.push_back(sn);
            // 节点值复制进父组件后不再借用局部
            // sn，避免后续循环覆盖上一节点状态。
        }

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 6. 自动采样使用独立 Registry，避免进入判定、Combo、KPS 与 HitFX。
    // 样本转换统一交给组件工厂，保持与其他导入入口相同的时间和字段语义。
    for ( const auto& sample : beatmap->m_audioSamples ) {
        // 自动采样直接带交互组件，拾取只需要样本身份，不进入音符的父子映射。
        // 即使时间与某个玩家音符一致也保持独立事件，不能按时刻合并实体。
        auto entity = ctx.sampleRegistry.create();
        ctx.sampleRegistry.emplace<SampleComponent>(
            entity, SampleComponent::fromAudioSample(sample));
        ctx.sampleRegistry.emplace<InteractionComponent>(entity);

        // BGM 使用玩家轨之后的全局轨号，最远样本决定至少需要显示多少 BGM 轨。
        if ( sample.m_track >= static_cast<std::uint32_t>(ctx.trackCount) ) {
            const auto requiredBgmCount =
                sample.m_track - static_cast<std::uint32_t>(ctx.trackCount) + 1;
            const auto clampedRequiredBgmCount =
                // 模型轨号为无符号，写入会话有符号轨数前限制到其表示上限。
                static_cast<std::int32_t>(std::min<std::uint32_t>(
                    requiredBgmCount,
                    static_cast<std::uint32_t>(
                        std::numeric_limits<std::int32_t>::max())));
            ctx.bgmTrackCount =
                std::max(ctx.bgmTrackCount, clampedRequiredBgmCount);
        }
    }

    // 构建音效触发事件队列并排序
    // 以下直接从模型生成首次播放事件，不等待后续组件更新再触发重建。
    ctx.hitEvents.clear();
    ctx.nextHitIndex                = 0;
    ctx.nextBoundSoundPrefetchIndex = 0;
    // 声音预读与实际触发各有游标，预读过资源不意味着事件已经播放。

    // 收集所有的 subNote 引用，避免它们被重复加入普通音符的播放队列
    // 去重依据对象身份而非同刻同轨，两个独立重叠物件仍应分别产生事件。
    std::unordered_set<const ::MMM::Note*> subNotesSet;
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            subNotesSet.insert(&subNoteRef.get());
        }
    }

    using HitRole = System::HitFXSystem::HitEvent::Role;

    // 这里的事件时间采用会话秒单位，绑定仍保留资源身份及其播放参数。
    // 加载只生成事件描述，不在遍历模型时解码资源或实际触发音频。
    for ( const auto& note : beatmap->m_noteData.notes ) {
        if ( subNotesSet.find(&note) != subNotesSet.end() ) continue;
        // 子节点在下面的折线遍历中保留角色，不能同时以独立物件角色触发一次。
        // 普通音符没有折线角色，覆盖一轨且持续时间为零。
        ctx.hitEvents.push_back({ note.m_timestamp / 1000.0,
                                  note.m_type,
                                  HitRole::None,
                                  1,
                                  static_cast<int>(note.m_track),
                                  0,
                                  0.0,
                                  false,
                                  note.getSampleBinding() });
    }
    for ( const auto& hold : beatmap->m_noteData.holds ) {
        if ( subNotesSet.find(&hold) != subNotesSet.end() ) continue;
        // 长条只产生一个携带持续时间的事件，不在加载时逐格展开连击或播放点。
        ctx.hitEvents.push_back({ hold.m_timestamp / 1000.0,
                                  hold.m_type,
                                  HitRole::None,
                                  1,
                                  static_cast<int>(hold.m_track),
                                  0,
                                  hold.m_duration / 1000.0,
                                  false,
                                  hold.getSampleBinding() });
    }
    for ( const auto& flick : beatmap->m_noteData.flicks ) {
        if ( subNotesSet.find(&flick) != subNotesSet.end() ) continue;
        // 覆盖轨数包含起始轨，因此绝对轨差需要加一；方向单独保存在
        // trackOffset。
        int span = std::abs(flick.m_dtrack) + 1;
        ctx.hitEvents.push_back({ flick.m_timestamp / 1000.0,
                                  flick.m_type,
                                  HitRole::None,
                                  span,
                                  static_cast<int>(flick.m_track),
                                  flick.m_dtrack,
                                  0.0,
                                  false,
                                  flick.getSampleBinding() });
    }
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        // 对于 Polyline 本身不发声，由子物件发声
        // 节点角色来自模型中的连接顺序，不能先按节点时间排序再判定首尾。
        // 空折线自然不产生事件；它的父实体仍已在前面的 ECS 加载中保留。
        size_t subNoteCount = polyline.m_subNotes.size();
        for ( size_t i = 0; i < subNoteCount; ++i ) {
            const auto& subNote = polyline.m_subNotes[i].get();

            HitRole role = HitRole::Internal;
            // 单节点折线优先使用 Head 角色，不能因为它也是末项而覆盖成 Tail。
            if ( i == 0 )
                role = HitRole::Head;
            else if ( i == subNoteCount - 1 )
                role = HitRole::Tail;

            int    span        = 1;
            int    trackOffset = 0;
            double duration    = 0.0;
            // 折线身份与节点具体类型是两个维度：角色决定首尾反馈，
            // 类型决定轨道跨度或持续时间，不能将所有节点简化为普通点击。
            if ( subNote.m_type == ::MMM::NoteType::FLICK ) {
                const auto& f = static_cast<const ::MMM::Flick&>(subNote);
                span          = std::abs(f.m_dtrack) + 1;
                trackOffset   = f.m_dtrack;
            } else if ( subNote.m_type == ::MMM::NoteType::HOLD ) {
                const auto& h = static_cast<const ::MMM::Hold&>(subNote);
                duration      = h.m_duration / 1000.0;
            }

            auto sampleBinding = subNote.getSampleBinding();
            // 保留节点显式绑定优先级；父级采样只作为头部缺省值。
            if ( !sampleBinding && role == HitRole::Head ) {
                sampleBinding = polyline.getSampleBinding();
            }
            // 父绑定只补到生成的事件副本，不写回节点模型；
            // 后续保存仍能区分节点显式绑定和父级缺省绑定。
            ctx.hitEvents.push_back({ subNote.m_timestamp / 1000.0,
                                      subNote.m_type,
                                      role,
                                      span,
                                      static_cast<int>(subNote.m_track),
                                      trackOffset,
                                      duration,
                                      true,
                                      std::move(sampleBinding) });
        }
    }
    // 各模型容器独立遍历后时间并不全局有序，发布播放缓存前需统一排序。
    std::sort(ctx.hitEvents.begin(), ctx.hitEvents.end());
    // 排序后的缓存拥有绑定副本，播放阶段无需借用本次模型遍历的局部对象。
    // 初次事件表已完整建立，清除脏标记避免下一次读取立即重复构造。
    ctx.isHitEventsDirty = false;

    XINFO(
        "Loaded new BeatMap with {} notes, {} holds, {} flicks, {} polylines "
        "and {} timings and {} audio samples.",
        beatmap->m_noteData.notes.size(),
        beatmap->m_noteData.holds.size(),
        beatmap->m_noteData.flicks.size(),
        beatmap->m_noteData.polylines.size(),
        beatmap->m_timings.size(),
        beatmap->m_audioSamples.size());

    // 正式物件已载入后再恢复项目草稿，使草稿关联使用当前项目和轨道环境。
    ProjectDraftLaneService::load(ctx, project);
    // 草稿在正式事件表生成后载入，不能把练习或暂存物件加入首次正式发声队列。

    // 载入不是编辑提交，不应立即把刚导入的数据作为变更同步回模型。
    ctx.m_needsTimingsSync = false;
    ctx.m_needsNotesSync   = false;
    ctx.m_needsSamplesSync = false;
    // 草稿加载可能追加实体，排序与统计需在完整数据就绪后统一重建。
    ctx.isNoteOrderDirty   = true;
    ctx.isNotePruneDirty   = false;
    ctx.isNoteStatsDirty   = true;
    ctx.isSampleOrderDirty = true;
    ctx.isSamplePruneDirty = false;
}

/// @brief 将新建的独立正式音符直接追加到模型，避免为单个创建重建全部物件。
/// @param ctx 持有当前谱面及会话锁环境的上下文。
/// @param note 已分配协作身份的新音符组件，使用秒单位时间。
/// @return 支持且已追加时返回 true，不适用该快速路径时返回 false。
/// @pre 调用方确认物件尚未写入模型，且轨道及时间已合法化。
/// @note 不按协作身份查重，也不清理其他模型同步标记。
/// @note 返回 false 只表示不能使用增量追加，调用方仍可安排完整类别同步。
/// @note 返回 true 不包含文件保存、撤销记录创建或 ECS 索引重建。
/// @pre 当前模型在入口检查到取得会话锁之间保持有效；调用方负责串行化模型切换。
/// @warning 低频编辑提交路径会复制元数据、追加容器并插入全局引用表。
bool SessionUtils::syncCreatedNoteToBeatmap(SessionContext&      ctx,
                                            const NoteComponent& note)
{
    // 草稿、折线和派生子实体有独立同步语义，不可当成普通正式物件追加。
    if ( !ctx.currentBeatmap || note.m_isDraft || note.m_isSubNote ||
         note.m_collaborationId.empty() ||
         (note.m_type != ::MMM::NoteType::NOTE &&
          note.m_type != ::MMM::NoteType::HOLD &&
          note.m_type != ::MMM::NoteType::FLICK) ) {
        return false;
    }

    // 写颜色元数据只作用于同步副本，不能在序列化过程中改写调用方组件。
    NoteComponent synchronized = note;
    // 完整复制保留格式扩展字段；不能仅从可见几何重新构造一个缺少绑定的物件。
    // 没有运行时颜色覆盖时保留原元数据，不在追加入口主动删除颜色扩展键。
    if ( hasAnyNoteColorOverride(synchronized.m_customColors) ) {
        writeNoteColorOverridesToMetadata(synchronized);
    }

    auto& mutex = EditorEngine::instance().getSessionMutex();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    ::MMM::Note*                          appended = nullptr;
    // 类型容器追加与统一索引插入受同一把锁保护，读者不能只看到其中一步。
    // 模型容器拥有新物件；appended 仅用于本函数后续建立非拥有的统一索引。
    // 类型容器使用稳定元素地址的存储，后续尾部追加不能使已发布的引用失效。
    // 若未来更换容器类型，必须同时检查 m_allNotes 和折线节点引用的稳定性。
    if ( synchronized.m_type == ::MMM::NoteType::NOTE ) {
        auto& target      = ctx.currentBeatmap->m_noteData.notes;
        auto& value       = target.emplace_back();
        value.m_type      = ::MMM::NoteType::NOTE;
        value.m_timestamp = synchronized.m_timestamp * 1000.0;
        // 模型使用毫秒与无符号正式轨号，草稿已在入口排除。
        value.m_track = static_cast<std::uint32_t>(synchronized.m_trackIndex);
        value.m_metadata        = synchronized.m_metadata;
        value.m_annotation      = synchronized.m_annotation;
        value.m_sampleBinding   = synchronized.m_sampleBinding;
        value.m_collaborationId = synchronized.m_collaborationId;
        appended                = &value;
    } else if ( synchronized.m_type == ::MMM::NoteType::HOLD ) {
        auto& target      = ctx.currentBeatmap->m_noteData.holds;
        auto& value       = target.emplace_back();
        value.m_type      = ::MMM::NoteType::HOLD;
        value.m_timestamp = synchronized.m_timestamp * 1000.0;
        value.m_track = static_cast<std::uint32_t>(synchronized.m_trackIndex);
        value.m_duration = synchronized.m_duration * 1000.0;
        // 保留持续时间而非预计算终点，以便模型继续执行其既有长条语义。
        value.m_metadata        = synchronized.m_metadata;
        value.m_annotation      = synchronized.m_annotation;
        value.m_sampleBinding   = synchronized.m_sampleBinding;
        value.m_collaborationId = synchronized.m_collaborationId;
        appended                = &value;
    } else {
        // 入口已经排除其他类型，剩余分支仅对应 FLICK；
        // 扩充入口允许类型时必须同步扩充分派，不能让新类型落入滑键容器。
        auto& target      = ctx.currentBeatmap->m_noteData.flicks;
        auto& value       = target.emplace_back();
        value.m_type      = ::MMM::NoteType::FLICK;
        value.m_timestamp = synchronized.m_timestamp * 1000.0;
        value.m_track  = static_cast<std::uint32_t>(synchronized.m_trackIndex);
        value.m_dtrack = synchronized.m_dtrack;
        value.m_metadata        = synchronized.m_metadata;
        value.m_annotation      = synchronized.m_annotation;
        value.m_sampleBinding   = synchronized.m_sampleBinding;
        value.m_collaborationId = synchronized.m_collaborationId;
        appended                = &value;
    }

    // 类型容器允许尾部追加，但统一引用表仍需按时间有序。
    // 使用 upper_bound 放在同时间组末尾，不重排已有同刻物件。
    // 插入的是模型元素引用而非同步副本；调用返回后局部 synchronized
    // 会销毁，不能让统一表引用它。已有统一表必须由调用方维持时间有序。
    const auto insertion = std::upper_bound(
        ctx.currentBeatmap->m_allNotes.begin(),
        ctx.currentBeatmap->m_allNotes.end(),
        appended->m_timestamp,
        [](double timestamp, const std::reference_wrapper<::MMM::Note>& value) {
            return timestamp < value.get().m_timestamp;
        });
    ctx.currentBeatmap->m_allNotes.insert(insertion, *appended);
    // 不清除 m_needsNotesSync：同一批编辑可能还有删除或移动等待完整回写，
    // 成功追加一个新物件并不能证明该类别的所有变更都已同步。
    return true;
}


/// @brief 按脏类别将 ECS 中的正式数据回写到当前谱面模型。
/// @param ctx 提供实体、类别同步标志和目标模型的会话上下文。
/// @pre 调用期间 ECS 不得并发修改，末尾的模型交换锁不保护此前的遍历。
/// @pre 当前谱面与类别脏标记也必须在整个构造期间稳定，不能仅在交换时同步。
/// @pre 正式组件的轨号可转换为模型无符号轨号，时间满足排序与格式转换要求。
/// @note 回写保留已有协作身份，不补发身份；新物件必须先经创建流程初始化。
/// @note 模型内容可能整体替换，外部不得跨同步保存旧模型元素的观察引用。
/// @note 仅替换标脏类别；独立批注记录及项目草稿不由本函数重建。
/// @note 同步到内存模型不等同于保存到磁盘，不在这里创建文件或发起网络提交。
/// @warning 低频同步路径包含完整扫描、排序和容器分配，不可无变更逐帧执行。
void SessionUtils::syncBeatmap(SessionContext& ctx)
{
    if ( !ctx.currentBeatmap ) return;
    // 无模型时保留原有脏标记，此入口不能替调用方决定是否丢弃待同步编辑状态。
    // 清洁会话直接返回，避免把同步入口调用频率变成全表重建频率。
    if ( !ctx.m_needsTimingsSync && !ctx.m_needsNotesSync &&
         !ctx.m_needsSamplesSync ) {
        return;
    }

    // 在独立容器中构造替代值，最后统一交换到模型，避免逐实体修改旧模型索引。
    // 不复制旧模型中已被 ECS 删除的实体，旧容器仅在完成交接前供已有模型读取。
    // 未标脏类别的临时容器会保持为空，但并不表示要清空该类模型数据；
    // 末尾的交换也必须受同一类别标志保护。
    std::vector<Timing>                       newTimings;
    std::vector<std::reference_wrapper<Note>> newAllNotes;
    NoteData                                  newNoteData;
    std::deque<AudioSampleEvent>              newAudioSamples;
    // 新旧模型在构造期并存，峰值内存包含两份类别数据；
    // 此设计用于低频回写，不适合逐帧用来生成只读显示快照。

    if ( ctx.m_needsTimingsSync ) {
        // 时间线组件按值快照，排序不会改变 ECS 的存储顺序或实体身份。
        // 元数据随快照一起复制，避免在生成结果时再次从实体表拼接属性。
        // 注册表顺序不是时间顺序，先排序才能为后续非 BPM 事件携带生效 BPM。
        auto tlView = ctx.timelineRegistry.view<TimelineComponent>();
        std::vector<TimelineComponent> sortedTLs;
        for ( auto entity : tlView ) {
            sortedTLs.push_back(tlView.get<TimelineComponent>(entity));
        }
        std::sort(sortedTLs.begin(),
                  sortedTLs.end(),
                  [](const auto& a, const auto& b) {
                      return a.m_timestamp < b.m_timestamp;
                  });

        // 比较器只约束时间先后，不额外定义同刻事件的优先级。
        // 下面沿排序结果逐项推进 BPM，不能把最后一个 BPM 回填给所有行。
        // 首事件之前使用规范化的偏好 BPM，没有可用偏好时采用统一默认值。
        double currentBPM = ::MMM::DEFAULT_NORMALIZED_BPM;
        if ( ctx.currentBeatmap &&
             ctx.currentBeatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
            // 非正偏好不进入规范化分支，保持上面的默认节奏基线。
            currentBPM = ::MMM::normalizeBpmValue(
                ctx.currentBeatmap->m_baseMapMetadata.preference_bpm);
        }
        for ( const auto& tc : sortedTLs ) {
            // 这里只转换运行期表示，不恢复旧 Timing 对象：
            // 新结果以当前组件为准，已删除的时间点自然不再出现在模型中。
            Timing timing;
            timing.m_timestamp             = tc.m_timestamp * 1000.0;
            timing.m_timingEffect          = tc.m_effect;
            timing.m_timingEffectParameter = tc.m_value;

            if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
                // 规范化只作用于待写模型，不能在保存过程中反向修改 ECS 数值。
                // 后续行使用此次规范化后的 BPM，避免继承一个不可用的原始参数。
                // BPM 行同时维护效果参数、BPM
                // 与毫秒拍长，避免模型字段互相矛盾。
                currentBPM = ::MMM::normalizeBpmValue(tc.m_value, currentBPM);
                timing.m_timingEffectParameter = currentBPM;
                timing.m_bpm                   = currentBPM;
                timing.m_beat_length           = 60000.0 / timing.m_bpm;
            } else {
                // 效果参数仍保留原值，不能将 SV、拍号等参数钳制到 BPM 范围。
                // 非 BPM 行保留当前生效节奏，beat_length
                // 按既有格式语义保存效果值。
                timing.m_bpm         = currentBPM;
                timing.m_beat_length = tc.m_value;
            }
            timing.m_metadata = tc.m_metadata;
            // 效果扩展元数据按值保留，不由 BPM 分支重新生成或筛掉未知键。
            newTimings.push_back(timing);
            // 只替换时间点列表，不将最后生效 BPM 写回谱面偏好 BPM。
        }
    }

    if ( ctx.m_needsNotesSync ) {
        // 完整类别同步重建全部正式音符，而非只追加发生变更的实体。
        // 这样删除操作也会反映到输出中，不需要用旧模型身份逐项求差集。
        auto noteView = ctx.noteRegistry.view<NoteComponent>();

        // 先重建独立正式物件，再从父折线重建其节点。
        // 两轮遍历分开处理表示方式，不能把派生子实体再当独立物件同步一次。
        // 这里只保留支持的具体类型，不以基类兜底序列化未知类型；
        // 新增物件类型时必须同时扩展此处与折线节点的分派。
        for ( auto entity : noteView ) {
            const auto& nc = noteView.get<NoteComponent>(entity);
            // 子实体稍后从父节点数组展开，草稿始终不进入正式谱面物件容器。
            if ( nc.m_isSubNote || nc.m_isDraft ) continue;
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) continue;

            // 只在存在运行时颜色覆盖时刷新同步副本的元数据。
            NoteComponent syncedNote = nc;
            if ( hasAnyNoteColorOverride(syncedNote.m_customColors) ) {
                writeNoteColorOverridesToMetadata(syncedNote);
            }

            if ( nc.m_type == ::MMM::NoteType::NOTE ) {
                // 值对象先进入拥有容器，再把容器内元素加入统一引用表。
                // 不能引用局部 n，后续迭代会结束它的生命周期。
                Note n;
                n.m_type       = ::MMM::NoteType::NOTE;
                n.m_timestamp  = syncedNote.m_timestamp * 1000.0;
                n.m_track      = static_cast<uint32_t>(syncedNote.m_trackIndex);
                n.m_metadata   = syncedNote.m_metadata;
                n.m_annotation = syncedNote.m_annotation;
                n.m_sampleBinding   = syncedNote.m_sampleBinding;
                n.m_collaborationId = syncedNote.m_collaborationId;
                newNoteData.notes.push_back(std::move(n));
                newAllNotes.push_back(newNoteData.notes.back());
            } else if ( nc.m_type == ::MMM::NoteType::HOLD ) {
                // 使用具体派生模型保留时长，不能把 Hold 切片为 Note 存储。
                // 即使时长为零也保持类型身份，以便纯长条往返保存。
                Hold h;
                h.m_type       = ::MMM::NoteType::HOLD;
                h.m_timestamp  = syncedNote.m_timestamp * 1000.0;
                h.m_track      = static_cast<uint32_t>(syncedNote.m_trackIndex);
                h.m_duration   = syncedNote.m_duration * 1000.0;
                h.m_metadata   = syncedNote.m_metadata;
                h.m_annotation = syncedNote.m_annotation;
                h.m_sampleBinding   = syncedNote.m_sampleBinding;
                h.m_collaborationId = syncedNote.m_collaborationId;
                newNoteData.holds.push_back(std::move(h));
                newAllNotes.push_back(newNoteData.holds.back());
            } else if ( nc.m_type == ::MMM::NoteType::FLICK ) {
                // 轨差按原符号回写，取绝对值会丢失滑动方向。
                Flick f;
                f.m_type       = ::MMM::NoteType::FLICK;
                f.m_timestamp  = syncedNote.m_timestamp * 1000.0;
                f.m_track      = static_cast<uint32_t>(syncedNote.m_trackIndex);
                f.m_dtrack     = syncedNote.m_dtrack;
                f.m_metadata   = syncedNote.m_metadata;
                f.m_annotation = syncedNote.m_annotation;
                f.m_sampleBinding   = syncedNote.m_sampleBinding;
                f.m_collaborationId = syncedNote.m_collaborationId;
                newNoteData.flicks.push_back(std::move(f));
                newAllNotes.push_back(newNoteData.flicks.back());
            }
        }

        // 折线内部引用必须指向本次新建的类型容器，不能沿用旧模型引用，
        // 否则末尾交换并释放旧模型后，父折线会留下失效的节点引用。
        for ( auto entity : noteView ) {
            const auto& nc = noteView.get<NoteComponent>(entity);
            if ( nc.m_isDraft || nc.m_type != ::MMM::NoteType::POLYLINE ) {
                continue;
            }

            NoteComponent syncedPolyline = nc;
            // 父组件副本连同节点数组一起复制，后续颜色序列化不污染编辑视图。
            // 父级颜色和节点颜色分别写入各自元数据，不能把父级缓存覆盖到所有节点。
            if ( hasAnyNoteColorOverride(syncedPolyline.m_customColors) ) {
                writeNoteColorOverridesToMetadata(syncedPolyline);
            }

            // 父折线和子物件分别存入模型容器，通过引用恢复原有拓扑。
            Polyline p;
            p.m_type       = ::MMM::NoteType::POLYLINE;
            p.m_timestamp  = syncedPolyline.m_timestamp * 1000.0;
            p.m_track      = static_cast<uint32_t>(syncedPolyline.m_trackIndex);
            p.m_metadata   = syncedPolyline.m_metadata;
            p.m_annotation = syncedPolyline.m_annotation;
            p.m_sampleBinding   = syncedPolyline.m_sampleBinding;
            p.m_collaborationId = syncedPolyline.m_collaborationId;
            // 折线父级不拥有一个用于替代节点时序的时长字段；
            // 各节点时间与长条时长在下方分别还原，不从首尾重新推算。

            // 保持父组件节点数组的顺序，统一时间索引的排序在最后单独进行。
            // 节点持有自身协作身份与绑定，不能用父级字段覆盖这些独立属性。
            for ( const auto& sub_note : syncedPolyline.m_subNotes ) {
                // 使用父组件内嵌值作为真源，不重新读取可能尚未刷新的派生子实体。
                NoteComponent::SubNote syncedSubNote = sub_note;
                // 回写节点的绝对时间，不叠加父时间；加载方向也没有减去父时间。
                // 节点显式采样保持原样，播放时采用的父绑定后备不持久化到节点。
                if ( hasAnyNoteColorOverride(syncedSubNote.customColors) ) {
                    writeNoteColorOverridesToMetadata(syncedSubNote);
                }

                if ( syncedSubNote.type == ::MMM::NoteType::NOTE ) {
                    Note n;
                    n.m_type      = ::MMM::NoteType::NOTE;
                    n.m_timestamp = syncedSubNote.timestamp * 1000.0;
                    n.m_track = static_cast<uint32_t>(syncedSubNote.trackIndex);
                    n.m_isSubNote = true;
                    // 子标记与父引用同时恢复，避免导出或再次载入时被当成独立音符。
                    n.m_metadata        = syncedSubNote.metadata;
                    n.m_annotation      = syncedSubNote.annotation;
                    n.m_sampleBinding   = syncedSubNote.sampleBinding;
                    n.m_collaborationId = syncedSubNote.collaborationId;
                    newNoteData.notes.push_back(std::move(n));
                    auto& ref = newNoteData.notes.back();
                    // 父节点列表与全局索引借用同一个拥有元素，不能各复制一个节点。
                    // 否则通过全局索引修改模型时，折线仍会看到另一份旧值。
                    p.m_subNotes.push_back(ref);
                    newAllNotes.push_back(ref);
                } else if ( syncedSubNote.type == ::MMM::NoteType::HOLD ) {
                    Hold h;
                    h.m_type      = ::MMM::NoteType::HOLD;
                    h.m_timestamp = syncedSubNote.timestamp * 1000.0;
                    h.m_track = static_cast<uint32_t>(syncedSubNote.trackIndex);
                    h.m_duration        = syncedSubNote.duration * 1000.0;
                    h.m_isSubNote       = true;
                    h.m_metadata        = syncedSubNote.metadata;
                    h.m_annotation      = syncedSubNote.annotation;
                    h.m_sampleBinding   = syncedSubNote.sampleBinding;
                    h.m_collaborationId = syncedSubNote.collaborationId;
                    newNoteData.holds.push_back(std::move(h));
                    auto& ref = newNoteData.holds.back();
                    p.m_subNotes.push_back(ref);
                    p.m_subHolds.push_back(ref);
                    // 专属长条集合引用同一个 Hold，不能另建副本分离时长修改。
                    // 长条同时加入通用节点序列和类型专属引用集合。
                    newAllNotes.push_back(ref);
                } else if ( syncedSubNote.type == ::MMM::NoteType::FLICK ) {
                    Flick f;
                    f.m_type      = ::MMM::NoteType::FLICK;
                    f.m_timestamp = syncedSubNote.timestamp * 1000.0;
                    f.m_track = static_cast<uint32_t>(syncedSubNote.trackIndex);
                    f.m_dtrack          = syncedSubNote.dtrack;
                    f.m_isSubNote       = true;
                    f.m_metadata        = syncedSubNote.metadata;
                    f.m_annotation      = syncedSubNote.annotation;
                    f.m_sampleBinding   = syncedSubNote.sampleBinding;
                    f.m_collaborationId = syncedSubNote.collaborationId;
                    newNoteData.flicks.push_back(std::move(f));
                    auto& ref = newNoteData.flicks.back();
                    p.m_subNotes.push_back(ref);
                    p.m_subFlicks.push_back(ref);
                    // 专属集合按折线遍历顺序追加，不从全局时间排序结果反向重建。
                    // 滑键专属集合不替代通用节点顺序，两者都必须保留。
                    newAllNotes.push_back(ref);
                }
            }

            // 统一索引同时收录父物件，便于模型侧访问完整对象集合；
            // 这不表示父折线也产生一次声音，播放事件由加载时的角色规则决定。
            newNoteData.polylines.push_back(std::move(p));
            // 统一索引引用容器内的父对象，不引用已经被移出的局部 p。
            // 移入父容器只转移引用集合；节点仍由前面的具体类型容器拥有。
            // 空节点数组也保留父物件，本入口不把空折线当作删除请求。
            newAllNotes.push_back(newNoteData.polylines.back());
        }

        // 统一表包括父折线和子节点，按时间排序但不改变父折线内部引用顺序。
        // 同刻对象不按轨道或类型二次排序，也不在同步时消除重叠物件。
        // 使用者若需稳定的同刻身份顺序，不能从这个仅按时间的比较器推断。
        std::sort(newAllNotes.begin(),
                  newAllNotes.end(),
                  [](const std::reference_wrapper<Note>& a,
                     const std::reference_wrapper<Note>& b) {
                      return a.get().m_timestamp < b.get().m_timestamp;
                  });
    }

    if ( ctx.m_needsSamplesSync ) {
        // 样本单独排序并转换，不混入玩家物件的类型容器或全局音符引用表。
        auto sampleView = ctx.sampleRegistry.view<const SampleComponent>();
        std::vector<const SampleComponent*> sortedSamples;
        // 这些指针不转移组件所有权，模型转换完成后无需逐项释放。
        sortedSamples.reserve(sampleView.size());
        // 预留数量来自当前视图，不使用上一次模型样本数；删除或导入后两者可不同。
        // 排序观察指针避免复制资源字符串等组件状态；这些指针只在本次
        // 同步中使用，因此遍历、排序到转换结束之前都不能改动样本 Registry。
        for ( auto entity : sampleView ) {
            sortedSamples.push_back(
                &sampleView.get<const SampleComponent>(entity));
        }
        // 同刻按轨号、资源偏移和资源身份继续排序，减少模型输出顺序的无谓变动。
        std::sort(sortedSamples.begin(),
                  sortedSamples.end(),
                  [](const SampleComponent* lhs, const SampleComponent* rhs) {
                      if ( lhs->m_timestamp != rhs->m_timestamp ) {
                          return lhs->m_timestamp < rhs->m_timestamp;
                      }
                      if ( lhs->m_track != rhs->m_track ) {
                          return lhs->m_track < rhs->m_track;
                      }
                      if ( lhs->m_offsetMs != rhs->m_offsetMs ) {
                          return lhs->m_offsetMs < rhs->m_offsetMs;
                      }
                      return lhs->m_audioResourceId < rhs->m_audioResourceId;
                  });
        // 排序不做合并：即使上述排序键相同，独立样本仍全部保留。
        // 音量、时长等未参与比较的属性照常复制，不能将排序键当成样本唯一标识。
        // 自动采样不根据当前资源能否解码过滤，资源缺失不应在保存时静默删事件。
        // 转换之后模型拥有值副本，不依赖临时指针数组的生命周期。
        for ( const auto* sample : sortedSamples ) {
            // 组件工厂负责秒与毫秒字段转换，保持与导入方向成对的规则。
            newAudioSamples.push_back(sample->toAudioSample());
        }
    }

    {
        // 交换阶段持会话锁，让目标模型的拥有容器与非拥有引用表一起更新。
        auto& mutex = EditorEngine::instance().getSessionMutex();
        std::lock_guard<std::recursive_mutex> lock(mutex);

        if ( ctx.m_needsTimingsSync ) {
            // 标脏且没有实体时交换空结果，表示用户已清空该类别而不是跳过同步。
            ctx.currentBeatmap->m_timings.swap(newTimings);
        }
        if ( ctx.m_needsNotesSync ) {
            // 所有新引用都指向新类型容器中的元素，必须一并交换完整容器集合。
            ctx.currentBeatmap->m_allNotes.swap(newAllNotes);
            // 模型对象本身保持不变，共享 BeatMap 的使用者仍观察同一个谱面实例。
            ctx.currentBeatmap->m_noteData.notes.swap(newNoteData.notes);
            ctx.currentBeatmap->m_noteData.holds.swap(newNoteData.holds);
            ctx.currentBeatmap->m_noteData.flicks.swap(newNoteData.flicks);
            ctx.currentBeatmap->m_noteData.polylines.swap(
                newNoteData.polylines);
            // 交换过程中短暂存在跨容器的引用配对，持锁读者只能在整组交换后观察。
            // 不得将这些交换拆成各自独立加锁，否则读者可能访问错误一代拥有容器。
        }
        if ( ctx.m_needsSamplesSync ) {
            // BGM 轨数由会话布局决定，不按此次样本最大轨号缩减，空轨也可保留。
            // 玩家轨数、封面等基础信息不属于样本类别同步，不在这里一并覆盖。
            // 音频样本回写时同步 BGM 轨数，未标脏时不改该元数据。
            ctx.currentBeatmap->m_audioSamples.swap(newAudioSamples);
            ctx.currentBeatmap->m_baseMapMetadata.bgm_track_count =
                std::max(0, ctx.bgmTrackCount);
        }
    }

    // swap 后旧模型数据留在局部容器，离开函数时释放；
    // 新模型引用随拥有容器一起交接，不能对临时容器额外执行逐元素搬移整理。
    // 旧数据析构留在交换锁之外，缩短持锁时执行的容器清理工作。
    // 这里没有替换 ECS 实体，因此会话选择与撤销记录不应在同步时重置。
    // 任何类别回写都可能影响音频时间线或内容长度，推迟到描述消费入口重建。
    ctx.isAudioTimelineDescriptorDirty = true;
    // 标志只在模型交换结束后清除，下一次清洁调用无需重复构造。
    // 这里只请求重新计算描述，不直接激活音频或重置正在使用的播放游标。
    ctx.m_needsTimingsSync = false;
    ctx.m_needsNotesSync   = false;
    ctx.m_needsSamplesSync = false;
}

}  // namespace MMM::Logic
