#pragma once

#include "imgui.h"

#include <string>

struct Clay_BoundingBox;

namespace MMM::UI
{

/// @brief 绘制带统一音效反馈和悬浮色过渡的 ImGui 按钮。
/// @param label 按钮显示文本和 ImGui ID。
/// @param size 按钮尺寸，语义与 ImGui::Button 保持一致。
/// @return 按钮本帧被激活时返回 true。
/// @warning UI 热路径：每帧按钮绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackButton(const char* label, const ImVec2& size = ImVec2(0, 0));

/// @brief 绘制带统一音效反馈和悬浮色过渡的 ImGui 小按钮。
/// @param label 按钮显示文本和 ImGui ID。
/// @return 按钮本帧被激活时返回 true。
/// @warning UI 热路径：每帧按钮绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSmallButton(const char* label);

}  // namespace MMM::UI

namespace MMM::UI::Utils
{

/// @brief Selectable 滚动文本核心绘制结果。
struct ScrollingSelectableResult {
    /// @brief 背景 Selectable 本帧是否被点击。
    bool clicked{ false };
};

/// @brief 树节点滚动文本核心绘制结果。
struct ScrollingTreeNodeResult {
    /// @brief 树节点本帧是否展开。
    bool open{ false };

    /// @brief 树节点本帧是否被点击且没有触发展开切换。
    bool clicked{ false };
};

/// @brief 渲染一个带水平自动滚动文本的 Selectable 核心。
/// @param id ImGui 控件 ID。
/// @param text 显示文本。
/// @param width 控件宽度。
/// @param height 控件高度。
/// @param tooltip 悬浮提示文本。
/// @return 本帧交互结果。
/// @warning UI 热路径：只执行 ImGui 控件绘制和局部文本滚动计算。
ScrollingSelectableResult renderScrollingSelectableCore(
    const std::string& id, const std::string& text, float width, float height,
    const std::string& tooltip = "");

/// @brief 渲染一个带水平自动滚动动画的 Selectable。
/// @param id ImGui 控件 ID。
/// @param text 显示文本。
/// @param width 控件宽度。
/// @param height 控件高度。
/// @param onClick 点击回调。
/// @param tooltip 悬浮提示文本。
/// @warning UI 热路径：模板层只分发点击回调，绘制逻辑在 .cpp 中。
template<typename OnClick>
void renderScrollingSelectable(const std::string& id, const std::string& text,
                               float width, float height, OnClick onClick,
                               const std::string& tooltip = "")
{
    const auto result =
        renderScrollingSelectableCore(id, text, width, height, tooltip);
    if ( result.clicked ) {
        onClick();
    }
}

/// @brief 在 Clay 布局块中渲染 CollapsingHeader，并确保其宽度适配布局边界。
/// @param label Header 标签。
/// @param p_state 展开状态。
/// @param r Clay 布局边界。
/// @param flags 额外 TreeNode 标志。
/// @return 本帧展开状态。
/// @warning UI 热路径：只做 ImGui 布局边界临时调整和 Header 绘制。
bool renderCollapsingHeader(const char* label, bool* p_state,
                            Clay_BoundingBox r, ImGuiTreeNodeFlags flags = 0);

/// @brief 在 Clay 布局块中渲染带水平自动滚动文本的 CollapsingHeader。
/// @param id ImGui ID。
/// @param text 显示文本。
/// @param p_state 展开状态。
/// @param r Clay 布局边界。
/// @param flags 额外 TreeNode 标志。
/// @return 本帧展开状态。
/// @warning UI 热路径：只做 ImGui Header 绘制和局部文本滚动计算。
bool renderScrollingCollapsingHeader(const std::string& id,
                                     const std::string& text, bool* p_state,
                                     Clay_BoundingBox   r,
                                     ImGuiTreeNodeFlags flags = 0);

/// @brief 渲染带自动滚动文本的树节点核心。
/// @param id ImGui 控件 ID。
/// @param text 显示文本。
/// @param width 控件宽度。
/// @param height 控件高度。
/// @param isLeaf 是否为叶子节点。
/// @param tooltip 悬浮提示文本。
/// @return 本帧交互结果。
/// @warning UI 热路径：只执行 ImGui 控件绘制和局部文本滚动计算。
ScrollingTreeNodeResult renderScrollingTreeNodeCore(
    const std::string& id, const std::string& text, float width, float height,
    bool isLeaf, const std::string& tooltip = "");

/// @brief 渲染带自动滚动的树节点（针对文件浏览器）。
/// @param id ImGui 控件 ID。
/// @param text 显示文本。
/// @param width 控件宽度。
/// @param height 控件高度。
/// @param isLeaf 是否为叶子节点。
/// @param onClick 点击回调。
/// @param tooltip 悬浮提示文本。
/// @return 本帧展开状态。
/// @warning UI 热路径：模板层只分发点击回调，绘制逻辑在 .cpp 中。
template<typename OnClick>
bool renderScrollingTreeNode(const std::string& id, const std::string& text,
                             float width, float height, bool isLeaf,
                             OnClick onClick, const std::string& tooltip = "")
{
    const auto result =
        renderScrollingTreeNodeCore(id, text, width, height, isLeaf, tooltip);
    if ( result.clicked ) {
        onClick();
    }
    return result.open;
}

enum class TooltipDir { Left, Right };

/// @brief 将下一个弹出式窗口固定到主视口中心。
/// @param desiredSize 期望尺寸，任意轴为 0 时不强制该轴尺寸。
/// @warning UI 热路径：只写入 ImGui 下一窗口状态。
void prepareCenteredModalWindow(ImVec2 desiredSize = ImVec2(0.0f, 0.0f));

/// @brief 全局审美配置驱动的居中模态弹窗作用域。
/// @warning UI 热路径：仅在模态弹窗绘制帧中使用，只做 ImGui next-window
/// 状态、字体栈和样式栈操作。
class CenteredModalPopupScope
{
public:
    /// @brief 使用当前窗口 DPI 缩放推入全局窗口样式。
    CenteredModalPopupScope();

    /// @brief 推入全局窗口样式。
    /// @param dpiScale 当前窗口内容缩放。
    explicit CenteredModalPopupScope(float dpiScale);

    /// @brief 恢复进入作用域前的字体与样式栈。
    ~CenteredModalPopupScope();

    /// @brief 禁止拷贝，避免重复弹出样式栈。
    CenteredModalPopupScope(const CenteredModalPopupScope&) = delete;
    /// @brief 禁止拷贝赋值，避免重复弹出样式栈。
    CenteredModalPopupScope& operator=(const CenteredModalPopupScope&) = delete;
    /// @brief 禁止移动，确保样式栈生命周期与局部作用域一致。
    CenteredModalPopupScope(CenteredModalPopupScope&&) = delete;
    /// @brief 禁止移动赋值，确保样式栈生命周期与局部作用域一致。
    CenteredModalPopupScope& operator=(CenteredModalPopupScope&&) = delete;

    /// @brief 开始一个居中的模态弹窗。
    /// @param name 弹窗标题和 ImGui ID。
    /// @param pOpen 可选打开状态指针。
    /// @param flags 额外窗口标志。
    /// @param desiredSize 期望尺寸，任意轴为 0 时交给内容自适应。
    /// @param autoResize 是否按内容自动调整尺寸。
    /// @return 弹窗本帧成功开始时返回 true。
    /// @warning UI 热路径：每帧只设置下一窗口状态，不执行阻塞操作。
    bool begin(const char* name, bool* pOpen = nullptr,
               ImGuiWindowFlags flags = ImGuiWindowFlags_None,
               ImVec2 desiredSize = ImVec2(0.0f, 0.0f), bool autoResize = true);

    /// @brief 开始一个使用模态样式的普通弹出窗口。
    /// @param name 窗口标题和 ImGui ID。
    /// @param pOpen 可选打开状态指针。
    /// @param flags 额外窗口标志。
    /// @param desiredSize 期望尺寸，任意轴为 0 时交给内容自适应。
    /// @param autoResize 是否按内容自动调整尺寸。
    /// @return 窗口本帧成功开始时返回 true。
    /// @warning UI 热路径：每帧只设置下一窗口状态，不执行阻塞操作。
    bool beginWindow(const char* name, bool* pOpen = nullptr,
                     ImGuiWindowFlags flags       = ImGuiWindowFlags_None,
                     ImVec2           desiredSize = ImVec2(0.0f, 0.0f),
                     bool             autoResize  = true);

private:
    /// @brief 使用字体加载时的固定尺寸压入弹窗标题字体。
    /// @warning UI 热路径：仅做字体栈操作；显式传入 LegacySize
    /// 以避免动态字号触发字体图集重排。
    void pushTitleFont();

    /// @brief 弹出 Begin 前推入的标题字体。
    void popTitleFontIfNeeded();

    /// @brief 构造函数中推入的样式变量数量。
    static constexpr int STYLE_VAR_COUNT = 7;

    /// @brief Begin 前临时推入的标题字体。
    ImFont* m_titleFont{ nullptr };
};

/// @brief 压入固定尺寸按钮的样式隔离变量，避免主题文字按钮内边距影响图标居中。
/// @warning 每帧 UI 绘制路径调用，只允许保留轻量 ImGui 样式栈操作。
void pushFixedButtonStyleVars();

/// @brief 弹出 pushFixedButtonStyleVars 压入的固定尺寸按钮样式变量。
/// @warning 每帧 UI 绘制路径调用，只允许保留轻量 ImGui 样式栈操作。
void popFixedButtonStyleVars();

/// @brief 绘制标准的、带有审美风格的 Tooltip。
/// @param text 文本内容。
/// @param dir 弹出方向，相对于当前 Item。
/// @warning UI 热路径：只在当前 Item 悬浮时绘制 Tooltip，不执行资源加载。
void renderTooltip(const char* text, TooltipDir dir = TooltipDir::Right);

}  // namespace MMM::UI::Utils
