#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"

#include <entt/entt.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{

// 在 CPU 快照层验证配色、遮罩尺寸和裁剪命令，不创建 Vulkan 设备。
// 纹理坐标是人工夹具；皮肤加载仅用于提供实际配置及颜色来源。
/// @brief 固定画布宽度，用于把归一化轨道布局换算为像素。
constexpr float VIEWPORT_WIDTH = 800.0F;
/// @brief 固定画布高度，同时作为快照生成的参考高度。
constexpr float VIEWPORT_HEIGHT = 600.0F;

/// @brief 为同轨同时间的两个 Tap 生成重叠遮罩快照。
/// @param snapshot 输出快照。
/// @param isPlaying 是否模拟播放中的渲染状态。
/// @param trackIndex Tap 所在轨道。
/// @param useAuxiliaryLaneLayout 是否启用覆盖草稿区和 BGM 区的布局。
/// @pre snapshot 为新建快照，不携带前一次调用的几何和遮罩。
/// @note 两个不同实体具有相同位置，不能用同一实体重复索引代替真实重叠。
/// @note 只构造 Tap，重叠长条、折线和音频采样不属于这个夹具。
/// @note 生成过程为同步调用，返回后快照中的几何可脱离局部注册表读取。
/// @note 轨道地址与草稿标志由夹具共同构造，不覆盖非法地址的容错行为。
void renderOverlappingTaps(MMM::Logic::RenderSnapshot& snapshot, bool isPlaying,
                           int  trackIndex             = 1,
                           bool useAuxiliaryLaneLayout = false)
{
    entt::registry noteRegistry;
    entt::registry sampleRegistry;
    // 空采样注册表隔离独立音频物件，避免把它们的覆盖命令当成音符遮罩。
    entt::registry timelineRegistry;

    // 固定 BPM 保持时间到屏幕投影一致，不引入 SV 反转等额外变量。
    const auto bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    config.visual.trackLayout.left  = useAuxiliaryLaneLayout ? 0.5F : 0.1F;
    config.visual.trackLayout.right = useAuxiliaryLaneLayout ? 0.9F : 0.5F;
    // 辅助布局整体右移玩家区，为左侧草稿物件留出独立可见区域。
    if ( useAuxiliaryLaneLayout ) {
        // 自定义草稿区与玩家区宽度不同，用于验证遮罩不再沿用玩家几何。
        config.visual.trackLayout.draftLanes.left  = -0.21F;
        config.visual.trackLayout.draftLanes.width = 0.06F;
        config.visual.trackLayout.bgmLanes.left    = 0.92F;
        config.visual.trackLayout.bgmLanes.width   = 0.08F;
    }
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    // 不绘制分拍线，减少与遮罩无关的图元；不以总顶点数作为成功条件。
    // 草稿用例必须启用专业模式，避免隐藏策略掩盖独立区域投影问题。
    config.settings.professionalMode = useAuxiliaryLaneLayout;

    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);
    // 缓存在两个物件生成前按同一配置建立，比较只改变场景参数，
    // 不把未初始化的滚动映射当成遮罩渲染回归。

    // 只保留两个必要物件，使遮罩 objectCount 的预期值唯一且清晰。
    std::vector<entt::entity> sortedNotes;
    sortedNotes.reserve(2U);
    // 同时刻输入天然满足时间排序，按插入顺序登记即可，无需另行排序。
    for ( std::size_t index = 0; index < 2U; ++index ) {
        const auto                entity = noteRegistry.create();
        MMM::Logic::NoteComponent note;
        note.m_timestamp  = 1.0;
        note.m_type       = MMM::NoteType::NOTE;
        note.m_trackIndex = trackIndex;
        note.m_isDraft    = trackIndex < 0;
        // 草稿标志与负轨号同时设置，不依赖渲染器猜测不一致的模型状态。
        noteRegistry.emplace<MMM::Logic::NoteComponent>(entity,
                                                        std::move(note));
        noteRegistry.emplace<MMM::Logic::TransformComponent>(entity);
        sortedNotes.push_back(entity);
    }
    // ctx 中登记的是索引容器指针，既不能传临时 vector，也不能在生成前搬走它。
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);
    // 索引由局部容器拥有，生命周期覆盖下面的同步快照生成调用。

    snapshot.hasBeatmap = true;
    // 显式标记已加载谱面，避免落入无项目时的 Logo 占位绘制流程。
    snapshot.isPlaying     = isPlaying;
    snapshot.playbackSpeed = 1.0;
    // 播放用例仅切换状态标志，不改变速度，避免位置差异来自播放倍率。
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note),
        glm::vec4{ 0.25F, 0.35F, 0.2F, 0.1F });
    // 固定纹理宽高比使遮罩几何可重复，不让皮肤图片分辨率变化影响本测试尺寸。

    // 当前时间与物件时间相等，把重叠区域放在判定线附近而非视口裁剪边缘。
    // 使用主画布入口，辅助区域和覆盖层均按编辑画布策略生成。
    // 辅助布局传入一条 BGM 轨只是提供区域布局，不向空采样表添加事件。
    MMM::Logic::System::NoteRenderSystem::generateSnapshot(
        noteRegistry,
        sampleRegistry,
        {},
        {},
        timelineRegistry,
        {},
        &snapshot,
        "Basic2DCanvas",
        1.0,
        VIEWPORT_WIDTH,
        VIEWPORT_HEIGHT,
        500.0F,
        4,
        useAuxiliaryLaneLayout ? 1 : 0,
        4,
        config,
        VIEWPORT_HEIGHT);
}

/// @brief 为玩家区和草稿区各生成一个带相同自定义颜色的 Tap。
/// @param snapshot 输出快照。
/// @pre 皮肤已成功加载，snapshot 为本次场景独占的新快照。
/// @note 相同自定义颜色分别用于正式物件和草稿物件，以比较颜色来源优先级。
/// @note 两个物件分处不同区域，不用于验证重叠计数，配色检查与遮罩检查分离。
/// @note 不给采样或背景物件配置相同 UV，避免后续颜色存在性判断命中无关图元。
void renderPlayerAndDraftTaps(MMM::Logic::RenderSnapshot& snapshot)
{
    entt::registry noteRegistry;
    entt::registry sampleRegistry;
    entt::registry timelineRegistry;

    const auto bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    config.visual.trackLayout.left  = 0.5F;
    config.visual.trackLayout.right = 0.9F;
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.settings.professionalMode = true;
    // 两类物件必须同时可见，不能用草稿隐藏后的空结果验证配色隔离。

    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);
    // 本用例保留默认草稿布局，与前一夹具的自定义草稿宽度测试分开，
    // 只验证草稿身份是否决定颜色来源，不同时改变宽度规则。

    constexpr glm::vec4 SHARED_CUSTOM_COLOR{ 0.12F, 0.34F, 0.56F, 1.0F };
    // 选择非默认的自定义色，玩家顶点应采用它，草稿顶点应采用专用皮肤色。
    std::vector<entt::entity> sortedNotes;
    for ( const auto [track, isDraft] :
          { std::pair{ 0, false }, std::pair{ -4, true } } ) {
        // 玩家索引从零起，草稿使用负轨号；不能把 -4 当成无符号玩家索引。
        const auto                entity = noteRegistry.create();
        MMM::Logic::NoteComponent note;
        note.m_timestamp        = 1.0;
        note.m_type             = MMM::NoteType::NOTE;
        note.m_trackIndex       = track;
        note.m_isDraft          = isDraft;
        note.m_customColors.tap = SHARED_CUSTOM_COLOR;
        // 显式写入运行时颜色覆盖字段，使玩家分支确实走自定义色优先级，
        // 而非依赖恰好与期望值相同的皮肤默认色。
        // 除所属区域外保持类型、时间和自定义色一致，避免混入物件类型色差。
        noteRegistry.emplace<MMM::Logic::NoteComponent>(entity,
                                                        std::move(note));
        noteRegistry.emplace<MMM::Logic::TransformComponent>(entity);
        sortedNotes.push_back(entity);
    }
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);

    snapshot.hasBeatmap = true;
    // 纯色与音符 UV 区域分离，后续只检查音符纹理范围内的顶点颜色。
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note),
        glm::vec4{ 0.25F, 0.35F, 0.2F, 0.1F });

    // 两种颜色共存于同一快照，避免分两次加载皮肤掩盖全局颜色状态串用。
    MMM::Logic::System::NoteRenderSystem::generateSnapshot(noteRegistry,
                                                           sampleRegistry,
                                                           {},
                                                           {},
                                                           timelineRegistry,
                                                           {},
                                                           &snapshot,
                                                           "Basic2DCanvas",
                                                           1.0,
                                                           VIEWPORT_WIDTH,
                                                           VIEWPORT_HEIGHT,
                                                           500.0F,
                                                           4,
                                                           0,
                                                           4,
                                                           config,
                                                           VIEWPORT_HEIGHT);
}

/// @brief 判断颜色通道在小容差内相等。
/// @param color 画布顶点颜色。
/// @param expected 期望颜色。
/// @return 四个通道均匹配时返回 true。
/// @note 透明度也属于颜色契约，不能只匹配 RGB 而漏掉错误的淡化处理。
/// @note 在顶点色数值空间比较，不引入颜色空间转换或屏幕截图采样。
/// @note 非有限通道不会通过差值容差比较，避免损坏颜色被视为有效匹配。
bool sameColor(const MMM::Common::Render::CanvasColor& color,
               const MMM::Config::Color&               expected)
{
    constexpr float EPSILON = 1e-4F;
    // 小容差吸收浮点换算，不允许把肉眼接近但来源不同的颜色视为相等。
    return std::abs(color.r - expected.r) < EPSILON &&
           std::abs(color.g - expected.g) < EPSILON &&
           std::abs(color.b - expected.b) < EPSILON &&
           std::abs(color.a - expected.a) < EPSILON;
}

/// @brief 验证玩家自定义颜色不能覆盖草稿皮肤色。
/// @return 玩家与草稿 Tap 的实际顶点颜色分别匹配对应来源时返回 true。
/// @note 分别要求至少一个匹配顶点，不声称遍历确认每个音符顶点都同色。
/// @pre 加载皮肤的草稿 Tap 色应与测试自定义色不同，才能区分两个来源。
bool testDraftTapUsesDedicatedSkinColor()
{
    // 独立快照不复用其他测试顶点，缺少任一类物件时不能靠旧图元满足断言。
    MMM::Logic::RenderSnapshot snapshot;
    renderPlayerAndDraftTaps(snapshot);

    auto&      skin       = MMM::Config::SkinManager::instance();
    const auto draftColor = skin.getColor("draft_notes.note_tap");
    // 草稿期望值来自本次已加载皮肤，而不是硬编码默认皮肤的颜色常量。
    constexpr MMM::Config::Color PLAYER_CUSTOM_COLOR{
        0.12F, 0.34F, 0.56F, 1.0F
    };
    bool playerMatched = false;
    bool draftMatched  = false;
    for ( const auto& vertex : snapshot.vertices ) {
        // 只接收人工 Note UV 矩形内的顶点，跳过背景与轨道底板几何。
        if ( vertex.uv.u < 0.25F || vertex.uv.u > 0.45F ||
             vertex.uv.v < 0.35F || vertex.uv.v > 0.45F ) {
            continue;
        }
        // 这里比较的是已提交顶点属性，不读取 NoteComponent 上的原始自定义色。
        // 因而能发现组件配置正确但渲染分支仍用了错误颜色的情况。
        if ( vertex.pos.x >= VIEWPORT_WIDTH * 0.5F ) {
            // 本夹具玩家区域在右半边，草稿区域在左半边，据此区分颜色断言。
            playerMatched |= sameColor(vertex.color, PLAYER_CUSTOM_COLOR);
            // 累积存在性结果，后续不匹配的顶点不会抹掉已经找到的匹配项。
        } else {
            draftMatched |= sameColor(vertex.color, draftColor);
        }
    }
    if ( !playerMatched || !draftMatched ) {
        // 两类都必须实际绘制，任何一类缺失不能算作配色互不干扰。
        XERROR("Draft and player Tap vertices did not use separated colors");
        return false;
    }
    return true;
}

/// @brief 验证草稿重叠遮罩使用独立区域宽度和裁剪范围。
/// @return 遮罩出现在自定义草稿轨并按其单轨宽度缩放时返回 true。
/// @note 同时要求遮罩数据和覆盖层命令，不能只生成元数据而不提交绘制。
/// @note 宽度预期沿用默认 noteScaleX，与夹具未覆盖该配置项的行为对应。
/// @note 只检查首个遮罩，不断言遮罩总数，也不以 overlayCmds 数量推断图层数。
/// @note 本用例失败只返回 false，进程级退出码由 main 汇总。
bool testDraftOverlapMaskUsesIndependentWidth()
{
    MMM::Logic::RenderSnapshot snapshot;
    renderOverlappingTaps(snapshot, false, -1, true);
    if ( snapshot.overlapMasks.empty() || snapshot.overlayCmds.empty() ) {
        // 先验证容器非空，再检查首个遮罩和命令，避免断言自身越界。
        return false;
    }
    const auto&               mask = snapshot.overlapMasks.front();
    MMM::Config::EditorConfig defaults;
    const float               expectedWidth =
        VIEWPORT_WIDTH * 0.06F * defaults.visual.noteScaleX;
    // 独立计算预期宽度，不回调生产投影函数计算期望值，避免相同错误互相抵消。
    // 草稿轨宽由独立比例 0.06 决定，不能回退到玩家区四等分后的轨宽。
    // 这里的“右侧草稿轨”是草稿区内靠右的一轨，整体仍位于玩家区左侧。
    // 检查命令左裁剪边界为零；完整横向宽度由辅助裁剪场景另外验证。
    return mask.x < VIEWPORT_WIDTH * 0.1F &&
           std::abs(mask.w - expectedWidth) < 1e-4F &&
           snapshot.overlayCmds.front().scissor.x == 0;
}

/// @brief 验证播放状态不会抑制重叠键的顶层遮罩。
/// @return 两状态的首个遮罩计数和几何相等，且都存在覆盖层命令时返回 true。
/// @note 比较首个遮罩的计数与几何，不比较完整命令列表、颜色或 GPU 合成结果。
/// @note 不启动播放设备或推进音频时钟，只测试快照生成时读取的播放状态。
bool testOverlapMaskRemainsVisibleDuringPlayback()
{
    MMM::Logic::RenderSnapshot stopped;
    MMM::Logic::RenderSnapshot playing;
    renderOverlappingTaps(stopped, false);
    renderOverlappingTaps(playing, true);
    // 两次生成各用全新的 Registry 和快照，不靠上一状态遗留的覆盖命令通过检查。

    // 静止分支先建立正向基线，防止两个分支都没有遮罩时误通过比较。
    if ( stopped.overlapMasks.empty() || stopped.overlayCmds.empty() ) {
        XERROR("Stopped overlap fixture did not generate an overlay mask");
        return false;
    }
    if ( playing.overlapMasks.empty() || playing.overlayCmds.empty() ) {
        XERROR("Playback suppressed the overlap overlay mask");
        return false;
    }

    const auto& stoppedMask = stopped.overlapMasks.front();
    const auto& playingMask = playing.overlapMasks.front();
    // 首个遮罩在两份极小夹具中都对应唯一的重叠位置，无需跨实体 ID 配对。
    // 切换播放状态不能改变重叠计数，也不能平移或缩放同一时刻的遮罩。
    constexpr float EPSILON = 1e-4F;
    // 显式要求两物件计数，不能只判断两状态计数相等而让同样漏计的结果通过。
    return stoppedMask.objectCount == 2 && playingMask.objectCount == 2 &&
           // 几何四项分别比较，不能只用面积相同代替位置与尺寸均未改变。
           std::abs(stoppedMask.x - playingMask.x) < EPSILON &&
           std::abs(stoppedMask.y - playingMask.y) < EPSILON &&
           std::abs(stoppedMask.w - playingMask.w) < EPSILON &&
           std::abs(stoppedMask.h - playingMask.h) < EPSILON;
}

/// @brief 验证草稿区及玩家区边界外的重叠遮罩使用完整辅助轨裁剪范围。
/// @return 左右越界遮罩均未被主玩家区裁剪时返回 true。
/// @note 右侧用例是玩家末轨的物件外缘越界，不是在 BGM 轨创建音符。
/// @note 仅验证视口内辅助区域不被玩家区裁掉，仍允许整个视口边缘正常裁剪。
/// @note 几何越界先作为夹具有效性条件，再检查绘制命令中的裁剪状态。
bool testOverlapMaskUsesAuxiliaryLaneScissor()
{
    MMM::Logic::RenderSnapshot draftSnapshot;
    MMM::Logic::RenderSnapshot boundarySnapshot;
    renderOverlappingTaps(draftSnapshot, false, -1, true);
    renderOverlappingTaps(boundarySnapshot, false, 3, true);
    // track=3 是四轨玩家区的末轨；track=-1 是左侧草稿地址，二者共享辅助布局。

    // 两个场景分别覆盖玩家区左侧草稿和右侧物件外缘，不能相互替代。
    if ( draftSnapshot.overlapMasks.empty() ||
         draftSnapshot.overlayCmds.empty() ||
         boundarySnapshot.overlapMasks.empty() ||
         boundarySnapshot.overlayCmds.empty() ) {
        XERROR("Auxiliary lane overlap fixture did not generate overlay masks");
        return false;
    }

    constexpr float PLAYER_LEFT_X  = VIEWPORT_WIDTH * 0.5F;
    constexpr float PLAYER_RIGHT_X = VIEWPORT_WIDTH * 0.9F;
    // 边界值来自本夹具的布局配置，与遮罩的中心或宽度计算相互独立。
    const auto& draftMask    = draftSnapshot.overlapMasks.front();
    const auto& boundaryMask = boundarySnapshot.overlapMasks.front();
    if ( draftMask.x + draftMask.w > PLAYER_LEFT_X ) {
        // 先确认夹具确实位于玩家区外，否则全视口裁剪断言缺少针对性。
        XERROR("Draft overlap fixture unexpectedly entered the player lanes");
        return false;
    }
    if ( boundaryMask.x + boundaryMask.w <= PLAYER_RIGHT_X ) {
        // 默认物件宽度需要越过末轨边缘，才会暴露仅按玩家区裁剪的问题。
        XERROR("Boundary overlap fixture did not cross the player lane edge");
        return false;
    }

    // 几何仍在正确位置但命令用了玩家区 scissor 时，GPU 执行会截掉辅助部分；
    // 所以本用例不能只判断 mask.x/mask.w 而忽略批处理命令状态。
    /// @brief 检查首个覆盖命令的横向裁剪是否覆盖完整视口。
    /// @param snapshot 已确认覆盖命令非空的快照。
    /// @return 左边界为零且宽度等于固定视口宽度时返回 true。
    /// @note 不检查纵向裁剪；本场景目标是辅助区域横向边界。
    const auto usesFullViewportScissor = [](const auto& snapshot) {
        // 比较提交给渲染器的 scissor，而不是仅看遮罩包围框是否位于视口内。
        const auto& command = snapshot.overlayCmds.front();
        // 固定 800 像素视口可精确转换为整数裁剪宽度，这里不测试 DPI 舍入策略。
        return command.scissor.x == 0 &&
               command.scissor.width ==
                   static_cast<std::uint32_t>(VIEWPORT_WIDTH);
    };
    // 左侧草稿与右侧外缘都必须保留完整视口裁剪，任一侧通过不代表另一侧也正确。
    return usesFullViewportScissor(draftSnapshot) &&
           usesFullViewportScissor(boundarySnapshot);
}

}  // namespace

/// @brief 运行草稿分色与重叠键遮罩渲染回归测试。
/// @param argc 参数数量，至少包含皮肤入口与翻译根目录。
/// @param argv 两个资源路径由 CTest 配置传入，手动运行时也必须提供。
/// @return 全部测试通过时返回 0。
/// @note 返回 1 包含参数、资源加载和断言失败，日志用于区分具体阶段。
/// @note 使用仓库资源作为只读输入，不依赖个人配置中安装的皮肤版本。
/// @note 本程序不重写传入的皮肤或翻译资源，也不生成谱面测试产物。
int main(int argc, char* argv[])
{
    if ( argc < 3 || !argv[1] || !argv[2] ) {
        // 资源缺失直接失败，不能使用皮肤管理器的默认哨兵色继续颜色断言。
        XERROR("NoteOverlapMaskRenderTest requires skin and translation paths");
        return 1;
    }
    // 资源路径作为参数读取，测试从构建目录运行时也不依赖源码根作为工作目录。
    auto& skin = MMM::Config::SkinManager::instance();
    // 只加载一次共享皮肤，后续夹具不修改其配置；所有预期色均来自同一资源版本。
    if ( !skin.loadSkin(argv[1], MMM::Config::utf8ToPath(argv[2])) ) {
        // 皮肤加载失败与几何断言失败分别报告，便于区分资源准备问题。
        XERROR("NoteOverlapMaskRenderTest failed to load bundled skin");
        return 1;
    }
    // 短路执行，返回零才表示配色、独立轨宽、播放与辅助裁剪四项全部通过。
    // 测试只运行一次快照生成序列，不启动后台渲染循环或等待窗口事件。
    // 非零退出可能尚未运行后续场景，不能将失败轮次解释为四项均已完成检查。
    return testDraftTapUsesDedicatedSkinColor() &&
                   testDraftOverlapMaskUsesIndependentWidth() &&
                   testOverlapMaskRemainsVisibleDuringPlayback() &&
                   testOverlapMaskUsesAuxiliaryLaneScissor()
               ? 0
               : 1;
}
