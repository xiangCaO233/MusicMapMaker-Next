#pragma once

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/layout/CLayDefs.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace MMM::UI::Utils
{

/**
 * @brief 渲染一个带有水平自动滚动动画的 Selectable。
 *        当文本宽度超出给定的 width 时，会自动开启往复滚动动画。
 */
template<typename OnClick>
static void renderScrollingSelectable(const std::string& id,
                                      const std::string& text, float width,
                                      float height, OnClick onClick,
                                      const std::string& tooltip = "")
{
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImVec2 textSize  = ImGui::CalcTextSize(text.c_str());

    // 绘制背景 Selectable (不带文本)
    std::string selectableId = "##selectable_" + id;
    if ( ImGui::Selectable(
             selectableId.c_str(), false, 0, ImVec2(width, height)) ) {
        onClick();
    }

    if ( !tooltip.empty() && ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    // 计算滚动位移
    float offset       = 0.0f;
    float padding      = 8.0f;
    float visibleWidth = width - padding;

    if ( textSize.x > visibleWidth ) {
        float scrollRange = textSize.x - visibleWidth + 40.0f;
        float time        = (float)ImGui::GetTime();
        // 平滑往复滚动，两端停顿
        float t = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
        t       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
        offset  = t * scrollRange;
    }

    // 垂直居中计算
    float textH   = ImGui::GetFontSize();
    float offsetY = (height - textH) * 0.5f;

    // 应用剪切矩形并绘制文本
    ImGui::PushClipRect(
        cursorPos, ImVec2(cursorPos.x + width, cursorPos.y + height), true);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(cursorPos.x - offset, cursorPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    ImGui::PopClipRect();
}

/**
 * @brief 在 Clay 布局块中渲染 CollapsingHeader，并确保其宽度适配布局边界。
 */
static bool renderCollapsingHeader(const char* label, bool* p_state,
                                   struct Clay_BoundingBox r,
                                   ImGuiTreeNodeFlags      flags = 0)
{
    ImGui::SetCursorScreenPos({ r.x, r.y });

    // 应用 SettingsView_Tabs 中的技巧，使 CollapsingHeader 受到 Clay
    // 布局边界的影响
    ImGuiWindow* win         = ImGui::GetCurrentWindow();
    float        savedWRMaxX = win->WorkRect.Max.x;
    win->WorkRect.Max.x      = r.x + r.width;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });

    bool open = ImGui::CollapsingHeader(
        label, flags | (*p_state ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    *p_state = open;

    ImGui::PopStyleVar();
    win->WorkRect.Max.x = savedWRMaxX;

    return open;
}

/**
 * @brief 渲染带自动滚动的树节点（针对文件浏览器）。
 */
template<typename OnClick>
static bool renderScrollingTreeNode(const std::string& id,
                                    const std::string& text, float width,
                                    float height, bool isLeaf, OnClick onClick,
                                    const std::string& tooltip = "")
{
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImVec2 textSize  = ImGui::CalcTextSize(text.c_str());

    const ImGuiStyle& style         = ImGui::GetStyle();
    const float       targetHeight  = std::max(height, ImGui::GetFrameHeight());
    const float       framePaddingY = std::max(
        style.FramePadding.y, (targetHeight - ImGui::GetFontSize()) * 0.5f);
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    if ( isLeaf )
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    // 强制占满可用宽度
    ImGui::SetNextItemAllowOverlap();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x, framePaddingY));
    bool open      = ImGui::TreeNodeEx(id.c_str(), flags, "");
    bool isHovered = ImGui::IsItemHovered();
    bool isClicked = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
    ImGui::PopStyleVar();

    if ( !tooltip.empty() && isHovered ) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    // 绘制滚动文本
    float  arrowWidth         = ImGui::GetTreeNodeToLabelSpacing();
    float  textAvailableWidth = width - arrowWidth;
    ImVec2 textStartPos       = { cursorPos.x + arrowWidth, cursorPos.y };

    float offset       = 0.0f;
    float padding      = 8.0f;
    float visibleWidth = textAvailableWidth - padding;

    if ( textSize.x > visibleWidth ) {
        float scrollRange = textSize.x - visibleWidth + 40.0f;
        float time        = (float)ImGui::GetTime();
        float t           = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
        t                 = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
        offset            = t * scrollRange;
    }

    float textH   = ImGui::GetFontSize();
    float offsetY = (targetHeight - textH) * 0.5f;

    ImGui::PushClipRect(textStartPos,
                        ImVec2(textStartPos.x + textAvailableWidth,
                               textStartPos.y + targetHeight),
                        true);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(textStartPos.x - offset, textStartPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    ImGui::PopClipRect();

    if ( isClicked ) onClick();

    return open;
}

enum class TooltipDir { Left, Right };

/// @brief 将下一个弹出式窗口固定到主视口中心。
/// @param desiredSize 期望尺寸，任意轴为 0 时不强制该轴尺寸。
/// @warning UI 热路径：只写入 ImGui 下一窗口状态。
static void prepareCenteredModalWindow(ImVec2 desiredSize = ImVec2(0.0f, 0.0f))
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(
        viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const float margin      = std::max(16.0f, 28.0f * dpiScale);
    const auto  maxAxisSize = [margin](float workSize, float preferredMin) {
        const float insetSize   = std::max(1.0f, workSize - margin * 2.0f);
        const float clampedMin  = std::min(preferredMin, workSize);
        const float clampedSize = std::max(clampedMin, insetSize);
        return std::max(1.0f, std::min(clampedSize, workSize));
    };
    const ImVec2 maxWindowSize{
        maxAxisSize(viewport->WorkSize.x, 160.0f * dpiScale),
        maxAxisSize(viewport->WorkSize.y, 160.0f * dpiScale),
    };
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), maxWindowSize);

    if ( desiredSize.x > 0.0f || desiredSize.y > 0.0f ) {
        if ( desiredSize.x > 0.0f ) {
            desiredSize.x = std::min(desiredSize.x, maxWindowSize.x);
        }
        if ( desiredSize.y > 0.0f ) {
            desiredSize.y = std::min(desiredSize.y, maxWindowSize.y);
        }
        ImGui::SetNextWindowSize(desiredSize, ImGuiCond_Always);
    }
}

/// @brief 全局审美配置驱动的居中模态弹窗作用域。
/// @warning UI 热路径：仅在模态弹窗绘制帧中使用，只做 ImGui next-window
/// 状态、字体栈和样式栈操作。
class CenteredModalPopupScope
{
public:
    /// @brief 推入全局窗口样式。
    /// @param dpiScale 当前窗口内容缩放。
    explicit CenteredModalPopupScope(
        float dpiScale = Config::AppConfig::instance().getWindowContentScale())
    {
        const auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        const float windowRound =
            std::floor(aesthetics.windowRounding * dpiScale);
        const float frameRound =
            std::floor(aesthetics.frameRounding * dpiScale);
        const ImVec2 windowPadding{
            std::floor(aesthetics.windowPadding * dpiScale),
            std::floor(aesthetics.windowPadding * dpiScale),
        };
        const ImVec2 itemSpacing{
            std::floor(aesthetics.itemSpacing * dpiScale),
            std::floor(aesthetics.itemSpacing * dpiScale),
        };

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, windowPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);
    }

    /// @brief 恢复进入作用域前的字体与样式栈。
    ~CenteredModalPopupScope()
    {
        popTitleFontIfNeeded();
        ImGui::PopStyleVar(STYLE_VAR_COUNT);
    }

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
               ImVec2 desiredSize = ImVec2(0.0f, 0.0f), bool autoResize = true)
    {
        prepareCenteredModalWindow(desiredSize);

        flags |= ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize;
        if ( autoResize ) {
            flags |= ImGuiWindowFlags_AlwaysAutoResize;
        }

        pushTitleFont();
        const bool opened = ImGui::BeginPopupModal(name, pOpen, flags);
        popTitleFontIfNeeded();
        return opened;
    }

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
                     bool             autoResize  = true)
    {
        prepareCenteredModalWindow(desiredSize);

        flags |= ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize;
        if ( autoResize ) {
            flags |= ImGuiWindowFlags_AlwaysAutoResize;
        }

        pushTitleFont();
        const bool opened = ImGui::Begin(name, pOpen, flags);
        popTitleFontIfNeeded();
        return opened;
    }

private:
    /// @brief 使用字体加载时的固定尺寸压入弹窗标题字体。
    /// @warning UI 热路径：仅做字体栈操作；显式传入 LegacySize
    /// 以避免动态字号触发字体图集重排。
    void pushTitleFont()
    {
        m_titleFont = Config::SkinManager::instance().getFont("title");
        if ( m_titleFont ) {
            ImGui::PushFont(m_titleFont, m_titleFont->LegacySize);
        }
    }

    /// @brief 弹出 Begin 前推入的标题字体。
    void popTitleFontIfNeeded()
    {
        if ( m_titleFont ) {
            ImGui::PopFont();
            m_titleFont = nullptr;
        }
    }

    /// @brief 构造函数中推入的样式变量数量。
    static constexpr int STYLE_VAR_COUNT = 7;

    /// @brief Begin 前临时推入的标题字体。
    ImFont* m_titleFont{ nullptr };
};

/// @brief 压入固定尺寸按钮的样式隔离变量，避免主题文字按钮内边距影响图标居中。
/// @warning 每帧 UI 绘制路径调用，只允许保留轻量 ImGui 样式栈操作。
static void pushFixedButtonStyleVars()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
}

/// @brief 弹出 pushFixedButtonStyleVars 压入的固定尺寸按钮样式变量。
/// @warning 每帧 UI 绘制路径调用，只允许保留轻量 ImGui 样式栈操作。
static void popFixedButtonStyleVars()
{
    ImGui::PopStyleVar(2);
}

/// @brief 绘制标准的、带有审美风格的 Tooltip。
/// @param text 文本内容。
/// @param dir 弹出方向，相对于当前 Item。
static void renderTooltip(const char* text, TooltipDir dir = TooltipDir::Right)
{
    if ( ImGui::IsItemHovered() ) {
        // 动态获取配置
        auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        float winPadding  = std::floor(aesthetics.windowPadding * dpiScale);
        float winRounding = std::floor(aesthetics.windowRounding * dpiScale);

        ImVec2 pos    = ImGui::GetItemRectMin();
        ImVec2 max    = ImGui::GetItemRectMax();
        float  gap    = 6.0f * dpiScale;
        ImVec2 target = { 0.0f, 0.0f };
        ImVec2 pivot  = { 0.0f, 0.0f };

        if ( dir == TooltipDir::Left ) {
            target = { pos.x - gap, pos.y };
            pivot  = { 1.0f, 0.0f };
        } else {
            target = { max.x + gap, pos.y };
            pivot  = { 0.0f, 0.0f };
        }

        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        ImGui::SetNextWindowPos(target, ImGuiCond_Always, pivot);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(winPadding, winPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, winRounding);

        ImFont* contentFont =
            Config::SkinManager::instance().getFont("content");
        if ( contentFont )
            ImGui::PushFont(contentFont, contentFont->LegacySize);

        if ( ImGui::BeginTooltip() ) {
            ImGui::TextUnformatted(text);
            ImGui::EndTooltip();
        }

        if ( contentFont ) ImGui::PopFont();
        ImGui::PopStyleVar(2);
    }
}

}  // namespace MMM::UI::Utils
