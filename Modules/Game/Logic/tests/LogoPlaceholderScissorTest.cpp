#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"

#include <entt/entt.hpp>

namespace
{

/// @brief 验证无谱面占位 Logo 使用完整画布裁剪区域。
/// @return 找到 Logo 绘制命令且其 scissor 覆盖完整视口时返回 true。
/// @note 检查 CPU 快照命令，不创建 Vulkan 设备或验证实际纹理像素。
/// @note 轨道布局故意窄于画布，以检测占位图错误继承轨道裁剪的问题。
bool testLogoUsesFullViewportScissor()
{
    // 使用非正方形视口，宽高互换或误用单边尺寸都不能通过精确断言。
    constexpr float VIEWPORT_WIDTH  = 1000.0f;
    constexpr float VIEWPORT_HEIGHT = 700.0f;
    entt::registry  noteRegistry;
    // 三类实体表独立且为空，保证测试只关注无谱面占位路径。
    entt::registry             sampleRegistry;
    entt::registry             timelineRegistry;
    MMM::Logic::RenderSnapshot snapshot;
    MMM::Config::EditorConfig  config;

    // 提供快照生成所需的缓存上下文，不加载谱面或构造滚动事件。
    timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    // 空注册表不等于明确的无谱面状态，必须设置快照的 hasBeatmap 标志。
    snapshot.hasBeatmap = false;
    // 隐藏分拍线，避免无关网格命令干扰对占位分支的理解。
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    // 玩家区仅占横向 [100,600]，它的裁剪框显然不等于完整的 [0,1000]。
    config.visual.trackLayout.left  = 0.10f;
    config.visual.trackLayout.right = 0.60f;

    // 通过生产快照入口生成命令，而非手工构造一个正确 scissor 的 Logo 命令。
    // 保留主画布身份，不能切到不走相同占位逻辑的辅助视图。
    // 判定线与轨道数量只构成正常上下文，不应收窄 Logo 的裁剪范围。
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
        VIEWPORT_HEIGHT * 0.8f,
        4,
        0,
        4,
        config);

    // 快照可以包含其它绘制命令，不依赖 Logo 在命令列表中的固定序号。
    for ( const auto& command : snapshot.cmds ) {
        if ( command.customTextureId !=
             static_cast<std::uint32_t>(MMM::Logic::TextureID::Logo) ) {
            continue;
        }
        // 按纹理身份找到目标后直接检查命令自身状态，不能查看最终批处理状态。
        const auto& scissor = command.scissor;
        // 原点和范围一起验证：只有宽高正确仍可能因偏移而裁掉部分占位图。
        // 本例采用整数视口尺寸，浮点到整数转换不涉及取整歧义。
        return scissor.x == 0 && scissor.y == 0 &&
               scissor.width == static_cast<std::uint32_t>(VIEWPORT_WIDTH) &&
               scissor.height == static_cast<std::uint32_t>(VIEWPORT_HEIGHT);
    }

    // 缺少 Logo 命令不能被当作裁剪正确，否则回归可能通过隐藏占位图被掩盖。
    XERROR("LogoPlaceholderScissorTest: missing Logo draw command");
    return false;
}

}  // namespace

/// @brief 无谱面 Logo 裁剪回归测试入口。
/// @return 全部断言通过时返回 0。
/// @note 不验证多个相机的互相切换，也不检查有谱面时的普通轨道裁剪。
/// @note 本例只验收首个 Logo 命令，不能推导所有纹理批次的裁剪均正确。
int main()
{
    // 所有状态由用例局部创建，无需系统窗口、输入设备或外部资源文件。
    // 用非零退出码报告裁剪不匹配，兼容直接运行与测试运行器调用。
    // 保留用例返回值，不因缺少 GPU 渲染阶段而将失败降级成跳过。
    return testLogoUsesFullViewportScissor() ? 0 : 1;
}
