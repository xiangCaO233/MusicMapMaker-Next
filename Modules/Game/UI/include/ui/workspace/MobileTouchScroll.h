#pragma once
#include <imgui.h>

namespace MMM::UI
{
/// @brief 触摸内容先判定滚动意图，轻点抬手后才交付组件点击。
/// @details 只保留稳定窗口 ID；画布、标签、边框与原生滚动条不参与仲裁。
class MobileTouchScroll
{
public:
    /// @brief 在业务窗口提交前暂存原始输入并决定本帧滚动或控件输入。
    /// @warning 每帧只访问固定状态和窗口父链，不等待、不扫描全部窗口。
    void beginFrame(float scale = 1.0f);
    /// @brief 控件提交后冻结起点命中，并恢复原始输入供下一帧平台事件使用。
    /// @warning 必须与 beginFrame 配对，不能把过滤状态留给下一次 NewFrame。
    void endFrame();
    /// @brief 标记刚提交的水平滑条或拖拽数值，横向手势交给组件、纵向滚动。
    /// @warning 仅在候选按下帧检查当前 Item，不保存组件指针或回调。
    void registerHorizontalDragItem();
    /// @brief 登记竖滑条，纵向调值、横向滚动，不能把调值误识别成整页滚动。
    /// @warning 只在初始触点检查当前组件矩形，无容器遍历或分配。
    void registerVerticalDragItem();
    /// @brief 候选、滚动、点击交付及其最终释放均不能穿透到编辑画布。
    bool ownsPointer() const
    {
        return m_captured || m_widgetDrag || m_tapRelease || m_releaseGuard;
    }

private:
    /// @brief 共用初始命中门禁，只登记当前组件的主拖动方向。
    void registerDragItem(bool vertical);
    /// @brief 仅清除自己持有的 ActiveID，不销毁业务组件的活动状态。
    void cancel();
    /// @brief 从内容命中向上寻找最近可滚动祖先，弹层与画布是边界。
    bool capture(ImVec2 origin, ImU16 count, ImGuiID windowId = 0);
    /// @brief 临时改变本次 UI 提交可见的左键状态，帧末恢复平台快照。
    void filter(bool down, bool clicked, bool released, ImU16 count = 0);
    /// @brief 完整保存需要过滤的左键派生状态，避免污染后端事件去重。
    struct PointerState {
        /// @brief 左键在当前物理帧的按下、点击、释放和双击沿。
        bool m_down{}, m_clicked{}, m_released{}, m_doubleClicked{};
        /// @brief 原始点击序号保留双击打开文件等业务语义。
        ImU16 m_count{};
        /// @brief 时长及别名键状态在 UI 提交后原样恢复。
        float m_duration{}, m_previousDuration{};
        /// @brief IsKeyDown 与 IsMouseDown 必须看到同一份过滤结果。
        ImGuiKeyData m_key{};
        /// @brief 补交点击释放时可恢复原触点位置，随后归还本帧真实坐标与窗口。
        ImVec2 m_position{}, m_delta{};
        /// @brief 当前物理命中身份只在本帧借用，防止快速连点改变前一点击归属。
        ImGuiID m_hoveredWindow{};
    };
    /// @brief 当前帧的非拥有输入快照，不保存 ImGui 上下文指针。
    PointerState m_raw;
    /// @brief 起点、两轴滚动目标、独立输入 ID 及起点组件身份。
    ImGuiID m_pressedWindow{}, m_xWindow{}, m_yWindow{}, m_activeId{}, m_item{};
    /// @brief 绝对位移相对按下位置计算，合帧不会累计滚动误差。
    ImVec2 m_origin{}, m_scrollOrigin{};
    /// @brief 点击交付的下一帧若又有物理按下，保留它而不丢弃快速连点。
    ImVec2 m_deferredOrigin{}, m_tapPosition{};
    /// @brief 连点起点与补交释放各自的窗口身份，不随当前触点移动重新命中。
    ImGuiID m_deferredWindow{}, m_tapWindow{};
    /// @brief 物理点击次数与延后按下的次数分别保存，不能互相覆盖。
    ImU16 m_clickCount{}, m_deferredCount{};
    /// @brief 候选、滚动、横向组件拖动以及点击释放分开推进。
    bool m_captured{}, m_dragging{}, m_widgetDrag{}, m_tapRelease{};
    /// @brief 按下帧才登记组件方向；窗口失效后吞掉余下触点直到抬手。
    bool m_justPressed{}, m_horizontal{}, m_vertical{}, m_cancelled{};
    /// @brief 帧内输入过滤、释放保护及连点补交状态均由 UI 线程独占。
    bool m_filtered{}, m_releaseGuard{}, m_deferredPress{};
};
}  // namespace MMM::UI
