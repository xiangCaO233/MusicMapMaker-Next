#include "canvas/Basic2DCanvasInteraction.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <entt/entity/entity.hpp>
#include <variant>

namespace
{
/// @brief 运行左右窗口实际 ImGui 帧，强制左侧聚焦并检视右侧。
/// @param interaction 绑定右侧相机的生产交互控制器。
/// @param snapshot 右侧物件与分拍快照，不借用左侧数据。
/// @param pointer 鼠标屏幕位置。
/// @note 每帧显式聚焦左侧，不能靠右窗意外获得焦点通过测试。
/// 鼠标事件在帧前排队，窗口悬停使用生产 ImGui 命中结果。
/// 右窗使用独立位置和尺寸，不能被左窗遮挡。
/// 浮层不得接收输入，结束绘制后导航焦点仍应在左窗。
/// 不创建 GPU 后端，字体顶点用于验证文本实际进入绘制列表。
void frame(MMM::Canvas::Basic2DCanvasInteraction&     interaction,
           const MMM::Common::Render::RenderSnapshot& snapshot, ImVec2 pointer)
{
    // 鼠标事件在帧开始前排队，与 GLFW 后端时序一致。
    ImGui::GetIO().AddMousePosEvent(pointer.x, pointer.y);
    ImGui::NewFrame();
    constexpr auto flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;
    // 两个窗口完全分离，右窗口 Begin 不应自动抢走左窗口焦点。
    ImGui::SetNextWindowPos({ 0.0F, 0.0F });
    ImGui::SetNextWindowSize({ 300.0F, 400.0F });
    ImGui::Begin("Left", nullptr, flags);
    // 左窗聚焦后 End 只结束绘制，不改变导航所属窗口。
    ImGui::SetWindowFocus();
    ImGui::End();
    ImGui::SetNextWindowPos({ 350.0F, 0.0F });
    ImGui::SetNextWindowSize({ 300.0F, 400.0F });
    // 右窗 Begin 只切换绘制目标，不能显式聚焦。
    ImGui::Begin("Right", nullptr, flags);
    // 只读入口必须同步鼠标、拾取物件和绘制提示，不能执行完整编辑流程。
    // 调用后台入口而非完整编辑入口，确保测试覆盖本次修复。
    interaction.updateReadOnlyHover(&snapshot, 280.0F, 380.0F);
    ImGui::End();
    // 固化当前帧窗口状态和绘制数据，随后检查真实显示结果。
    ImGui::Render();
}
}  // namespace

/// @brief 验证非聚焦窗口检视实际显示且视觉命令不会误投活动窗口。
/// @details 运行真实交互控制器，不能以复制一份显示条件函数代替生产入口。
/// 事件监听核对物件的目标相机，后台实体 ID 不能投递给活动谱面。
/// 窗口检查核对提示活跃状态与文字顶点，避免仅找到旧窗口就宣称显示。
/// 鼠标离开后核对空实体与浮层隐藏，防止详情残留。
/// 测试不初始化 EditorEngine 会话，视觉事件只记录不修改项目。
/// 所有控制器和订阅先于 ImGui 上下文释放，避免跨用例状态泄漏。
/// @return 默认窗口聚焦、物件拾取、退出清理及无编辑副作用全部成立时返回零。
int main()
{
    ImGui::CreateContext();
    // 独立上下文避免继承别的用例的悬停窗口和导航焦点。
    auto& io = ImGui::GetIO();
    // 测试不读写用户布局，也不创建平台或 GPU 资源。
    io.IniFilename        = nullptr;
    io.DisplaySize        = { 800.0F, 600.0F };
    io.DeltaTime          = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    // 图集数据由 ImGui 持有，不上传 GPU，也不由测试释放。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    bool passed = false;
    {
        MMM::Canvas::Basic2DCanvasInteraction interaction(
            "Right", "Basic2DCanvas_Right");
        MMM::Common::Render::RenderSnapshot snapshot;
        snapshot.hasBeatmap       = true;
        snapshot.isHoveringCanvas = true;
        snapshot.isSnapped        = true;
        snapshot.trackCount       = 4;
        // Move 模式只有分拍或物件详情时显示，避免绘制工具常显分支掩盖问题。
        snapshot.currentTool = MMM::Logic::EditTool::Move;
        // 完整物件详情与分拍来源都属于右侧快照，避免用空提示通过测试。
        snapshot.hoverInspect.show = true;
        snapshot.hoverInspect.kind =
            MMM::Common::Render::HoverInspectKind::Note;
        snapshot.hoverInspect.head.show      = true;
        snapshot.hoverInspect.head.beatIndex = 12;
        snapshot.hoverInspect.head.numerator = 6;
        // 使用非平凡分拍，不依靠空的悬浮信息通过测试。
        snapshot.hoverInspect.head.denominator = 11;
        const auto entity                      = static_cast<entt::entity>(77);
        // 包围盒使用右窗局部坐标，实体只在所属谱面有意义。
        snapshot.hitboxes.push_back({ .entity = entity,
                                      .x      = 0.0F,
                                      .y      = 0.0F,
                                      .w      = 280.0F,
                                      .h      = 380.0F });

        // 分别记录目标命中、退出清空和编辑副作用，不用单一成功标志替代。
        bool  targeted = false, cleared = false, wrongTarget = false,
              editing = false;
        auto& bus     = MMM::Event::EventBus::instance();
        // 监听周期仅覆盖当前控制器，不给下一测试留下悬浮状态。
        const auto subscription = bus.subscribe<MMM::Event::LogicCommandEvent>(
            [&](const MMM::Event::LogicCommandEvent& event) {
                if ( const auto* hover =
                         std::get_if<MMM::Logic::CmdSetHoveredEntity>(
                             &event.command) ) {
                    // 即使聚焦左窗，后台拾取与清空也必须明确绑定右侧相机。
                    wrongTarget |= hover->cameraId != "Basic2DCanvas_Right";
                    targeted |= hover->entity == entity;
                    cleared |= hover->entity == entt::null;
                } else if ( !std::holds_alternative<
                                MMM::Logic::CmdSetMousePosition>(
                                event.command) ) {
                    // 只读入口没有点击、选中、拖动、播放或项目切换命令。
                    editing = true;
                }
            });
        // 首帧建立窗口，后续帧使用 ImGui 的真实悬停命中结果。
        for ( int index = 0; index < 3; ++index )
            frame(interaction, snapshot, { 500.0F, 200.0F });
        const auto* overlay =
            ImGui::FindWindowByName("##CanvasHoverInspectOverlay");
        // 提示、绘制顶点和左窗焦点必须同时成立。
        passed = targeted && !wrongTarget && !editing && overlay &&
                 overlay->Active && overlay->DrawList->VtxBuffer.Size > 0 &&
                 GImGui->NavWindow == ImGui::FindWindowByName("Left");
        // 鼠标回到左侧后，右侧悬浮应清空，旧提示不能残留。
        // 重新记录退出动作，不能用初始空候选的清理掩盖离开窗口后未清空。
        cleared = false;
        frame(interaction, snapshot, { 100.0F, 200.0F });
        // 初次提示创建失败可能为空，应返回失败而非解引用空指针。
        passed &=
            cleared && overlay && !overlay->Active && !wrongTarget && !editing;
        // 移除监听后才析构控制器，避免回调访问已退出的栈变量。
        bus.unsubscribe<MMM::Event::LogicCommandEvent>(subscription);
    }
    // 控制器先取消事件订阅，再销毁 ImGui 上下文，避免测试泄漏跨场景状态。
    ImGui::DestroyContext();
    // 所有失败仍经过上下文清理，不能早退跳过生命周期收尾。
    return passed ? 0 : 1;
}
