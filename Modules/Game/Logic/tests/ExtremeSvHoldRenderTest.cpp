#include "logic/ecs/system/NoteRenderSystem.h"

#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
/// @brief 回归测试使用的画布宽度。
constexpr float VIEWPORT_WIDTH = 800.0F;
/// @brief 回归测试使用的画布高度。
constexpr float VIEWPORT_HEIGHT = 600.0F;
/// @brief 画布玩家轨道数量。
constexpr std::int32_t TRACK_COUNT = 4;
/// @brief 复现问题的极大 SV 倍率。
constexpr double EXTREME_SV = 10000.0;

/// @brief 判断顶点 UV 是否属于指定图集区域。
/// @param vertex 待检查的画布顶点。
/// @param region 图集中的起点与尺寸。
/// @return UV 位于区域内部时返回 true。
bool isInsideUvRegion(const MMM::Common::Render::CanvasVertex& vertex,
                      const glm::vec4&                         region)
{
    // 半像素内缩仍位于区域内部，因此无需依赖 Batcher 的图集尺寸常量。
    return vertex.uv.u >= region.x && vertex.uv.u <= region.x + region.z &&
           vertex.uv.v >= region.y && vertex.uv.v <= region.y + region.w;
}

/// @brief 验证极大 SV 下 Hold 主体只提交视口内矩形。
/// @return 主体纵坐标有界、宽度稳定且离屏尾部未提交时返回 true。
bool testExtremeSvHoldIsClippedBeforeBatching()
{
    // 三个 Registry 与编辑器会话中的数据边界一致。
    // 音符、采样和时间线分离可避免测试构造偏离正式渲染入口。
    entt::registry noteRegistry;
    entt::registry sampleRegistry;
    entt::registry timelineRegistry;

    // 固定 BPM 后在同一时间启用 10000 倍 SV，稳定复现超长主体投影。
    // BPM 提供 ScrollCache 积分使用的基础速度。
    // 时间零点与 SV 重合，避免额外时间段干扰极端倍率计算。
    const auto bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });
    // ScrollCache 会按 BPM、SCROLL 顺序处理同时间点事件。
    // SCROLL 值直接使用问题报告中的 10000，而不是人工放大的替代值。
    // 同时间点排序由正式缓存实现负责，测试不手动拼装 Segment。
    const auto scrollEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        scrollEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::SCROLL,
            .m_value     = EXTREME_SV,
        });

    // 默认视觉参数保留正式画布的判定线与纵向缩放语义。
    // 测试只覆盖几何稳定性，不覆盖皮肤颜色或资源加载。
    MMM::Config::EditorConfig config;
    // 缩小轨道区只影响横向布局，不改变被测纵向裁剪条件。
    config.visual.trackLayout.left  = 0.1F;
    config.visual.trackLayout.right = 0.9F;
    config.visual.noteScaleX        = 0.8F;
    config.visual.noteScaleY        = 1.0F;
    // 关闭无关动态线，保证快照中的测试纹理只来自 Hold。
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.visual.previewConfig.drawBeatLines   = false;
    config.visual.previewConfig.drawTimingLines = false;
    // 关闭辅助线只减少无关顶点，不改变 Hold 的可见性判断。
    // 主画布轨道背景仍照常生成，可验证 UV 过滤没有误选普通几何。

    // 渲染与可见性查询共享同一个 ScrollCache，避免测试绕开真实投影。
    // 缓存通过 Registry context 暴露，与 NoteRenderSystem 的读取方式一致。
    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);
    // rebuild 使用真实 BPM 与 SV 事件生成超大但有限的 double 投影。
    // 该投影在修复前会直接缩窄为远离视口的 float 顶点。

    // Hold 头部位于判定线，尾部在极大 SV 下远离视口但主体仍穿过画布。
    // 一百秒持续时间确保尾部远超画布顶部，主体仍覆盖判定线。
    // 选择普通 Hold 可直接覆盖截图中出现粗细异常的纵向长条。
    const auto holdEntity = noteRegistry.create();
    noteRegistry.emplace<MMM::Logic::NoteComponent>(
        holdEntity,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::HOLD,
            .m_timestamp  = 0.0,
            .m_duration   = 100.0,
            .m_trackIndex = 1,
        });
    // TransformComponent 是普通音符渲染实体的基础组件。
    noteRegistry.emplace<MMM::Logic::TransformComponent>(holdEntity);
    // 正式会话会维护按时间排序的实体缓存，测试显式提供同样的观察指针。
    // 单实体顺序是确定的，避免排序逻辑掩盖渲染回归。
    const std::vector<entt::entity> sortedNotes{ holdEntity };
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);

    // 快照直接收集 CPU 端顶点，因此测试无需依赖 Vulkan 驱动结果。
    // 这能在光栅化异常出现前验证危险坐标已经被消除。
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.hasBeatmap = true;
    // 为主体和尾部使用互不重叠的 UV，便于准确提取生成的顶点。
    const glm::vec4 noneUv{ 0.0F, 0.0F, 0.01F, 0.01F };
    const glm::vec4 noteUv{ 0.1F, 0.1F, 0.1F, 0.1F };
    const glm::vec4 bodyUv{ 0.4F, 0.2F, 0.05F, 0.2F };
    const glm::vec4 endUv{ 0.7F, 0.3F, 0.1F, 0.1F };
    // 主体区域较窄可同时检查皮肤宽度换算后仍保持两个固定 X 边界。
    // 尾部区域单独保留，用于证明离屏端点没有进入批次。
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None), noneUv);
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note), noteUv);
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::HoldBodyVertical),
        bodyUv);
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::HoldEnd), endUv);

    // 走完整主画布快照路径，确保测试覆盖调用方传入的真实裁剪边界。
    // 当前时间与 Hold 头部一致，使主体从判定线贯穿可见轨道区。
    // cameraId 选择主画布路径，预览区压缩投影不参与本测试。
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
        TRACK_COUNT,
        0,
        0,
        config,
        VIEWPORT_HEIGHT);

    // 一个 Hold 主体应恰好生成四个顶点；旧实现也生成四个但 Y 远超视口。
    // 只保存坐标可让后续断言独立检查纵向边界与横向宽度。
    // 尾部使用布尔标记，因为预期正确结果是完全没有对应顶点。
    std::vector<glm::vec2> bodyPositions;
    bool                   foundEndVertex = false;
    for ( const auto& vertex : snapshot.vertices ) {
        if ( isInsideUvRegion(vertex, bodyUv) ) {
            bodyPositions.push_back({ vertex.pos.x, vertex.pos.y });
        }
        if ( isInsideUvRegion(vertex, endUv) ) foundEndVertex = true;
    }
    if ( bodyPositions.size() != 4U ) {
        XERROR("Extreme-SV Hold body vertex count mismatch: {}",
               bodyPositions.size());
        return false;
    }

    // CPU 裁剪后所有主体顶点都必须落在画布纵向范围内。
    for ( const auto& position : bodyPositions ) {
        if ( !std::isfinite(position.x) || !std::isfinite(position.y) ||
             position.y < 0.0F || position.y > VIEWPORT_HEIGHT ) {
            XERROR("Extreme-SV Hold emitted out-of-viewport vertex: ({}, {})",
                   position.x,
                   position.y);
            return false;
        }
    }

    // 左右边界在两端各出现一次，证明裁剪没有改变主体宽度或形成斜边。
    const auto [minXIt, maxXIt] = std::minmax_element(
        bodyPositions.begin(),
        bodyPositions.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.x < rhs.x; });
    // minmax 只分析主体自己的四个顶点，不受轨道边界影响。
    const float minX = minXIt->x;
    const float maxX = maxXIt->x;
    if ( !(maxX > minX) ) {
        XERROR("Extreme-SV Hold body collapsed horizontally");
        return false;
    }
    // 每个顶点必须严格落在同一组左右边界，保持矩形粗细一致。
    for ( const auto& position : bodyPositions ) {
        if ( std::abs(position.x - minX) > 1e-4F &&
             std::abs(position.x - maxX) > 1e-4F ) {
            XERROR("Extreme-SV Hold body width became nonuniform");
            return false;
        }
    }

    // 远离视口的尾部不应进入顶点缓冲，避免继续携带极端坐标。
    if ( foundEndVertex ) {
        XERROR("Extreme-SV Hold emitted its off-screen end geometry");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行极大 SV 下 Hold 几何裁剪回归测试。
/// @return 测试通过时返回 0。
int main()
{
    return testExtremeSvHoldIsClippedBeforeBatching() ? 0 : 1;
}
