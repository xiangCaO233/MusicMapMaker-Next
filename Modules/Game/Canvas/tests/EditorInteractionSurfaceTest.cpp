#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/PlanarObservationInput.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "ui/interaction/EditorInteraction.h"
#include <imgui.h>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>

namespace
{
namespace Interaction = MMM::UI::Interaction;
/// @brief 在真实主画布控制器中替换整段操作，而非仅调用接口替身。
class SurfaceProfile final : public Interaction::IEditorInteractionProfile
{
public:
    /// @brief 测试方案身份只存在于独立测试进程。
    /// @warning 测试身份是静态字面量，注册期间不会失效或随显示文本改变。
    std::string_view id() const override { return "surface-test"; }
    /// @brief 展示名称与注册身份无关。
    /// @warning 只返回已有字符串，不在画布帧中构造描述对象。
    std::string_view name() const override { return "Surface test"; }
    /// @brief 指针手势在释放处理完毕之前阻止全局切换。
    /// @return 新控制器是否还拥有未处理完释放事件的手势。
    /// @warning 只读 UI 本地状态，不能等待旧逻辑工具完成一轮更新。
    bool hasActiveGesture() const override { return m_dragging; }
    /// @brief 当前主按钮开始选中并保持所有权，释放后才结束事务。
    /// @param frame 由实际 Basic2DCanvasInteraction 更新提供的视图上下文。
    /// @return 主画布独占时返回 true，其他表面交还原控制器。
    /// @warning 每帧只处理边沿和常量命令，不重新加载工程或创建新方案。
    bool handleSurface(const Interaction::SurfaceFrame& frame) override
    {
        if ( frame.m_surface != Interaction::Surface::MainCanvas ) return false;
        // 生产入口必须提供真实快照和投影，而不是测试自行拼装的替代坐标。
        m_receivedProjection =
            frame.m_projection != nullptr && frame.m_snapshot != nullptr;
        if ( Interaction::mouseClicked(0) && frame.m_hovered ) {
            m_dragging = true;
            Interaction::dispatchCommand(
                MMM::Event::LogicCommandEvent(MMM::Logic::CmdSelectAll{}));
        }
        // 返回 true 后不能再执行经典状态机，否则会重复发布 hover/brush 命令。
        if ( Interaction::mouseReleased(0) ) m_dragging = false;
        return true;
    }
    /// @brief 用例检查新控制器确实获得了坐标上下文。
    bool m_receivedProjection{};
    /// @brief 仅由测试 UI 线程维护，不访问会话锁。
    bool m_dragging{};
};
/// @brief 建立实际窗口并调用生产 update；窗口命中由 ImGui 计算。
/// @param interaction 绑定唯一相机身份的生产控制器，不是接口替身。
/// @param snapshot 用例拥有的不可变快照，生命周期覆盖整个输入帧。
/// @param down 是否保持物理主按钮按住，不直接修改方案自己的手势标志。
/// @details 窗口位置与尺寸固定，避免桌面窗口管理器或 DPI 改变输入命中。
/// 输入先排队再 NewFrame，保证方案与默认控制器看到同一份平台状态。
/// 调用后结束整个 ImGui 帧，不用直接调用私有 handleInteractions 模拟结果。
void frame(MMM::Canvas::Basic2DCanvasInteraction&     interaction,
           const MMM::Common::Render::RenderSnapshot& snapshot, bool down)
{
    auto& io = ImGui::GetIO();
    // 按下原点位于内容区，排除从标题栏或其他窗口拖入的情况。
    io.AddMousePosEvent(150, 150);
    io.AddMouseButtonEvent(0, down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ 400, 400 });
    ImGui::Begin("SurfaceTest", nullptr, ImGuiWindowFlags_NoSavedSettings);
    // 焦点来自正式窗口栈，不能把测试硬编码的 focused 标志交给方案。
    ImGui::SetWindowFocus();
    interaction.update(nullptr, &snapshot, 300, 300);
    ImGui::End();
    ImGui::EndFrame();
}
}  // namespace

/// @brief 确认新方案独占生产画布、释放收尾和经典操作恢复的完整链路。
/// @details 用例不创建 Vulkan 设备，字体图集只保留 CPU 数据。
/// 默认配置目录由 CTest 隔离，测试不会改变用户的编辑方案。
/// 第一次窗口帧只建立真实悬浮归属，第二帧才执行按下边沿。
/// 只有正式事件总线收到 CmdSelectAll 才算新方案的选择操作已经发布。
/// 同一帧不得再收到默认 CmdSetMousePosition，证明接管范围覆盖整段操作。
/// 新手势仍活跃时，即使调用者报告安全帧也不能立即切换方案。
/// 释放由生产更新送入当前方案，测试不直接清空其内部标志。
/// 连续安全帧后再切回默认方案，避免跳过手势交接协议。
/// 切回后的真实鼠标命令证明经典控制器恢复，而不是永久关闭编辑。
/// 画布在 ImGui 上下文之前析构，确保观察登记和订阅安全释放。
/// 所有测试输出限于构建树，不写入源码资源或实际谱面。
int main()
{
    ImGui::CreateContext();
    auto& io                        = ImGui::GetIO();
    io.IniFilename                  = nullptr;
    io.DisplaySize                  = { 800, 600 };
    io.DeltaTime                    = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels{};
    int            width{}, height{};
    // 图集由 ImGui 拥有，不上传设备，也不由用例释放内部像素指针。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto  profile  = std::make_unique<SurfaceProfile>();
    auto* observer = profile.get();
    Interaction::registerProfile(std::move(profile));
    // 激活也走正式安全帧协议，不直接改注册表的活动指针。
    Interaction::synchronizeProfile("surface-test", true);
    Interaction::synchronizeProfile("surface-test", true);
    bool ok = true;
    {
        MMM::Canvas::Basic2DCanvasInteraction interaction("SurfaceTest",
                                                          "Canvas_Probe");
        MMM::Canvas::PlanarObservationInputProjection projection;
        interaction.setInputProjection(projection);
        MMM::Common::Render::RenderSnapshot snapshot;
        // 空工程会被生产入口拒绝，测试必须经过实际有谱面分支。
        snapshot.hasBeatmap = true;
        // 四键布局只提供足够的经典拾取上下文，不添加真实谱面实体。
        snapshot.trackCount       = 4;
        snapshot.isHoveringCanvas = true;
        // 观察正式事件出口，不能把私有状态变化当作命令已执行。
        int        selected = 0, hovered = 0;
        auto&      bus          = MMM::Event::EventBus::instance();
        const auto subscription = bus.subscribe<MMM::Event::LogicCommandEvent>(
            [&](const auto& event) {
                if ( std::holds_alternative<MMM::Logic::CmdSelectAll>(
                         event.command) )
                    ++selected;
                if ( std::holds_alternative<MMM::Logic::CmdSetMousePosition>(
                         event.command) )
                    ++hovered;
            });
        // 第一帧建立真实窗口命中，第二帧才开始方案自己的选择手势。
        frame(interaction, snapshot, false);
        // 按下只允许方案自己发布一次业务命令，经典鼠标跟踪应被抑制。
        frame(interaction, snapshot, true);
        ok &= observer->m_receivedProjection && observer->m_dragging &&
              selected == 1 && hovered == 0;
        // 即使调用者误报物理输入空闲，方案自己的事务 guard 也必须阻止切出。
        ok &= !Interaction::synchronizeProfile("classic-desktop", true);
        // 仍由当前方案接收释放，不能直接修改其私有拖拽状态促成切换。
        frame(interaction, snapshot, false);
        ok &= !observer->m_dragging;
        Interaction::synchronizeProfile("classic-desktop", true);
        ok &= Interaction::synchronizeProfile("classic-desktop", true);
        // 切回经典方案后原控制器重新发送鼠标状态，不能永久停留在被接管状态。
        frame(interaction, snapshot, false);
        ok &= hovered > 0 && selected == 1;
        // 只撤销本用例的监听，不影响注册表或其他订阅者。
        bus.unsubscribe<MMM::Event::LogicCommandEvent>(subscription);
    }
    // 控制器已先析构并解除 guard，此时才释放底层 ImGui 输入快照。
    ImGui::DestroyContext();
    if ( !ok )
        XERROR(
            "EditorInteractionSurfaceTest: surface ownership or release "
            "handoff failed");
    return ok ? 0 : 1;
}
