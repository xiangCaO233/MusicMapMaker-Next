#include "logic/ecs/system/NoteRenderSystem.h"

#include "common/render/CanvasRenderTypes.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace
{
/// @brief 测试主画布宽度。
constexpr float VIEWPORT_WIDTH = 800.0F;
/// @brief 测试主画布高度。
constexpr float VIEWPORT_HEIGHT = 600.0F;
/// @brief 玩家轨道数量。
constexpr std::int32_t PLAYER_TRACK_COUNT = 4;

/// @brief 比较两个画布坐标是否足够接近。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 误差小于测试容差时返回 true。
/// @note 使用绝对像素容差，适用于本文件固定大小的测试视口。
/// @note 不比较颜色或实体身份，也不作为生产拾取阈值。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 从指定绘制指令中提取 Note 纹理几何的最小横坐标。
/// @param snapshot 待检查快照。
/// @param commands 基础层或发光层绘制指令。
/// @param minX 成功时写入最小横坐标。
/// @return 找到 Note 纹理顶点时返回 true。
/// @note 沿指定层的索引读取顶点，避免混入另一层或未提交的几何。
/// @note 输出值只在返回 true 时有效，空层不能当作位于零点的物件。
/// @note 越界索引跳过，助手不负责验证整个快照的索引完整性。
/// @note Note UV 区域与本文件单键夹具配套，不适用于跨区域夹具的小图块。
/// @note 同一个顶点被多个索引引用不会改变极值结果。
/// @note 命令为空或只有非 Note 纹理时，返回 false 而不发布坐标。
bool findMinimumNoteX(
    const MMM::Logic::RenderSnapshot&                      snapshot,
    const std::vector<MMM::Common::Render::CanvasDrawCmd>& commands,
    float&                                                 minX)
{
    // 用极值初始化聚合，避免正坐标几何的最小值错误停留在零。
    // found 单独标识是否命中，调用方不能把未更新极值当作有效范围。
    minX       = std::numeric_limits<float>::max();
    bool found = false;
    for ( const auto& command : commands ) {
        // 命令索引范围与顶点范围不同，先在索引数组上限定本批访问。
        // 将起始偏移扩展为 size_t 后相加，保持与容器下标类型一致。
        const std::size_t end =
            static_cast<std::size_t>(command.indexOffset) + command.indexCount;
        for ( std::size_t index = command.indexOffset;
              index < end && index < snapshot.indices.size();
              ++index ) {
            // 每条命令可以引用不同顶点基址，局部索引必须叠加 vertexOffset。
            // 直接拿 indices 的值访问可能读到另一批背景或发光顶点。
            const std::size_t vertexIndex =
                static_cast<std::size_t>(snapshot.indices[index]) +
                command.vertexOffset;
            // 对意外越界保持测试助手可诊断，不在提取范围时崩溃。
            // 该跳过行为不证明快照索引合法，结构完整性不由此助手覆盖。
            if ( vertexIndex >= snapshot.vertices.size() ) continue;
            const auto& vertex = snapshot.vertices[vertexIndex];
            // 测试 UV 图中只有 Note 使用该区间，可排除轨道背景和调试几何。
            if ( vertex.uv.u < 0.24F || vertex.uv.u > 0.46F ||
                 vertex.uv.v < 0.34F || vertex.uv.v > 0.46F ) {
                continue;
            }
            // 三角形索引会重复引用四边形角点，极值聚合无需去重。
            // 只读计算不修改快照，也不改变实际绘制顺序。
            minX  = std::min(minX, vertex.pos.x);
            found = true;
        }
    }
    // 存在至少一个匹配顶点才允许调用方比较坐标。
    // 缺少整个绘制层时返回 false，防止空范围误通过。
    return found;
}

/// @brief 从指定绘制层提取一个测试纹理的横向顶点范围。
/// @param snapshot 待检查渲染快照。
/// @param commands 基础层或发光层绘制指令。
/// @param textureU 测试纹理的起始 U 坐标。
/// @param minX 成功时写入最小横坐标。
/// @param maxX 成功时写入最大横坐标。
/// @return 找到目标纹理顶点时返回 true。
/// @note 要求各测试纹理的 U 区域互不重叠，V 坐标不参与本助手筛选。
/// @note 范围是该层所有目标纹理顶点的并集，不区分同纹理的多个部件。
/// @note 基础层与发光层应分别调用，不能只比较整个顶点数组。
/// @note 命令 vertexOffset 是索引基址，必须与局部索引相加后访问顶点。
/// @note 测试纹理宽度由夹具固定为 0.01，本助手不是通用图集查询器。
/// @note 输出不包含垂直范围，不能据此断言时间映射正确。
bool findTextureXRange(
    const MMM::Logic::RenderSnapshot&                      snapshot,
    const std::vector<MMM::Common::Render::CanvasDrawCmd>& commands,
    float textureU, float& minX, float& maxX)
{
    // 用极值初始化聚合，避免正坐标几何的最小值错误停留在零。
    // found 单独标识是否命中，调用方不能把未更新极值当作有效范围。
    minX       = std::numeric_limits<float>::max();
    maxX       = std::numeric_limits<float>::lowest();
    bool found = false;
    for ( const auto& command : commands ) {
        // 命令索引范围与顶点范围不同，先在索引数组上限定本批访问。
        // 将起始偏移扩展为 size_t 后相加，保持与容器下标类型一致。
        const std::size_t end =
            static_cast<std::size_t>(command.indexOffset) + command.indexCount;
        for ( std::size_t index = command.indexOffset;
              index < end && index < snapshot.indices.size();
              ++index ) {
            // 每条命令可以引用不同顶点基址，局部索引必须叠加 vertexOffset。
            // 直接拿 indices 的值访问可能读到另一批背景或发光顶点。
            const std::size_t vertexIndex =
                static_cast<std::size_t>(snapshot.indices[index]) +
                command.vertexOffset;
            // 对意外越界保持测试助手可诊断，不在提取范围时崩溃。
            // 该跳过行为不证明快照索引合法，结构完整性不由此助手覆盖。
            if ( vertexIndex >= snapshot.vertices.size() ) continue;
            const auto& vertex = snapshot.vertices[vertexIndex];
            // 夹具图块宽度固定为 0.01，右侧少量容差覆盖浮点边界。
            // 各图块 U 间隔远大于容差，其他纹理不能进入目标范围。
            if ( vertex.uv.u < textureU - 1e-4F ||
                 vertex.uv.u > textureU + 0.011F ) {
                continue;
            }
            // 三角形索引会重复引用四边形角点，极值聚合无需去重。
            // 只读计算不修改快照，也不改变实际绘制顺序。
            minX  = std::min(minX, vertex.pos.x);
            maxX  = std::max(maxX, vertex.pos.x);
            found = true;
        }
    }
    // 存在至少一个匹配顶点才允许调用方比较坐标。
    // 缺少整个绘制层时返回 false，防止空范围误通过。
    return found;
}

/// @brief 为跨 Draft/Player 边界的单个拖动物件生成主画布快照。
/// @param note 待渲染 Flick 或 Polyline。
/// @param snapshot 输出快照。
/// @param projection 输出统一轨道投影。
/// @param noteEntity 输出物件实体。
/// @param brush 可选笔刷预览状态；为空时仅渲染实体。
/// @param includeBgm 是否启用一条独立 BGM 轨道。
/// @warning 测试夹具只在单线程 CTest 中运行；局部 Registry
/// 在快照生成后即可释放。
/// @note note 按值接收并移入 Registry，调用方不应依赖被移动实参的内容。
/// @note 输出实体只用于比对快照中的命中身份，不能在夹具返回后访问局部
/// Registry。
/// @note 草稿区和 BGM 区宽度故意不同于玩家区，以暴露沿用根轨宽度的问题。
/// @note 画笔状态在生成前复制进快照，不会注册为正式 Note。
/// @note 输入快照应为空，夹具只注入本次所需状态，不主动清除旧输出。
/// @note 生成和期望投影使用相同区域开关，测试断言另以固定坐标检验端点几何。
/// @note 没有启动逻辑循环或渲染线程，夹具同步完成全部几何生成。
/// @note 快照中的本地实体号只作为值标识，不延长 Registry 生命周期。
void renderCrossRegionGhost(
    MMM::Logic::NoteComponent note, MMM::Logic::RenderSnapshot& snapshot,
    MMM::Logic::CanvasLaneProjection& projection, entt::entity& noteEntity,
    const MMM::Logic::RenderSnapshot::BrushSnapshot* brush      = nullptr,
    bool                                             includeBgm = false)
{
    // 三个对象域分开构造，保持主渲染入口的真实参数结构。
    // 样本 Registry 为空，启用 BGM 布局也不自动生成音频物件。
    entt::registry noteRegistry;
    entt::registry sampleRegistry;
    entt::registry timelineRegistry;

    // 固定 BPM 与判定线，确保所有端点同时处于可见和可拾取范围。
    const auto bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    // 玩家区固定为 80 至 400 像素，单轨宽 80 像素。
    // 草稿和 BGM 另给位置与宽度，使跨区端点无法用同一线性轨距替代。
    config.visual.trackLayout.left             = 0.1F;
    config.visual.trackLayout.right            = 0.5F;
    config.visual.trackLayout.draftLanes.left  = -0.21F;
    config.visual.trackLayout.draftLanes.width = 0.06F;
    config.visual.trackLayout.bgmLanes.left    = 0.7F;
    config.visual.trackLayout.bgmLanes.width   = 0.05F;
    // 横向缩放固定为八成，使节点边界与轨道中心明确分开。
    // 独立轨宽应同时作用于本体、箭头和命中宽度。
    config.visual.noteScaleX = 0.8F;
    config.visual.noteScaleY = 1.0F;
    // 关闭节拍和 Timing 辅助线，缩小测试中非目标纹理的干扰。
    // 仍走完整快照入口，不直接调用内部几何函数构造期望结果。
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.visual.previewConfig.drawBeatLines   = false;
    config.visual.previewConfig.drawTimingLines = false;
    // 专业模式显示草稿区域，BMS 开关控制独立 BGM 区是否参与。
    // 两种区域开关必须传递到期望投影，不能只设置可见轨数量。
    config.settings.professionalMode = true;
    config.settings.enableBmsEditing = includeBgm;

    // 滚动缓存放在 Timeline 的上下文中，由正常快照路径借用。
    // 局部缓存与 Registry 同寿命，生成期间不会留下悬空观察地址。
    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);

    // 父物件的草稿属性取决于根轨，终点可以跨到玩家或 BGM 区。
    // 不能因为根属于草稿，就把所有子项或 Flick 箭头按草稿宽度绘制。
    note.m_isDraft = note.m_trackIndex < 0;
    noteEntity     = noteRegistry.create();
    noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity,
                                                    std::move(note));
    noteRegistry.emplace<MMM::Logic::TransformComponent>(noteEntity);
    // 同时选中并拖动，以覆盖拖动虚影及其发光生成路径。
    // 身份在夹具内固定，后面用输出实体过滤命中框。
    noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity,
        MMM::Logic::InteractionComponent{
            .isSelected = true,
            .isDragging = true,
        });
    // 单物件索引自然有序，通过观察指针向渲染系统提供候选。
    // 索引在 generateSnapshot 返回之前保持存活，不把它放入跨线程快照。
    const std::vector<entt::entity> sortedNotes{ noteEntity };
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);

    // 开启谱面和交互状态，确保命中数据不会被后台会话条件跳过。
    // 若提供笔刷，先复制其值，避免生成器借用调用方的临时对象。
    snapshot.hasBeatmap         = true;
    snapshot.acceptsInteraction = true;
    if ( brush ) snapshot.brush = *brush;
    // 按纹理类别分配独立 U 区间，测试据此识别节点、身体和箭头。
    // 图块只需内存坐标，不需要真正加载图像或创建 GPU 纹理。
    /// @brief 为一个部件注入可区分的测试图块。
    /// @param id 渲染器请求的纹理标识。
    /// @param u 该部件独有的图块横向起点。
    const auto addTexture = [&](MMM::Logic::TextureID id, float u) {
        snapshot.uvMap.emplace(static_cast<std::uint32_t>(id),
                               glm::vec4{ u, 0.2F, 0.01F, 0.01F });
    };
    addTexture(MMM::Logic::TextureID::None, 0.0F);
    addTexture(MMM::Logic::TextureID::Note, 0.2F);
    // 横向身体与斜向过渡使用分离图块，使范围检查能独立归因。
    // Node 与 HoldEnd 也独立，避免一个终点缺失被另一种节点掩盖。
    addTexture(MMM::Logic::TextureID::HoldBodyHorizontal, 0.4F);
    addTexture(MMM::Logic::TextureID::HoldBodyVertical, 0.5F);
    addTexture(MMM::Logic::TextureID::Node, 0.6F);
    addTexture(MMM::Logic::TextureID::HoldEnd, 0.7F);
    // 左右箭头都注入，夹具不靠缺少某个纹理强迫方向分支。
    // 当前案例朝右移动，通过右箭头图块验证对应几何。
    addTexture(MMM::Logic::TextureID::FlickArrowRight, 0.8F);
    addTexture(MMM::Logic::TextureID::FlickArrowLeft, 0.9F);

    // 相机使用主画布 ID，以走跨区域轨道投影和拾取分支。
    // 传入相同视口与判定线，所有测试坐标均为画布局部像素。
    MMM::Logic::System::NoteRenderSystem::generateSnapshot(
        noteRegistry,
        sampleRegistry,
        {},
        {},
        timelineRegistry,
        {},
        &snapshot,
        "Basic2DCanvas",
        0.0,
        VIEWPORT_WIDTH,
        VIEWPORT_HEIGHT,
        VIEWPORT_HEIGHT * config.visual.judgeline_pos,
        PLAYER_TRACK_COUNT,
        includeBgm ? 1 : 0,
        PLAYER_TRACK_COUNT,
        config,
        VIEWPORT_HEIGHT);
    // 输出投影用于确认区域存在和解释预期几何。
    // 渲染结果仍从快照命令提取，不把投影对象本身作为测试结果。
    projection =
        MMM::Logic::calculateCanvasLaneProjection(VIEWPORT_WIDTH,
                                                  PLAYER_TRACK_COUNT,
                                                  includeBgm ? 1 : 0,
                                                  config.visual.trackLayout,
                                                  0.0F,
                                                  true,
                                                  includeBgm,
                                                  true,
                                                  PLAYER_TRACK_COUNT,
                                                  true);
}

/// @brief 验证跨入 BGM 区的拖动单键在基础层、发光层与命中盒中共用统一投影。
/// @return 虚影避开批注沟槽并落在第一条 BGM 轨道时返回 true。
/// @note 模拟拖动尚未提交时的状态：实体仍属于 Note Registry，轨号已进入 BGM
/// 区。
/// @note 基础层、发光层和头部命中框必须同时落到独立 BGM 布局。
/// @note 批注沟槽与 BGM 区之间的间隔不能按玩家连续轨宽推算。
/// @note 本测试不执行拖动命令，也不验证提交后 Note 转换为采样的过程。
/// @note 只校验头部左边界，不单独证明完整命中矩形的纵向尺寸。
/// @note BGM 轨宽通过本体居中公式进入预期位置，不能替换为玩家轨宽。
bool testDraggedTapUsesBgmLaneBounds()
{
    entt::registry noteRegistry;
    entt::registry sampleRegistry;
    entt::registry timelineRegistry;

    // 固定 BPM 让音符在判定线附近稳定生成渲染与命中数据。
    const auto bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    config.visual.trackLayout.left  = 0.1F;
    config.visual.trackLayout.right = 0.5F;
    // 首条 BGM 轨从 560 像素开始，宽 40 像素。
    // 它与玩家区右边界存在明显空隙，可发现虚影落入批注沟槽的问题。
    config.visual.trackLayout.bgmLanes.left  = 0.7F;
    config.visual.trackLayout.bgmLanes.width = 0.05F;
    // 固定缩放避免用户默认值变化影响本例的几何定位。
    // 测试目标是 BGM 投影选择，不是配置默认值。
    config.visual.noteScaleX = 0.8F;
    config.visual.noteScaleY = 1.0F;
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.visual.previewConfig.drawBeatLines   = false;
    config.visual.previewConfig.drawTimingLines = false;
    config.settings.enableBmsEditing            = true;

    // 提供正常时间映射，零秒物件应落在可见判定线附近。
    // 不手工写 Transform 屏幕位置，让快照生成器计算实际位置。
    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);

    // 拖动更新期间普通 Tap 仍位于 Note registry，但轨道已编码为首条 BGM
    // 绝对轨。
    const auto noteEntity = noteRegistry.create();
    noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::NOTE,
            .m_timestamp  = 0.0,
            .m_trackIndex = PLAYER_TRACK_COUNT,
        });
    // 提供生产渲染视图要求的 Transform 与交互组件。
    // 仅有 NoteComponent 可能无法进入同一实体渲染分支。
    noteRegistry.emplace<MMM::Logic::TransformComponent>(noteEntity);
    noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity,
        MMM::Logic::InteractionComponent{
            .isSelected = true,
            .isDragging = true,
        });
    // 通过正式的排序索引入口提供拖动物件。
    // 单实体无需额外排序，不引入与本例无关的索引维护逻辑。
    const std::vector<entt::entity> sortedNotes{ noteEntity };
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);

    MMM::Logic::RenderSnapshot snapshot;
    snapshot.hasBeatmap = true;
    // 命中框是本例必检输出，因此显式允许交互。
    // 不能用禁止拾取的后台快照测试头部位置。
    snapshot.acceptsInteraction = true;
    // 纯色背景与 Note 图块互不重叠，便于按 UV 排除装饰几何。
    // 这里的 Note 图块与 findMinimumNoteX 的筛选区间配套。
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note),
        glm::vec4{ 0.25F, 0.35F, 0.2F, 0.1F });

    // 通过公开快照入口覆盖拖动虚影与拾取的共同调用链。
    // 样本列表为空，确保检测到的 Note 几何不是另一个自动采样。
    MMM::Logic::System::NoteRenderSystem::generateSnapshot(
        noteRegistry,
        sampleRegistry,
        {},
        {},
        timelineRegistry,
        {},
        &snapshot,
        "Basic2DCanvas",
        0.0,
        VIEWPORT_WIDTH,
        VIEWPORT_HEIGHT,
        VIEWPORT_HEIGHT * config.visual.judgeline_pos,
        PLAYER_TRACK_COUNT,
        1,
        PLAYER_TRACK_COUNT,
        config,
        VIEWPORT_HEIGHT);

    // 期望布局显式打开 BGM 区，草稿区在这个单键案例中关闭。
    // 轨道编号与区域序号不同：统一轨号四对应 BGM 区内序号零。
    const auto projection =
        MMM::Logic::calculateCanvasLaneProjection(VIEWPORT_WIDTH,
                                                  PLAYER_TRACK_COUNT,
                                                  1,
                                                  config.visual.trackLayout,
                                                  0.0F,
                                                  true,
                                                  true,
                                                  false,
                                                  PLAYER_TRACK_COUNT,
                                                  true);
    const auto bgmBounds =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Bgm, 0U });
    // 缺少 BGM 边界说明测试布局未建立，不能继续用默认坐标断言。
    // 提前失败也避免对空 optional 解引用。
    if ( !bgmBounds ) {
        XERROR("Dragged Note ghost test could not resolve the first BGM lane");
        return false;
    }

    // 本体以独立 BGM 轨宽缩放，再在该轨内居中。
    // 使用玩家轨宽会同时造成宽度和左边界错误。
    const float noteWidth = projection.bgmLaneWidth * config.visual.noteScaleX;
    const float expectedHeadX =
        bgmBounds->leftX + (projection.bgmLaneWidth - noteWidth) * 0.5F;

    // 分别从基础与发光命令提取位置，验证两层共用同一投影。
    // 只扫描整个顶点数组会把两个层合并，可能掩盖其中一层仍在旧位置。
    float baseMinX = 0.0F;
    float glowMinX = 0.0F;
    if ( !findMinimumNoteX(snapshot, snapshot.cmds, baseMinX) ||
         !near(baseMinX, expectedHeadX) ||
         !findMinimumNoteX(snapshot, snapshot.glowCmds, glowMinX) ||
         !near(glowMinX, expectedHeadX) ) {
        XERROR(
            "Dragged Note body/glow X mismatch: base={}, glow={}, expected={}",
            baseMinX,
            glowMinX,
            expectedHeadX);
        return false;
    }

    // 只检查该实体头部命中框，忽略其他部件或背景拾取数据。
    // 拾取位置必须与刚才验证的可见几何一致。
    for ( const auto& hitbox : snapshot.hitboxes ) {
        if ( hitbox.entity != noteEntity ||
             hitbox.part != MMM::Logic::HoverPart::Head ) {
            continue;
        }
        // 除了具体坐标，还确认头部不落在批注区域左侧。
        // 这直接约束拖动时视觉与交互共同避开沟槽的回归场景。
        if ( !near(hitbox.x, expectedHeadX) ||
             hitbox.x < projection.annotationRightX ) {
            XERROR(
                "Dragged Note ghost remained in the annotation gutter: {} != "
                "{}",
                hitbox.x,
                expectedHeadX);
            return false;
        }
        return true;
    }

    // 没有头部命中框属于失败，不能因循环未遇到候选而空通过。
    // 日志与坐标错误分开，区分交互数据缺失和投影错误。
    XERROR("Dragged Note ghost did not publish a head hitbox");
    return false;
}

/// @brief 验证 Draft 根节点指向 Player 终点的 Flick 使用两侧真实轨道几何。
/// @return 箭头、连接体、发光与命中盒均跨越独立间隙时返回 true。
/// @note 统一轨号从草稿 -1 到玩家 0 相差一，但屏幕距离不是草稿单轨宽。
/// @note 连接体按两侧中心连接，箭头宽度取终点玩家轨。
/// @note 命中框与两类绘制层共同约束，避免只修复拾取或仅修复可见层。
/// @note 检查横向范围，不覆盖 Flick 的时间吸附或纵向布局。
bool testCrossRegionFlickUsesEndpointProjection()
{
    // 用普通 Flick 组件模拟未提交的跨区域拖动状态。
    // 轨差保持统一轨号的差值，屏幕端点必须由各自区域解析。
    MMM::Logic::NoteComponent flick{
        .m_type       = MMM::NoteType::FLICK,
        .m_timestamp  = 0.0,
        .m_trackIndex = -1,
        .m_dtrack     = 1,
    };
    MMM::Logic::RenderSnapshot       snapshot;
    MMM::Logic::CanvasLaneProjection projection;
    entt::entity                     entity{ entt::null };
    renderCrossRegionGhost(std::move(flick), snapshot, projection, entity);

    // 统一轨号 -1 对应草稿投影中的末端轨，包含前置追加轨偏移。
    // 玩家终点使用区内序号零，不能直接拿负轨号查询玩家边界。
    const auto rootLane =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Draft, 4U });
    const auto endpointLane =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Player, 0U });
    // 两侧区域必须都能解析，即使后续断言使用固定像素也不能省略该前提。
    // 布局开关错误应报告区域缺失，而不误归因于端点绘制。
    if ( !rootLane || !endpointLane ) {
        XERROR(
            "Cross-region Flick projection did not expose both endpoint lanes");
        return false;
    }

    // 草稿末轨中心为 48，玩家首轨中心为 120，连接体跨度为 72 像素。
    // 箭头使用玩家轨 80 像素宽的八成，居中后落在 88 至 152。
    constexpr float expectedBodyX   = 48.0F;
    constexpr float expectedBodyW   = 72.0F;
    constexpr float expectedArrowX  = 88.0F;
    constexpr float expectedArrowX2 = 152.0F;
    // 身体和箭头是不同拾取部件，分别保存检查结果。
    // 两者都必须出现，不能只有完整的外观却缺少端点手柄。
    bool foundBody  = false;
    bool foundArrow = false;
    // 实体身份先过滤，再按部件类型验证相应矩形。
    // 不依赖命中框排列顺序，避免渲染批次调整导致无关失败。
    for ( const auto& hitbox : snapshot.hitboxes ) {
        if ( hitbox.entity != entity ) continue;
        // Flick 连接体沿用 HoldBody 部件类型，这是交互协议而非物件类型。
        // 按 FLICK 枚举过滤命中框会漏掉可拖动的身体区域。
        if ( hitbox.part == MMM::Logic::HoverPart::HoldBody ) {
            foundBody =
                near(hitbox.x, expectedBodyX) && near(hitbox.w, expectedBodyW);
        } else if ( hitbox.part == MMM::Logic::HoverPart::FlickArrow ) {
            foundArrow = near(hitbox.x, expectedArrowX) &&
                         near(hitbox.w, expectedArrowX2 - expectedArrowX);
        }
    }

    // 各层分别保留最小值与最大值，避免一个层的查询覆盖另一个层。
    // 查找失败时初值不具备坐标含义，必须配合返回标志判断。
    float baseArrowMin = 0.0F;
    float baseArrowMax = 0.0F;
    float glowArrowMin = 0.0F;
    float glowArrowMax = 0.0F;
    float baseBodyMin  = 0.0F;
    float baseBodyMax  = 0.0F;
    float glowBodyMin  = 0.0F;
    float glowBodyMax  = 0.0F;
    // 用独立纹理提取身体与箭头，分别检查基础层和发光层。
    // 查找返回值也参与最终断言，缺少整块纹理不能只靠初始零值比较。
    const bool baseArrow = findTextureXRange(
        snapshot, snapshot.cmds, 0.8F, baseArrowMin, baseArrowMax);
    // 发光箭头也需使用终点轨宽，不能仅随本体平移固定偏移。
    // 分别查询可发现只有选中状态下出现的端点错位。
    const bool glowArrow = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.8F, glowArrowMin, glowArrowMax);
    const bool baseBody = findTextureXRange(
        snapshot, snapshot.cmds, 0.4F, baseBodyMin, baseBodyMax);
    const bool glowBody = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.4F, glowBodyMin, glowBodyMax);
    // 固定像素期望独立于目标物件的渲染顶点，能识别共用错误投影。
    // 基础层与发光层都应覆盖同一横向跨度，不能只保证它们彼此相等。
    if ( !foundBody || !foundArrow || !baseArrow || !glowArrow || !baseBody ||
         !glowBody || !near(baseArrowMin, expectedArrowX) ||
         !near(baseArrowMax, expectedArrowX2) ||
         !near(glowArrowMin, expectedArrowX) ||
         !near(glowArrowMax, expectedArrowX2) ||
         !near(baseBodyMin, expectedBodyX) ||
         !near(baseBodyMax, expectedBodyX + expectedBodyW) ||
         !near(glowBodyMin, expectedBodyX) ||
         !near(glowBodyMax, expectedBodyX + expectedBodyW) ) {
        XERROR(
            "Cross-region Flick endpoint geometry diverged from Player lane");
        return false;
    }
    return true;
}

/// @brief 验证 Player 根节点指向独立 BGM 轨道的 Flick 终点投影。
/// @return 箭头按 BGM 轨宽缩放且连接体跨越批注间隙时返回 true。
/// @note 根节点在最后一条玩家轨，终点在第一条独立 BGM 轨。
/// @note 较窄的 BGM 轨决定箭头宽度，玩家区到 BGM 区的空隙决定连接长度。
/// @note 这里仍渲染拖动中的 Flick 组件，不创建实际音频采样。
/// @note 统一轨号差一只表达相邻逻辑轨道，不代表屏幕坐标连续。
/// @note 固定 BGM 区域位置使连接体跨距明显大于玩家单轨宽。
bool testPlayerToBgmFlickUsesEndpointProjection()
{
    // 用普通 Flick 组件模拟未提交的跨区域拖动状态。
    // 轨差保持统一轨号的差值，屏幕端点必须由各自区域解析。
    MMM::Logic::NoteComponent flick{
        .m_type       = MMM::NoteType::FLICK,
        .m_timestamp  = 0.0,
        .m_trackIndex = 3,
        .m_dtrack     = 1,
    };
    MMM::Logic::RenderSnapshot       snapshot;
    MMM::Logic::CanvasLaneProjection projection;
    entt::entity                     entity{ entt::null };
    renderCrossRegionGhost(
        std::move(flick), snapshot, projection, entity, nullptr, true);

    // 最后一条玩家轨中心为 360，首 BGM 轨中心为 580。
    // BGM 箭头宽 40×0.8=32，因此边界为 564 至 596。
    constexpr float expectedBodyX   = 360.0F;
    constexpr float expectedBodyMax = 580.0F;
    constexpr float expectedArrowX  = 564.0F;
    constexpr float expectedArrowX2 = 596.0F;
    // 身体和箭头是不同拾取部件，分别保存检查结果。
    // 两者都必须出现，不能只有完整的外观却缺少端点手柄。
    bool foundBody  = false;
    bool foundArrow = false;
    // 实体身份先过滤，再按部件类型验证相应矩形。
    // 不依赖命中框排列顺序，避免渲染批次调整导致无关失败。
    for ( const auto& hitbox : snapshot.hitboxes ) {
        if ( hitbox.entity != entity ) continue;
        // 身体包围盒覆盖两个中心之间的整段空隙，方便拖动时连续拾取。
        // 箭头使用独立部件，宽度不能扩张为整段连接长度。
        if ( hitbox.part == MMM::Logic::HoverPart::HoldBody ) {
            foundBody = near(hitbox.x, expectedBodyX) &&
                        near(hitbox.w, expectedBodyMax - expectedBodyX);
        } else if ( hitbox.part == MMM::Logic::HoverPart::FlickArrow ) {
            foundArrow = near(hitbox.x, expectedArrowX) &&
                         near(hitbox.w, expectedArrowX2 - expectedArrowX);
        }
    }

    // 连接体和箭头在本例中具有不同范围，不能共用一对输出变量。
    // 两层都分别保存，便于识别某一层未应用 BGM 投影。
    float baseBodyMin  = 0.0F;
    float baseBodyMax  = 0.0F;
    float glowBodyMin  = 0.0F;
    float glowBodyMax  = 0.0F;
    float baseArrowMin = 0.0F;
    float baseArrowMax = 0.0F;
    float glowArrowMin = 0.0F;
    float glowArrowMax = 0.0F;
    // 用独立纹理提取身体与箭头，分别检查基础层和发光层。
    // 查找返回值也参与最终断言，缺少整块纹理不能只靠初始零值比较。
    const bool baseBody = findTextureXRange(
        snapshot, snapshot.cmds, 0.4F, baseBodyMin, baseBodyMax);
    // 发光身体应连续跨越玩家区和 BGM 区之间的空隙。
    // 沿用玩家单轨跨度会得到明显过短的范围。
    const bool glowBody = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.4F, glowBodyMin, glowBodyMax);
    const bool baseArrow = findTextureXRange(
        snapshot, snapshot.cmds, 0.8F, baseArrowMin, baseArrowMax);
    const bool glowArrow = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.8F, glowArrowMin, glowArrowMax);
    // 连接体需跨越区域空隙，箭头则按较窄的 BGM 轨尺寸生成。
    // 同时检查各层两端，避免只改平移量却仍使用玩家宽度。
    if ( !foundBody || !foundArrow || !baseBody || !glowBody || !baseArrow ||
         !glowArrow || !near(baseBodyMin, expectedBodyX) ||
         !near(baseBodyMax, expectedBodyMax) ||
         !near(glowBodyMin, expectedBodyX) ||
         !near(glowBodyMax, expectedBodyMax) ||
         !near(baseArrowMin, expectedArrowX) ||
         !near(baseArrowMax, expectedArrowX2) ||
         !near(glowArrowMin, expectedArrowX) ||
         !near(glowArrowMax, expectedArrowX2) ) {
        XERROR("Player-to-BGM Flick endpoint ignored independent BGM geometry");
        return false;
    }
    return true;
}

/// @brief 验证跨 Draft/Player 的 Polyline 节点与过渡段逐端点投影。
/// @return 两侧节点宽度及斜向连接包围盒均匹配真实轨道时返回 true。
/// @note 三个子项跨越两个区域，并让最后一个子项包含横向 Flick。
/// @note 父实体身份配合 subIndex 定位具体节点或连接段。
/// @note 同纹理顶点范围是多个节点的并集，单节点尺寸另由命中框验证。
/// @note 生成一次快照即检查基础和发光两层，无需窗口或 GPU 读回。
/// @note 本例检查几何与命中范围，不执行折线编辑或子实体重建。
/// @note 所有子项时间都靠近零点，避免可见性剔除干扰跨区投影验证。
bool testCrossRegionPolylineUsesPerNodeProjection()
{
    MMM::Logic::NoteComponent polyline{
        .m_type       = MMM::NoteType::POLYLINE,
        .m_timestamp  = 0.0,
        .m_trackIndex = -1,
    };
    // 时间递增构成斜向过渡，轨号则在草稿与玩家之间往返。
    // 最后一项是跨区 Flick，用来覆盖折线内部的独立端点投影。
    polyline.m_subNotes = {
        MMM::Logic::NoteComponent::SubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 0.0, .trackIndex = -1 },
        MMM::Logic::NoteComponent::SubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 0.1, .trackIndex = 0 },
        MMM::Logic::NoteComponent::SubNote{ .type       = MMM::NoteType::FLICK,
                                            .timestamp  = 0.2,
                                            .trackIndex = -1,
                                            .dtrack     = 1 },
    };

    MMM::Logic::RenderSnapshot       snapshot;
    MMM::Logic::CanvasLaneProjection projection;
    entt::entity                     entity{ entt::null };
    renderCrossRegionGhost(std::move(polyline), snapshot, projection, entity);

    // 草稿轨宽为 48，八成节点宽为 38.4，左边界为 28.8。
    // 玩家节点宽为 64，左边界为 88，两侧不能沿用同一根节点宽度。
    constexpr float draftNodeX  = 28.8F;
    constexpr float draftNodeW  = 38.4F;
    constexpr float playerNodeX = 88.0F;
    constexpr float playerNodeW = 64.0F;
    // 过渡包围盒从草稿节点左边缘延伸到玩家节点右边缘。
    // 它包含端点宽度，不等于仅两条轨道中心的距离。
    constexpr float transitionW = 123.2F;
    bool            foundDraft  = false;
    bool            foundPlayer = false;
    bool            foundBody   = false;
    // 子项二是 Flick，HoldBody 在此表示水平连接而非时间持续段。
    // 该跨度取草稿与玩家轨中心，与斜向过渡包围盒不同。
    bool foundFlickBody = false;
    // 最后一个子项的箭头也以父实体和子索引发布。
    // 它落在玩家轨，宽度应与玩家节点一致而非草稿节点一致。
    bool foundFlickArrow = false;
    // 折线部件共享父实体，必须再用 subIndex 区分节点和对应连接段。
    // 只按实体或部件类型匹配会误把其他子项当作目标。
    for ( const auto& hitbox : snapshot.hitboxes ) {
        if ( hitbox.entity != entity ) continue;
        if ( hitbox.part == MMM::Logic::HoverPart::PolylineNode &&
             hitbox.subIndex == 0 ) {
            foundDraft =
                near(hitbox.x, draftNodeX) && near(hitbox.w, draftNodeW);
        } else if ( hitbox.part == MMM::Logic::HoverPart::PolylineNode &&
                    hitbox.subIndex == 1 ) {
            foundPlayer =
                near(hitbox.x, playerNodeX) && near(hitbox.w, playerNodeW);
            // 第一段的斜向连接使用起始子项索引，而不是终点索引。
            // 其横向包围盒同时覆盖两侧不同宽度的节点。
        } else if ( hitbox.part == MMM::Logic::HoverPart::HoldBody &&
                    hitbox.subIndex == 0 ) {
            foundBody =
                near(hitbox.x, draftNodeX) && near(hitbox.w, transitionW);
        } else if ( hitbox.part == MMM::Logic::HoverPart::HoldBody &&
                    hitbox.subIndex == 2 ) {
            foundFlickBody = near(hitbox.x, 48.0F) && near(hitbox.w, 72.0F);
        } else if ( hitbox.part == MMM::Logic::HoverPart::FlickArrow &&
                    hitbox.subIndex == 2 ) {
            foundFlickArrow =
                near(hitbox.x, playerNodeX) && near(hitbox.w, playerNodeW);
        }
    }

    // 各纹理的聚合范围分别保存，不按顶点提交顺序猜测部件。
    // 发光与基础命令可能分批，索引助手负责遵守各自 vertexOffset。
    float baseHeadMin       = 0.0F;
    float baseHeadMax       = 0.0F;
    float glowHeadMin       = 0.0F;
    float glowHeadMax       = 0.0F;
    float baseNodeMin       = 0.0F;
    float baseNodeMax       = 0.0F;
    float glowNodeMin       = 0.0F;
    float glowNodeMax       = 0.0F;
    float baseArrowMin      = 0.0F;
    float baseArrowMax      = 0.0F;
    float glowArrowMin      = 0.0F;
    float glowArrowMax      = 0.0F;
    float baseFlickBodyMin  = 0.0F;
    float baseFlickBodyMax  = 0.0F;
    float glowFlickBodyMin  = 0.0F;
    float glowFlickBodyMax  = 0.0F;
    float baseTransitionMin = 0.0F;
    float baseTransitionMax = 0.0F;
    float glowTransitionMin = 0.0F;
    float glowTransitionMax = 0.0F;
    // 头纹理只代表第一个草稿节点，Node 纹理范围则汇总后续节点。
    // 纹理范围验证可见层，单个节点的尺寸由前面的命中框验证。
    const bool baseHead = findTextureXRange(
        snapshot, snapshot.cmds, 0.2F, baseHeadMin, baseHeadMax);
    const bool glowHead = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.2F, glowHeadMin, glowHeadMax);
    // 后续 Node 纹理同时包含玩家节点与返回草稿区的 Flick 节点。
    // 所以合并范围横跨两区，而非仅等于第二个节点的矩形。
    const bool baseNodes = findTextureXRange(
        snapshot, snapshot.cmds, 0.6F, baseNodeMin, baseNodeMax);
    const bool glowNodes = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.6F, glowNodeMin, glowNodeMax);
    // 内嵌 Flick 虽以草稿子项为根，箭头终点仍在玩家首轨。
    // 该断言防止父折线或子项草稿属性覆盖终点区域。
    const bool baseArrow = findTextureXRange(
        snapshot, snapshot.cmds, 0.8F, baseArrowMin, baseArrowMax);
    const bool glowArrow = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.8F, glowArrowMin, glowArrowMax);
    // 折线内部 Flick 的横向身体与斜向过渡使用不同纹理。
    // 分别提取才能发现只修复折线连线却遗漏内嵌 Flick 的情况。
    const bool baseFlickBody = findTextureXRange(
        snapshot, snapshot.cmds, 0.4F, baseFlickBodyMin, baseFlickBodyMax);
    const bool glowFlickBody = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.4F, glowFlickBodyMin, glowFlickBodyMax);
    // 斜向过渡使用纵向身体纹理，范围覆盖两个不同宽度端点。
    // 它与同一折线内的水平 Flick 连接体需分别检查。
    const bool baseTransition = findTextureXRange(
        snapshot, snapshot.cmds, 0.5F, baseTransitionMin, baseTransitionMax);
    const bool glowTransition = findTextureXRange(snapshot,
                                                  snapshot.glowCmds,
                                                  0.5F,
                                                  glowTransitionMin,
                                                  glowTransitionMax);
    // 存在性、拾取尺寸和各绘制层范围全部满足才通过。
    // 尤其要求过渡覆盖玩家节点右端，避免沿用草稿宽度导致尾部截短。
    if ( !foundDraft || !foundPlayer || !foundBody || !foundFlickBody ||
         !foundFlickArrow || !baseHead || !glowHead || !baseNodes ||
         !glowNodes || !baseArrow || !glowArrow || !baseFlickBody ||
         !glowFlickBody || !baseTransition || !glowTransition ||
         !near(baseHeadMin, draftNodeX) ||
         !near(baseHeadMax, draftNodeX + draftNodeW) ||
         !near(glowHeadMin, draftNodeX) ||
         !near(glowHeadMax, draftNodeX + draftNodeW) ||
         !near(baseNodeMin, draftNodeX) ||
         !near(baseNodeMax, playerNodeX + playerNodeW) ||
         !near(glowNodeMin, draftNodeX) ||
         !near(glowNodeMax, playerNodeX + playerNodeW) ||
         !near(baseArrowMin, playerNodeX) ||
         !near(baseArrowMax, playerNodeX + playerNodeW) ||
         !near(glowArrowMin, playerNodeX) ||
         !near(glowArrowMax, playerNodeX + playerNodeW) ||
         !near(baseFlickBodyMin, 48.0F) || !near(baseFlickBodyMax, 120.0F) ||
         !near(glowFlickBodyMin, 48.0F) || !near(glowFlickBodyMax, 120.0F) ||
         !near(baseTransitionMin, draftNodeX) ||
         !near(baseTransitionMax, playerNodeX + playerNodeW) ||
         !near(glowTransitionMin, draftNodeX) ||
         !near(glowTransitionMax, playerNodeX + playerNodeW) ) {
        XERROR(
            "Cross-region Polyline geometry mismatch: draft={}, player={}, "
            "body={}, base={} [{}, {}], glow={} [{}, {}]",
            foundDraft,
            foundPlayer,
            foundBody,
            baseNodes,
            baseNodeMin,
            baseNodeMax,
            glowNodes,
            glowNodeMin,
            glowNodeMax);
        return false;
    }
    return true;
}

/// @brief 验证 Flick 与 Polyline 笔刷预览同样逐端点使用统一轨道投影。
/// @return 预览箭头、横向连接体和折线过渡均跨越 Draft/Player 间隙时返回 true。
/// @note 复用跨区布局但传入离屏占位实体，使可见目标纹理来自画笔。
/// @note Flick 和 Polyline 使用不同快照，避免前一预览残留影响范围。
/// @note 只验证几何投影，不测试画笔提交、撤销或音频播放。
/// @note 预览应采用与正式对象一致的端点布局，即使它没有独立 ECS 实体。
/// @note 所测纹理范围只来自基础命令，不对画笔发光层作额外约束。
/// @note 离屏占位 Note 使用头纹理，与待测连接体和箭头的 UV 分开。
bool testCrossRegionBrushPreviewUsesEndpointProjection()
{
    // 占位物件放在八秒处，远离当前零秒视口。
    // 夹具仍可复用真实实体路径，但所测身体和箭头来自可见画笔。
    const MMM::Logic::NoteComponent dummyNote{
        .m_type       = MMM::NoteType::NOTE,
        .m_timestamp  = 8.0,
        .m_trackIndex = 3,
    };

    // Flick 预览不生成命中盒，因此直接检查纹理顶点范围。
    MMM::Logic::RenderSnapshot::BrushSnapshot flickBrush;
    flickBrush.isActive = true;
    flickBrush.time     = 0.0;
    // 预览根轨为最后一条草稿轨，轨差一指向首条玩家轨。
    // 这里只填写工具状态，不把虚拟终点写入正式 Note Registry。
    flickBrush.track  = -1;
    flickBrush.dtrack = 1;
    flickBrush.type   = MMM::NoteType::FLICK;
    // 预览快照拥有复制后的 BrushSnapshot，不依赖原始状态继续存活。
    // 生成期间局部变量有效，测试不模拟跨线程修改工具状态。
    MMM::Logic::RenderSnapshot       flickSnapshot;
    MMM::Logic::CanvasLaneProjection flickProjection;
    entt::entity                     flickEntity{ entt::null };
    renderCrossRegionGhost(
        dummyNote, flickSnapshot, flickProjection, flickEntity, &flickBrush);

    float flickBodyMin  = 0.0F;
    float flickBodyMax  = 0.0F;
    float flickArrowMin = 0.0F;
    float flickArrowMax = 0.0F;
    // 预览没有独立命中框可用于定位，直接读取基础层目标纹理。
    // 查找并限制为身体与箭头，避开夹具背景和可能的占位物件。
    const bool flickBody = findTextureXRange(
        flickSnapshot, flickSnapshot.cmds, 0.4F, flickBodyMin, flickBodyMax);
    const bool flickArrow = findTextureXRange(
        flickSnapshot, flickSnapshot.cmds, 0.8F, flickArrowMin, flickArrowMax);
    // 期望与正式跨区 Flick 一致，证明两条生成路径使用相同端点语义。
    // 固定坐标同时约束间隙跨度和终点宽度。
    if ( !flickBody || !flickArrow || !near(flickBodyMin, 48.0F) ||
         !near(flickBodyMax, 120.0F) || !near(flickArrowMin, 88.0F) ||
         !near(flickArrowMax, 152.0F) ) {
        XERROR("Cross-region Flick brush preview did not use endpoint lane");
        return false;
    }

    // Polyline 预览同时覆盖独立宽度节点、斜向过渡和内嵌 Flick 终点。
    MMM::Logic::RenderSnapshot::BrushSnapshot polylineBrush;
    polylineBrush.isActive = true;
    polylineBrush.time     = 0.0;
    polylineBrush.track    = -1;
    polylineBrush.type     = MMM::NoteType::POLYLINE;
    // 画笔段使用渲染 DTO，区别于正式物件的 NoteComponent 子项。
    // 仍构造同样的草稿、玩家、内嵌 Flick 序列，覆盖两套数据入口。
    polylineBrush.polylineSegments = {
        MMM::Common::Render::PolylineSubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 0.0, .trackIndex = -1 },
        MMM::Common::Render::PolylineSubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 0.1, .trackIndex = 0 },
        MMM::Common::Render::PolylineSubNote{ .type      = MMM::NoteType::FLICK,
                                              .timestamp = 0.2,
                                              .trackIndex = -1,
                                              .dtrack     = 1 },
    };
    // 第二份快照隔离 Flick 预览的旧顶点与批次。
    // 否则水平身体范围可能由前一案例贡献，掩盖折线预览漏绘。
    MMM::Logic::RenderSnapshot       polylineSnapshot;
    MMM::Logic::CanvasLaneProjection polylineProjection;
    entt::entity                     polylineEntity{ entt::null };
    renderCrossRegionGhost(dummyNote,
                           polylineSnapshot,
                           polylineProjection,
                           polylineEntity,
                           &polylineBrush);

    // 每种部件从自己对应的纹理范围取值。
    // 极值查找容忍三角形顶点重复引用，不要求四边形连续排布。
    float transitionMin = 0.0F;
    float transitionMax = 0.0F;
    float bodyMin       = 0.0F;
    float bodyMax       = 0.0F;
    float arrowMin      = 0.0F;
    float arrowMax      = 0.0F;
    // 分别读取斜向过渡、水平身体和箭头的横向范围。
    // 只比较整体包围盒可能被较大的过渡掩盖错误的 Flick 尺寸。
    const bool transition = findTextureXRange(polylineSnapshot,
                                              polylineSnapshot.cmds,
                                              0.5F,
                                              transitionMin,
                                              transitionMax);
    // 水平纹理仅用于末端 Flick，不能把斜向过渡当作其替代。
    // 折线节点正确但 Flick 身体错误时仍会报告失败。
    const bool body = findTextureXRange(
        polylineSnapshot, polylineSnapshot.cmds, 0.4F, bodyMin, bodyMax);
    const bool arrow = findTextureXRange(
        polylineSnapshot, polylineSnapshot.cmds, 0.8F, arrowMin, arrowMax);
    // 预览过渡也要延伸至玩家节点外缘，而非按草稿根宽收尾。
    // 三个目标部件都需存在，缺失任一部件不能以其他几何代替。
    if ( !transition || !body || !arrow || !near(transitionMin, 28.8F) ||
         !near(transitionMax, 152.0F) || !near(bodyMin, 48.0F) ||
         !near(bodyMax, 120.0F) || !near(arrowMin, 88.0F) ||
         !near(arrowMax, 152.0F) ) {
        XERROR("Cross-region Polyline brush preview used root lane width");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行拖动单键虚影渲染回归测试。
/// @return 测试通过时返回 0。
/// @note 先执行所有案例再合并结果，让一次 CTest 保留多个独立失败日志。
/// @note 测试只创建内存快照，不读写谱面、图集或音频文件。
int main()
{
    // 各用例独立运行，不在首次失败时跳过后续区域组合。
    // 最终只合并布尔结果，失败细节由用例日志提供。
    const bool tapPassed      = testDraggedTapUsesBgmLaneBounds();
    const bool flickPassed    = testCrossRegionFlickUsesEndpointProjection();
    const bool bgmPassed      = testPlayerToBgmFlickUsesEndpointProjection();
    const bool polylinePassed = testCrossRegionPolylineUsesPerNodeProjection();
    const bool brushPassed =
        testCrossRegionBrushPreviewUsesEndpointProjection();
    // 任意区域组合失败都必须返回非零退出码。
    // 布尔合并发生在所有用例之后，不会丢失其他用例的验证机会。
    return tapPassed && flickPassed && bgmPassed && polylinePassed &&
                   brushPassed
               ? 0
               : 1;
}
