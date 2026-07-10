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

/// @brief 绘制带统一音效反馈的 ImGui 颜色按钮。
/// @param descId 颜色按钮描述文本和 ImGui ID。
/// @param color 按钮显示颜色。
/// @param flags 颜色按钮标志。
/// @param size 按钮尺寸，语义与 ImGui::ColorButton 保持一致。
/// @return 按钮本帧被激活时返回 true。
/// @warning UI 热路径：每帧颜色按钮绘制路径调用，只做 ImGui 状态读写
/// 和已预加载 SFX pool 的即时触发。
bool FeedbackColorButton(const char* descId, const ImVec4& color,
                         ImGuiColorEditFlags flags = 0,
                         const ImVec2&       size  = ImVec2(0, 0));

/// @brief 绘制带统一音效反馈和悬浮色过渡的 ImGui 菜单入口。
/// @param label 菜单显示文本和 ImGui ID。
/// @param enabled 是否允许打开菜单。
/// @return 菜单本帧打开时返回 true，语义与 ImGui::BeginMenu 保持一致。
/// @warning UI 热路径：每帧菜单栏绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackBeginMenu(const char* label, bool enabled = true);

/// @brief 结束由 FeedbackBeginMenu 打开的菜单。
/// @warning UI 热路径：弹出菜单绘制结束时恢复动画样式并调用 ImGui::EndMenu。
void FeedbackEndMenu();

/// @brief 绘制带统一音效反馈和悬浮色过渡的 ImGui 菜单项。
/// @param label 菜单项显示文本和 ImGui ID。
/// @param shortcut 快捷键显示文本，可为空。
/// @param selected 当前选中状态。
/// @param enabled 是否允许点击。
/// @return 菜单项本帧被激活时返回 true。
/// @warning UI 热路径：每帧菜单绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackMenuItem(const char* label, const char* shortcut = nullptr,
                      bool selected = false, bool enabled = true);

/// @brief 绘制可直接修改布尔状态的反馈式 ImGui 菜单项。
/// @param label 菜单项显示文本和 ImGui ID。
/// @param shortcut 快捷键显示文本，可为空。
/// @param pSelected 可选选中状态指针，语义与 ImGui::MenuItem 保持一致。
/// @param enabled 是否允许点击。
/// @return 菜单项本帧被激活时返回 true。
/// @warning UI 热路径：每帧菜单绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackMenuItem(const char* label, const char* shortcut, bool* pSelected,
                      bool enabled = true);

/// @brief 绘制带图标列的反馈式 ImGui 菜单项。
/// @param label 菜单项显示文本和 ImGui ID。
/// @param icon 图标文本，可为空。
/// @param shortcut 快捷键显示文本，可为空。
/// @param selected 当前选中状态。
/// @param enabled 是否允许点击。
/// @return 菜单项本帧被激活时返回 true。
/// @warning UI 热路径：每帧菜单绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackMenuItemEx(const char* label, const char* icon = nullptr,
                        const char* shortcut = nullptr, bool selected = false,
                        bool enabled = true);

/// @brief 绘制带统一反馈的 ImGui CollapsingHeader。
/// @param label Header 显示文本和 ImGui ID。
/// @param flags Header 标志。
/// @return Header 本帧展开时返回 true。
/// @warning UI 热路径：每帧 Header 绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0);

/// @brief 绘制带统一反馈的 ImGui Checkbox。
/// @param label Checkbox 显示文本和 ImGui ID。
/// @param value 当前布尔值指针。
/// @return 本帧值变化时返回 true。
/// @warning UI 热路径：每帧勾选控件绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackCheckbox(const char* label, bool* value);

/// @brief 绘制带统一反馈的 ImGui RadioButton。
/// @param label RadioButton 显示文本和 ImGui ID。
/// @param active 当前是否选中。
/// @return 本帧被激活时返回 true。
/// @warning UI 热路径：每帧单选控件绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackRadioButton(const char* label, bool active);

/// @brief 绘制带统一反馈的 ImGui RadioButton。
/// @param label RadioButton 显示文本和 ImGui ID。
/// @param value 当前整型值指针。
/// @param buttonValue 本按钮代表的值。
/// @return 本帧值变化时返回 true。
/// @warning UI 热路径：每帧单选控件绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackRadioButton(const char* label, int* value, int buttonValue);

/// @brief 绘制带统一反馈的 ImGui Selectable。
/// @param label Selectable 显示文本和 ImGui ID。
/// @param selected 当前选中状态。
/// @param flags Selectable 标志。
/// @param size Selectable 尺寸。
/// @return 本帧被激活时返回 true。
/// @warning UI 热路径：每帧列表绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSelectable(const char* label, bool selected = false,
                        ImGuiSelectableFlags flags = 0,
                        const ImVec2&        size  = ImVec2(0, 0));

/// @brief 绘制带统一反馈的 ImGui Selectable。
/// @param label Selectable 显示文本和 ImGui ID。
/// @param pSelected 可选选中状态指针。
/// @param flags Selectable 标志。
/// @param size Selectable 尺寸。
/// @return 本帧被激活时返回 true。
/// @warning UI 热路径：每帧列表绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSelectable(const char* label, bool* pSelected,
                        ImGuiSelectableFlags flags = 0,
                        const ImVec2&        size  = ImVec2(0, 0));

/// @brief 绘制带统一反馈的 ImGui BeginCombo。
/// @param label Combo 显示文本和 ImGui ID。
/// @param previewValue 当前预览文本。
/// @param flags Combo 标志。
/// @return 弹出列表打开时返回 true。
/// @warning UI 热路径：每帧 Combo 绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackBeginCombo(const char* label, const char* previewValue,
                        ImGuiComboFlags flags = 0);

/// @brief 结束由 FeedbackBeginCombo 打开的 Combo。
/// @warning UI 热路径：弹出列表绘制结束时调用 ImGui::EndCombo。
void FeedbackEndCombo();

/// @brief 绘制带统一反馈的 ImGui Combo 数组辅助控件。
/// @param label Combo 显示文本和 ImGui ID。
/// @param currentItem 当前选中索引。
/// @param items 选项文本数组。
/// @param itemsCount 选项数量。
/// @param popupMaxHeightInItems 弹出列表最大显示项数。
/// @return 选中项变化时返回 true。
/// @warning UI 热路径：每帧 Combo 绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackCombo(const char* label, int* currentItem,
                   const char* const items[], int itemsCount,
                   int popupMaxHeightInItems = -1);

/// @brief 绘制带统一反馈的 ImGui Float 滑块。
/// @warning UI 热路径：每帧滑块绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSliderFloat(const char* label, float* value, float minValue,
                         float maxValue, const char* format = "%.3f",
                         ImGuiSliderFlags flags = 0);

/// @brief 绘制带统一反馈的 ImGui Int 滑块。
/// @warning UI 热路径：每帧滑块绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSliderInt(const char* label, int* value, int minValue,
                       int maxValue, const char* format = "%d",
                       ImGuiSliderFlags flags = 0);

/// @brief 绘制带统一反馈的 ImGui 垂直 Float 滑块。
/// @warning UI 热路径：每帧滑块绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackVSliderFloat(const char* label, const ImVec2& size, float* value,
                          float minValue, float maxValue,
                          const char*      format = "%.3f",
                          ImGuiSliderFlags flags  = 0);

/// @brief 绘制带统一反馈的 ImGui Float 拖拽输入。
/// @warning UI 热路径：每帧拖拽输入绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackDragFloat(const char* label, float* value, float speed = 1.0f,
                       float minValue = 0.0f, float maxValue = 0.0f,
                       const char* format = "%.3f", ImGuiSliderFlags flags = 0);

/// @brief 绘制带统一反馈的 ImGui 二维 Int 拖拽输入。
/// @warning UI 热路径：每帧拖拽输入绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackDragInt2(const char* label, int values[2], float speed = 1.0f,
                      int minValue = 0, int maxValue = 0,
                      const char* format = "%d", ImGuiSliderFlags flags = 0);

/// @brief 绘制带统一反馈的 ImGui 标量拖拽输入。
/// @warning UI 热路径：每帧拖拽输入绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackDragScalar(const char* label, ImGuiDataType dataType, void* value,
                        float speed = 1.0f, const void* minValue = nullptr,
                        const void*      maxValue = nullptr,
                        const char*      format   = nullptr,
                        ImGuiSliderFlags flags    = 0);

/// @brief 设置统一 UI 交互音效是否允许播放。
/// @param enabled 是否允许 hover、点击、鼠标边沿和滑块反馈音效。
/// @warning UI 热路径：每帧写入一次，只更新内存标志，不执行资源操作。
void SetInteractionFeedbackEnabled(bool enabled);

/// @brief 打开 ImGui 弹窗，并在弹窗由关闭切换为打开时播放一次提示音。
/// @param popupId 弹窗显示文本和 ImGui ID。
/// @warning UI 热路径：部分弹窗会每帧调用；只查询/请求 ImGui 弹窗状态，
/// 并仅在打开边沿触发已预加载 SFX pool，不执行资源加载。
void FeedbackOpenPopup(const char* popupId);

/// @brief 播放一次独立弹窗打开提示音。
/// @warning UI 低频路径：调用方必须只在普通窗口或文件选择器的打开边沿调用；
/// 只触发已预加载 SFX pool，不执行资源加载。
void PlayPopupOpenFeedback();

/// @brief 处理全局鼠标左右键按下与松开音效。
/// @warning UI 热路径：每帧调用一次，只读取 ImGui 鼠标边沿状态并触发已预加载
/// SFX pool，禁止执行资源加载。
void ProcessGlobalMouseFeedback();

/// @brief 播放统一的鼠标松开反馈音效。
/// @warning UI 热路径：只触发已预加载 SFX pool，不执行资源加载。
void PlayInteractionMouseUpFeedback();

/// @brief 给上一条 ImGui Item 补充统一交互音效。
/// @param id 独立反馈状态 ID。
/// @param clicked 上一条 Item 本帧是否被激活。
/// @warning UI 热路径：用于自绘 hit zone，只做 ImGui 状态读写和已预加载
/// SFX pool 的即时触发。
void FeedbackLastItem(ImGuiID id, bool clicked);

/// @brief 给当前 ImGui 窗口原生关闭按钮补充统一反馈。
/// @param wasOpenBeforeBegin 调用 ImGui::Begin 前窗口是否处于打开状态。
/// @param pOpen 传给 ImGui::Begin 的打开状态指针。
/// @warning UI 热路径：每帧窗口 Begin 后调用，只读取 ImGui 内部交互状态，
/// 并触发已预加载 SFX pool。
void FeedbackCurrentWindowCloseButton(bool wasOpenBeforeBegin, bool* pOpen);

/// @brief 给指定 DockSpace 下的原生节点按钮补充统一反馈。
/// @param dockspaceId 目标 DockSpace 节点 ID。
/// @warning UI 热路径：每帧 DockSpace 绘制后调用，只遍历当前 dock 树节点，
/// 并触发已预加载 SFX pool。
void FeedbackDockNodeControls(ImGuiID dockspaceId);

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
/// @param id ImGui 内部标识。
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

/// @brief 在局部作用域内为纵向滚动容器推入 DPI 感知的最小滚动条宽度。
/// @warning UI 热路径：构造与析构只读取当前 DPI/主题并执行一次 ImGui
/// 样式栈操作。
/// @warning ImGuiStyleVar_ScrollbarSize
/// 同时控制横向滚动条高度，本作用域只能包裹 不生成横向滚动条的容器。
class VerticalScrollbarStyleScope
{
public:
    /// @brief 使用当前窗口 DPI 缩放推入纵向滚动条宽度样式。
    VerticalScrollbarStyleScope();

    /// @brief 使用指定 DPI 缩放推入纵向滚动条宽度样式。
    /// @param dpiScale 当前窗口内容缩放。
    explicit VerticalScrollbarStyleScope(float dpiScale);

    /// @brief 恢复进入作用域前的滚动条宽度样式。
    ~VerticalScrollbarStyleScope();

    /// @brief 禁止拷贝，避免重复弹出样式栈。
    VerticalScrollbarStyleScope(const VerticalScrollbarStyleScope&) = delete;
    /// @brief 禁止拷贝赋值，避免重复管理样式栈。
    VerticalScrollbarStyleScope& operator=(const VerticalScrollbarStyleScope&) =
        delete;
    /// @brief 禁止移动，确保样式栈生命周期与局部作用域一致。
    VerticalScrollbarStyleScope(VerticalScrollbarStyleScope&&) = delete;
    /// @brief 禁止移动赋值，确保样式栈生命周期与局部作用域一致。
    VerticalScrollbarStyleScope& operator=(VerticalScrollbarStyleScope&&) =
        delete;
};

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
