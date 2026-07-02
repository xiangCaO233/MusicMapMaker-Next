#include "ui/utils/UIWidgetUtils.h"

#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui_internal.h"
#include "ui/layout/CLayDefs.h"

#include <algorithm>
#include <cmath>

namespace MMM::UI::Utils
{

/// @brief Tooltip 动画进度存储键的盐值。
constexpr ImGuiID TOOLTIP_ANIM_AMOUNT_KEY_SALT = 0x6D6D5421u;

/// @brief Tooltip 最后一次更新帧存储键的盐值。
constexpr ImGuiID TOOLTIP_LAST_FRAME_KEY_SALT = 0x6D6D5422u;

/// @brief Tooltip 淡入速度，值越大越快。
constexpr float TOOLTIP_FADE_IN_SPEED = 12.0f;

/// @brief Tooltip 弹出动画的横向位移像素。
constexpr float TOOLTIP_SLIDE_X = 6.0f;

/// @brief Tooltip 弹出动画的纵向位移像素。
constexpr float TOOLTIP_SLIDE_Y = 4.0f;

/// @brief 计算带盐的 Tooltip 状态键。
/// @param id 控件 ID。
/// @param salt 用途盐值。
/// @return 用于 ImGuiStorage 的状态键。
ImGuiID makeTooltipStorageKey(ImGuiID id, ImGuiID salt)
{
    return id ^ salt;
}

/// @brief 计算 ease-out cubic 缓动值。
/// @param value 线性进度。
/// @return 缓动后的进度。
float easeOutCubic(float value)
{
    const float t   = std::clamp(value, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

/// @brief 更新当前 Item 的 Tooltip 弹出动画进度。
/// @param itemId 当前 Item 的 ImGui ID。
/// @param isHovered 当前 Item 是否悬浮。
/// @return 本帧动画进度。
/// @warning UI 热路径：只访问当前窗口 ImGuiStorage，不执行资源加载。
float updateTooltipAnimationAmount(ImGuiID itemId, bool isHovered)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();
    if ( !storage ) {
        return isHovered ? 1.0f : 0.0f;
    }

    const ImGuiID amountKey =
        makeTooltipStorageKey(itemId, TOOLTIP_ANIM_AMOUNT_KEY_SALT);
    const ImGuiID lastFrameKey =
        makeTooltipStorageKey(itemId, TOOLTIP_LAST_FRAME_KEY_SALT);
    const int  currentFrame      = ImGui::GetFrameCount();
    const int  lastFrame         = storage->GetInt(lastFrameKey, -1);
    const bool wasDrawnLastFrame = lastFrame == currentFrame - 1;

    if ( !isHovered ) {
        storage->SetFloat(amountKey, 0.0f);
        storage->SetInt(lastFrameKey, currentFrame);
        return 0.0f;
    }

    float amount =
        wasDrawnLastFrame ? storage->GetFloat(amountKey, 0.0f) : 0.0f;
    const float step =
        std::max(0.0f, ImGui::GetIO().DeltaTime) * TOOLTIP_FADE_IN_SPEED;
    amount = std::min(1.0f, amount + step);

    storage->SetFloat(amountKey, amount);
    storage->SetInt(lastFrameKey, currentFrame);
    return amount;
}

/// @brief 计算水平滚动文本偏移。
/// @param textWidth 完整文本宽度。
/// @param visibleWidth 可见宽度。
/// @return 当前帧滚动偏移。
float calcScrollingTextOffset(float textWidth, float visibleWidth)
{
    if ( textWidth <= visibleWidth ) {
        return 0.0f;
    }

    const float scrollRange = textWidth - visibleWidth + 40.0f;
    const float time        = static_cast<float>(ImGui::GetTime());
    float       t           = std::sin(time * 0.5f - 1.57f) * 0.5f + 0.5f;
    t                       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
    return t * scrollRange;
}

/// @brief 在剪切矩形中绘制单行滚动文本。
/// @param text 文本内容。
/// @param startPos 绘制起点。
/// @param availableWidth 可用宽度。
/// @param targetHeight 可用高度。
/// @warning UI 热路径：只向当前窗口 DrawList 添加文字。
void drawScrollingText(const std::string& text, ImVec2 startPos,
                       float availableWidth, float targetHeight)
{
    const ImVec2 textSize     = ImGui::CalcTextSize(text.c_str());
    const float  visibleWidth = std::max(0.0f, availableWidth);
    const float  offset  = calcScrollingTextOffset(textSize.x, visibleWidth);
    const float  textH   = ImGui::GetFontSize();
    const float  offsetY = (targetHeight - textH) * 0.5f;

    ImGui::PushClipRect(
        startPos,
        ImVec2(startPos.x + visibleWidth, startPos.y + targetHeight),
        true);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(startPos.x - offset, startPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    ImGui::PopClipRect();
}

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
    const std::string& tooltip)
{
    const ImVec2      cursorPos    = ImGui::GetCursorScreenPos();
    const std::string selectableId = "##selectable_" + id;
    const bool        clicked      = ImGui::Selectable(
        selectableId.c_str(), false, 0, ImVec2(width, height));

    if ( !tooltip.empty() && ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    drawScrollingText(text, cursorPos, width - 8.0f, height);
    return { .clicked = clicked };
}

/// @brief 在 Clay 布局块中渲染 CollapsingHeader，并确保其宽度适配布局边界。
/// @param label Header 标签。
/// @param p_state 展开状态。
/// @param r Clay 布局边界。
/// @param flags 额外 TreeNode 标志。
/// @return 本帧展开状态。
/// @warning UI 热路径：只做 ImGui 布局边界临时调整和 Header 绘制。
bool renderCollapsingHeader(const char* label, bool* p_state,
                            Clay_BoundingBox r, ImGuiTreeNodeFlags flags)
{
    ImGui::SetCursorScreenPos({ r.x, r.y });

    ImGuiWindow* win         = ImGui::GetCurrentWindow();
    float        savedWRMaxX = win->WorkRect.Max.x;
    win->WorkRect.Max.x      = r.x + r.width;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });

    const bool open = ImGui::CollapsingHeader(
        label, flags | (*p_state ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    *p_state = open;

    ImGui::PopStyleVar();
    win->WorkRect.Max.x = savedWRMaxX;

    return open;
}

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
                                     ImGuiTreeNodeFlags flags)
{
    ImGui::SetCursorScreenPos({ r.x, r.y });

    ImGuiWindow* win         = ImGui::GetCurrentWindow();
    float        savedWRMaxX = win->WorkRect.Max.x;
    win->WorkRect.Max.x      = r.x + r.width;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });

    const std::string hiddenLabel = "##" + id;
    const bool        open        = ImGui::CollapsingHeader(
        hiddenLabel.c_str(),
        flags | (*p_state ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    *p_state = open;

    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();

    ImGui::PopStyleVar();
    win->WorkRect.Max.x = savedWRMaxX;

    const ImGuiStyle& style        = ImGui::GetStyle();
    const float       targetHeight = std::max(itemMax.y - itemMin.y, r.height);
    const float       arrowWidth   = ImGui::GetTreeNodeToLabelSpacing();
    const float       textPadding  = style.FramePadding.x;
    const ImVec2      textStartPos = { itemMin.x + arrowWidth, itemMin.y };
    const float       textAvailableWidth =
        std::max(0.0f, itemMax.x - textStartPos.x - textPadding);

    drawScrollingText(text, textStartPos, textAvailableWidth, targetHeight);
    return open;
}

/// @brief 渲染带自动滚动文本的树节点核心。
/// @param id ImGui 控件 ID。
/// @param text 显示文本。
/// @param width 控件宽度。
/// @param height 控件高度。
/// @param isLeaf 是否为叶子节点。
/// @param tooltip 悬浮提示文本。
/// @return 本帧交互结果。
/// @warning UI 热路径：只执行 ImGui 控件绘制和局部文本滚动计算。
ScrollingTreeNodeResult renderScrollingTreeNodeCore(const std::string& id,
                                                    const std::string& text,
                                                    float width, float height,
                                                    bool               isLeaf,
                                                    const std::string& tooltip)
{
    const ImVec2      cursorPos     = ImGui::GetCursorScreenPos();
    const ImGuiStyle& style         = ImGui::GetStyle();
    const float       targetHeight  = std::max(height, ImGui::GetFrameHeight());
    const float       framePaddingY = std::max(
        style.FramePadding.y, (targetHeight - ImGui::GetFontSize()) * 0.5f);

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    if ( isLeaf ) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    ImGui::SetNextItemAllowOverlap();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x, framePaddingY));
    const bool open      = ImGui::TreeNodeEx(id.c_str(), flags, "");
    const bool isHovered = ImGui::IsItemHovered();
    const bool isClicked =
        ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
    ImGui::PopStyleVar();

    if ( !tooltip.empty() && isHovered ) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    const float  arrowWidth         = ImGui::GetTreeNodeToLabelSpacing();
    const float  treeGutterWidth    = isLeaf ? 0.0f : arrowWidth;
    const float  textAvailableWidth = width - treeGutterWidth - 8.0f;
    const ImVec2 textStartPos = { cursorPos.x + treeGutterWidth, cursorPos.y };

    drawScrollingText(text, textStartPos, textAvailableWidth, targetHeight);
    return { .open = open, .clicked = isClicked };
}

/// @brief 将下一个弹出式窗口固定到主视口中心。
/// @param desiredSize 期望尺寸，任意轴为 0 时不强制该轴尺寸。
/// @warning UI 热路径：只写入 ImGui 下一窗口状态。
void prepareCenteredModalWindow(ImVec2 desiredSize)
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

/// @brief 使用当前窗口 DPI 缩放推入全局窗口样式。
CenteredModalPopupScope::CenteredModalPopupScope()
    : CenteredModalPopupScope(
          Config::AppConfig::instance().getWindowContentScale())
{
}

/// @brief 推入全局窗口样式。
/// @param dpiScale 当前窗口内容缩放。
CenteredModalPopupScope::CenteredModalPopupScope(float dpiScale)
{
    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    const float  windowRound = std::floor(aesthetics.windowRounding * dpiScale);
    const float  frameRound  = std::floor(aesthetics.frameRounding * dpiScale);
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
CenteredModalPopupScope::~CenteredModalPopupScope()
{
    popTitleFontIfNeeded();
    ImGui::PopStyleVar(STYLE_VAR_COUNT);
}

/// @brief 开始一个居中的模态弹窗。
/// @param name 弹窗标题和 ImGui ID。
/// @param pOpen 可选打开状态指针。
/// @param flags 额外窗口标志。
/// @param desiredSize 期望尺寸，任意轴为 0 时交给内容自适应。
/// @param autoResize 是否按内容自动调整尺寸。
/// @return 弹窗本帧成功开始时返回 true。
/// @warning UI 热路径：每帧只设置下一窗口状态，不执行阻塞操作。
bool CenteredModalPopupScope::begin(const char* name, bool* pOpen,
                                    ImGuiWindowFlags flags, ImVec2 desiredSize,
                                    bool autoResize)
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
bool CenteredModalPopupScope::beginWindow(const char* name, bool* pOpen,
                                          ImGuiWindowFlags flags,
                                          ImVec2 desiredSize, bool autoResize)
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

/// @brief 使用字体加载时的固定尺寸压入弹窗标题字体。
/// @warning UI 热路径：仅做字体栈操作；显式传入 LegacySize
/// 以避免动态字号触发字体图集重排。
void CenteredModalPopupScope::pushTitleFont()
{
    m_titleFont = Config::SkinManager::instance().getFont("title");
    if ( m_titleFont ) {
        ImGui::PushFont(m_titleFont, m_titleFont->LegacySize);
    }
}

/// @brief 弹出 Begin 前推入的标题字体。
void CenteredModalPopupScope::popTitleFontIfNeeded()
{
    if ( m_titleFont ) {
        ImGui::PopFont();
        m_titleFont = nullptr;
    }
}

/// @brief 压入固定尺寸按钮的样式隔离变量，避免主题文字按钮内边距影响图标居中。
/// @warning 每帧 UI 绘制路径调用，只允许保留轻量 ImGui 样式栈操作。
void pushFixedButtonStyleVars()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
}

/// @brief 弹出 pushFixedButtonStyleVars 压入的固定尺寸按钮样式变量。
/// @warning 每帧 UI 绘制路径调用，只允许保留轻量 ImGui 样式栈操作。
void popFixedButtonStyleVars()
{
    ImGui::PopStyleVar(2);
}

/// @brief 绘制标准的、带有审美风格的 Tooltip。
/// @param text 文本内容。
/// @param dir 弹出方向，相对于当前 Item。
/// @warning UI 热路径：只在当前 Item 悬浮时绘制 Tooltip，不执行资源加载。
void renderTooltip(const char* text, TooltipDir dir)
{
    if ( text == nullptr || text[0] == '\0' ) {
        return;
    }

    const bool    isHovered = ImGui::IsItemHovered();
    const ImGuiID rawItemId = ImGui::GetItemID();
    const ImGuiID itemId    = rawItemId != 0 ? rawItemId : ImGui::GetID(text);
    const float   amount    = updateTooltipAnimationAmount(itemId, isHovered);
    if ( amount <= 0.001f ) {
        return;
    }

    const float eased = easeOutCubic(amount);
    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const float winPadding  = std::floor(aesthetics.windowPadding * dpiScale);
    const float winRounding = std::floor(aesthetics.windowRounding * dpiScale);

    const ImVec2 pos = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float  gap = 6.0f * dpiScale;
    ImVec2       target{ 0.0f, 0.0f };
    ImVec2       pivot{ 0.0f, 0.0f };

    if ( dir == TooltipDir::Left ) {
        target = { pos.x - gap, pos.y };
        pivot  = { 1.0f, 0.0f };
        target.x += TOOLTIP_SLIDE_X * dpiScale * (1.0f - eased);
    } else {
        target = { max.x + gap, pos.y };
        pivot  = { 0.0f, 0.0f };
        target.x -= TOOLTIP_SLIDE_X * dpiScale * (1.0f - eased);
    }
    target.y += TOOLTIP_SLIDE_Y * dpiScale * (1.0f - eased);

    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::SetNextWindowPos(target, ImGuiCond_Always, pivot);

    const float currentAlpha = ImGui::GetStyle().Alpha;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, currentAlpha * eased);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(winPadding, winPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, winRounding);

    ImFont* contentFont = Config::SkinManager::instance().getFont("content");
    if ( contentFont ) {
        ImGui::PushFont(contentFont, contentFont->LegacySize);
    }

    if ( ImGui::BeginTooltip() ) {
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();
    }

    if ( contentFont ) {
        ImGui::PopFont();
    }
    ImGui::PopStyleVar(3);
}

}  // namespace MMM::UI::Utils

namespace MMM::UI
{
namespace
{
/// @brief UI 按钮进入悬浮时播放的皮肤音频 ID。
constexpr const char* BUTTON_HOVER_SFX_KEY = "ui.hover";

/// @brief UI 按钮激活时播放的皮肤音频 ID。
constexpr const char* BUTTON_CLICK_SFX_KEY = "ui.click";

/// @brief 悬浮音效的单次触发音量倍率。
constexpr float BUTTON_HOVER_SFX_VOLUME = 0.22f;

/// @brief 点击音效的单次触发音量倍率。
constexpr float BUTTON_CLICK_SFX_VOLUME = 0.36f;

/// @brief 悬浮颜色过渡速度，值越大越快。
constexpr float BUTTON_HOVER_FADE_SPEED = 10.0f;

/// @brief 按钮悬浮状态存储键的盐值。
constexpr ImGuiID BUTTON_HOVERED_KEY_SALT = 0x6D6D4821u;

/// @brief 按钮悬浮过渡进度存储键的盐值。
constexpr ImGuiID BUTTON_HOVER_AMOUNT_KEY_SALT = 0x6D6D4822u;

/// @brief 按钮最后一次绘制帧存储键的盐值。
constexpr ImGuiID BUTTON_LAST_FRAME_KEY_SALT = 0x6D6D4823u;

/// @brief 菜单打开状态存储键的盐值。
constexpr ImGuiID MENU_OPEN_KEY_SALT = 0x6D6D4D21u;

/// @brief 菜单弹窗动画进度存储键的盐值。
constexpr ImGuiID MENU_POPUP_AMOUNT_KEY_SALT = 0x6D6D4D22u;

/// @brief 菜单弹窗最后一次绘制帧存储键的盐值。
constexpr ImGuiID MENU_POPUP_LAST_FRAME_KEY_SALT = 0x6D6D4D23u;

/// @brief 菜单弹窗关闭动画状态存储键的盐值。
constexpr ImGuiID MENU_POPUP_CLOSING_KEY_SALT = 0x6D6D4D24u;

/// @brief 菜单弹窗淡入速度，值越大越快。
constexpr float MENU_POPUP_FADE_SPEED = 8.0f;

/// @brief 菜单弹窗滑入位移像素。
constexpr float MENU_POPUP_SLIDE_Y = 8.0f;

/// @brief 计算带盐的 ImGuiStorage 键，避免与控件自身 ID 冲突。
/// @param id 控件 ID。
/// @param salt 用途盐值。
/// @return 用于 ImGuiStorage 的状态键。
ImGuiID makeButtonStorageKey(ImGuiID id, ImGuiID salt)
{
    return id ^ salt;
}

/// @brief 将数值限制到 0 到 1。
/// @param value 输入值。
/// @return 限制后的值。
float saturate(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

/// @brief 计算 ease-out cubic 缓动值。
/// @param value 线性进度。
/// @return 缓动后的进度。
float easeOutCubic(float value)
{
    const float t   = saturate(value);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

/// @brief 按线性权重混合两种 ImGui 颜色。
/// @param from 起始颜色。
/// @param to 目标颜色。
/// @param amount 混合权重。
/// @return 混合后的颜色。
ImVec4 lerpColor(const ImVec4& from, const ImVec4& to, float amount)
{
    const float t = saturate(amount);
    return ImVec4(from.x + (to.x - from.x) * t,
                  from.y + (to.y - from.y) * t,
                  from.z + (to.z - from.z) * t,
                  from.w + (to.w - from.w) * t);
}

/// @brief 读取上一帧悬浮状态并推进按钮颜色过渡。
/// @param id 按钮 ImGui ID。
/// @param storage 当前控件所属窗口的状态存储。
/// @return 本帧绘制应使用的悬浮过渡进度。
/// @warning UI 热路径：每个按钮每帧调用，只访问当前窗口 ImGuiStorage。
float updateButtonHoverAmount(ImGuiID id, ImGuiStorage* storage)
{
    if ( !storage ) return 0.0f;

    const ImGuiID hoveredKey =
        makeButtonStorageKey(id, BUTTON_HOVERED_KEY_SALT);
    const ImGuiID amountKey =
        makeButtonStorageKey(id, BUTTON_HOVER_AMOUNT_KEY_SALT);
    const ImGuiID lastFrameKey =
        makeButtonStorageKey(id, BUTTON_LAST_FRAME_KEY_SALT);

    const int  currentFrame      = ImGui::GetFrameCount();
    const int  lastFrame         = storage->GetInt(lastFrameKey, -1);
    const bool wasDrawnLastFrame = lastFrame == currentFrame - 1;
    const bool wasHovered =
        wasDrawnLastFrame && storage->GetInt(hoveredKey, 0) != 0;
    const ImGuiID openKey = makeButtonStorageKey(id, MENU_OPEN_KEY_SALT);
    const bool  wasOpen = wasDrawnLastFrame && storage->GetInt(openKey, 0) != 0;
    const float target  = (wasHovered || wasOpen) ? 1.0f : 0.0f;
    float       amount =
        wasDrawnLastFrame ? storage->GetFloat(amountKey, 0.0f) : 0.0f;
    const float step = std::min(
        1.0f,
        std::max(0.0f, ImGui::GetIO().DeltaTime) * BUTTON_HOVER_FADE_SPEED);

    if ( amount < target ) {
        amount = std::min(target, amount + step);
    } else {
        amount = std::max(target, amount - step);
    }

    storage->SetFloat(amountKey, amount);
    return amount;
}

/// @brief 读取上一帧悬浮状态并推进按钮颜色过渡。
/// @param id 按钮 ImGui ID。
/// @return 本帧绘制应使用的悬浮过渡进度。
/// @warning UI 热路径：每个按钮每帧调用，只访问当前窗口 ImGuiStorage。
float updateButtonHoverAmount(ImGuiID id)
{
    return updateButtonHoverAmount(id, ImGui::GetStateStorage());
}

/// @brief 写回按钮当前交互状态并按边沿触发音效。
/// @param id 按钮 ImGui ID。
/// @param clicked 按钮本帧是否被激活。
/// @param storage 当前控件所属窗口的状态存储。
/// @param isHovered 当前控件是否悬浮。
/// @warning UI 热路径：只读取上一帧状态并触发已预加载音效池，不执行资源加载。
void finishButtonFeedback(ImGuiID id, bool clicked, ImGuiStorage* storage,
                          bool isHovered)
{
    if ( !storage ) return;

    const ImGuiID hoveredKey =
        makeButtonStorageKey(id, BUTTON_HOVERED_KEY_SALT);
    const ImGuiID lastFrameKey =
        makeButtonStorageKey(id, BUTTON_LAST_FRAME_KEY_SALT);
    const int  currentFrame = ImGui::GetFrameCount();
    const bool wasHovered =
        storage->GetInt(lastFrameKey, -1) == currentFrame - 1 &&
        storage->GetInt(hoveredKey, 0) != 0;

    if ( isHovered && !wasHovered ) {
        Audio::AudioManager::instance().playSoundEffect(
            BUTTON_HOVER_SFX_KEY, BUTTON_HOVER_SFX_VOLUME);
    }

    if ( clicked ) {
        Audio::AudioManager::instance().playSoundEffect(
            BUTTON_CLICK_SFX_KEY, BUTTON_CLICK_SFX_VOLUME);
    }

    storage->SetInt(hoveredKey, isHovered ? 1 : 0);
    storage->SetInt(lastFrameKey, currentFrame);
}

/// @brief 写回菜单当前交互状态并按边沿触发音效。
/// @param id 菜单 ImGui ID。
/// @param clicked 菜单入口本帧是否被点击。
/// @param open 菜单本帧是否打开。
/// @param storage 菜单入口所属窗口的状态存储。
/// @param isHovered 菜单入口本帧是否悬浮。
/// @warning UI 热路径：只读取上一帧状态并触发已预加载音效池，不执行资源加载。
void finishMenuFeedback(ImGuiID id, bool clicked, bool open,
                        ImGuiStorage* storage, bool isHovered)
{
    finishButtonFeedback(id, clicked, storage, isHovered);
    if ( !storage ) return;

    const ImGuiID openKey = makeButtonStorageKey(id, MENU_OPEN_KEY_SALT);
    storage->SetInt(openKey, open ? 1 : 0);
}

/// @brief 写回按钮当前交互状态并按边沿触发音效。
/// @param id 按钮 ImGui ID。
/// @param clicked 按钮本帧是否被激活。
/// @warning UI 热路径：只读取上一帧状态并触发已预加载音效池，不执行资源加载。
void finishButtonFeedback(ImGuiID id, bool clicked)
{
    finishButtonFeedback(
        id, clicked, ImGui::GetStateStorage(), ImGui::IsItemHovered());
}

/// @brief 压入按钮悬浮颜色过渡样式。
/// @param hoverAmount 当前悬浮过渡进度。
/// @return 压入的样式颜色数量。
/// @warning UI 热路径：只操作 ImGui 样式栈。
int pushAnimatedButtonColors(float hoverAmount)
{
    const ImVec4 baseColor  = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    const ImVec4 hoverColor = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
    const ImVec4 mixedColor = lerpColor(baseColor, hoverColor, hoverAmount);

    ImGui::PushStyleColor(ImGuiCol_Button, mixedColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mixedColor);
    return 2;
}

/// @brief 压入菜单项悬浮颜色过渡样式。
/// @param hoverAmount 当前悬浮过渡进度。
/// @return 压入的样式颜色数量。
/// @warning UI 热路径：只操作 ImGui 样式栈。
int pushAnimatedMenuColors(float hoverAmount)
{
    const ImVec4 baseColor  = ImGui::GetStyleColorVec4(ImGuiCol_Header);
    const ImVec4 hoverColor = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    const ImVec4 mixedColor = lerpColor(baseColor, hoverColor, hoverAmount);

    ImGui::PushStyleColor(ImGuiCol_Header, mixedColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, mixedColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, mixedColor);
    return 3;
}

/// @brief 推进菜单弹窗打开动画。
/// @param id 菜单入口 ImGui ID。
/// @param storage 菜单入口所属窗口的状态存储。
/// @param open 菜单本帧是否打开。
/// @return 当前弹窗动画进度。
/// @warning UI 热路径：只访问当前窗口 ImGuiStorage。
float updateMenuPopupAmount(ImGuiID id, ImGuiStorage* storage, bool open)
{
    if ( !storage ) return open ? 1.0f : 0.0f;

    const ImGuiID amountKey =
        makeButtonStorageKey(id, MENU_POPUP_AMOUNT_KEY_SALT);
    const ImGuiID lastFrameKey =
        makeButtonStorageKey(id, MENU_POPUP_LAST_FRAME_KEY_SALT);
    const int  currentFrame      = ImGui::GetFrameCount();
    const int  lastFrame         = storage->GetInt(lastFrameKey, -1);
    const bool wasDrawnLastFrame = lastFrame == currentFrame - 1;
    float      amount =
        wasDrawnLastFrame ? storage->GetFloat(amountKey, 0.0f) : 0.0f;
    const float target = open ? 1.0f : 0.0f;
    const float step   = std::min(
        1.0f, std::max(0.0f, ImGui::GetIO().DeltaTime) * MENU_POPUP_FADE_SPEED);

    if ( amount < target ) {
        amount = std::min(target, amount + step);
    } else {
        amount = std::max(target, amount - step);
    }

    storage->SetFloat(amountKey, amount);
    storage->SetInt(lastFrameKey, currentFrame);
    return amount;
}

/// @brief 读取菜单弹窗上一帧动画进度。
/// @param id 菜单入口 ImGui ID。
/// @param storage 菜单入口所属窗口的状态存储。
/// @return 上一帧弹窗动画进度。
/// @warning UI 热路径：只访问当前窗口 ImGuiStorage。
float getStoredMenuPopupAmount(ImGuiID id, ImGuiStorage* storage)
{
    if ( !storage ) return 0.0f;

    return storage->GetFloat(
        makeButtonStorageKey(id, MENU_POPUP_AMOUNT_KEY_SALT), 0.0f);
}

/// @brief 对当前已打开菜单弹窗应用窗口级进入动画。
/// @param amount 动画线性进度。
/// @warning UI 热路径：只设置当前 ImGui 窗口位置和 alpha 样式。
void pushMenuPopupAnimation(float amount)
{
    const float  eased = easeOutCubic(amount);
    const float  alpha = std::max(0.04f, eased);
    const ImVec2 pos   = ImGui::GetWindowPos();
    ImGui::SetWindowPos(
        ImVec2(pos.x, pos.y - MENU_POPUP_SLIDE_Y * (1.0f - eased)),
        ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * alpha);
}

}  // namespace

/// @brief 绘制带统一音效反馈和悬浮色过渡的 ImGui 按钮。
/// @param label 按钮显示文本和 ImGui ID。
/// @param size 按钮尺寸，语义与 ImGui::Button 保持一致。
/// @return 按钮本帧被激活时返回 true。
/// @warning UI 热路径：每帧按钮绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackButton(const char* label, const ImVec2& size)
{
    const ImGuiID id               = ImGui::GetID(label);
    const float   hoverAmount      = updateButtonHoverAmount(id);
    const int     pushedColorCount = pushAnimatedButtonColors(hoverAmount);
    const bool    clicked          = ImGui::Button(label, size);
    ImGui::PopStyleColor(pushedColorCount);
    finishButtonFeedback(id, clicked);
    return clicked;
}

/// @brief 绘制带统一音效反馈和悬浮色过渡的 ImGui 小按钮。
/// @param label 按钮显示文本和 ImGui ID。
/// @return 按钮本帧被激活时返回 true。
/// @warning UI 热路径：每帧按钮绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSmallButton(const char* label)
{
    const ImGuiID id               = ImGui::GetID(label);
    const float   hoverAmount      = updateButtonHoverAmount(id);
    const int     pushedColorCount = pushAnimatedButtonColors(hoverAmount);
    const bool    clicked          = ImGui::SmallButton(label);
    ImGui::PopStyleColor(pushedColorCount);
    finishButtonFeedback(id, clicked);
    return clicked;
}

/// @brief 绘制带统一音效反馈和悬浮色过渡的 ImGui 菜单入口。
/// @param label 菜单显示文本和 ImGui ID。
/// @param enabled 是否允许打开菜单。
/// @return 菜单本帧打开时返回 true，语义与 ImGui::BeginMenu 保持一致。
/// @warning UI 热路径：每帧菜单栏绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackBeginMenu(const char* label, bool enabled)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id      = ImGui::GetID(label);
    const ImGuiID closingKey =
        makeButtonStorageKey(id, MENU_POPUP_CLOSING_KEY_SALT);
    const ImGuiID openKey    = makeButtonStorageKey(id, MENU_OPEN_KEY_SALT);
    bool          closing    = storage && storage->GetInt(closingKey, 0) != 0;
    const bool    wasOpen    = storage && storage->GetInt(openKey, 0) != 0;
    const float   lastAmount = getStoredMenuPopupAmount(id, storage);
    const bool    popupOpen  = ImGui::IsPopupOpen(label);
    if ( enabled && wasOpen && !popupOpen && lastAmount > 0.01f ) {
        closing = true;
        storage->SetInt(closingKey, 1);
    }
    if ( closing && lastAmount <= 0.01f ) {
        closing = false;
        if ( storage ) {
            storage->SetInt(closingKey, 0);
        }
    }
    if ( closing && lastAmount > 0.01f ) {
        ImGui::OpenPopup(label);
    }

    const float hoverAmount      = updateButtonHoverAmount(id, storage);
    const int   pushedColorCount = pushAnimatedMenuColors(hoverAmount);
    const bool  open             = ImGui::BeginMenu(label, enabled);
    const bool  clicked          = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool  hovered          = ImGui::IsItemHovered();
    if ( clicked ) {
        closing = false;
        if ( storage ) {
            storage->SetInt(closingKey, 0);
        }
    }
    ImGui::PopStyleColor(pushedColorCount);
    finishMenuFeedback(id, clicked, open && !closing, storage, hovered);
    if ( open ) {
        const float amount = updateMenuPopupAmount(id, storage, !closing);
        pushMenuPopupAnimation(amount);
        if ( closing && amount <= 0.02f ) {
            if ( storage ) {
                storage->SetInt(closingKey, 0);
            }
            ImGui::CloseCurrentPopup();
        }
    } else {
        updateMenuPopupAmount(id, storage, false);
    }
    return open;
}

/// @brief 结束由 FeedbackBeginMenu 打开的菜单。
/// @warning UI 热路径：弹出菜单绘制结束时恢复动画样式并调用 ImGui::EndMenu。
void FeedbackEndMenu()
{
    ImGui::PopStyleVar();
    ImGui::EndMenu();
}

/// @brief 绘制带统一音效反馈和悬浮色过渡的 ImGui 菜单项。
/// @param label 菜单项显示文本和 ImGui ID。
/// @param shortcut 快捷键显示文本，可为空。
/// @param selected 当前选中状态。
/// @param enabled 是否允许点击。
/// @return 菜单项本帧被激活时返回 true。
/// @warning UI 热路径：每帧菜单绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackMenuItem(const char* label, const char* shortcut, bool selected,
                      bool enabled)
{
    ImGuiStorage* storage          = ImGui::GetStateStorage();
    const ImGuiID id               = ImGui::GetID(label);
    const float   hoverAmount      = updateButtonHoverAmount(id, storage);
    const int     pushedColorCount = pushAnimatedMenuColors(hoverAmount);
    const bool    clicked = ImGui::MenuItem(label, shortcut, selected, enabled);
    const bool    hovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(pushedColorCount);
    finishButtonFeedback(id, clicked, storage, hovered);
    return clicked;
}

/// @brief 绘制可直接修改布尔状态的反馈式 ImGui 菜单项。
/// @param label 菜单项显示文本和 ImGui ID。
/// @param shortcut 快捷键显示文本，可为空。
/// @param pSelected 可选选中状态指针，语义与 ImGui::MenuItem 保持一致。
/// @param enabled 是否允许点击。
/// @return 菜单项本帧被激活时返回 true。
/// @warning UI 热路径：每帧菜单绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackMenuItem(const char* label, const char* shortcut, bool* pSelected,
                      bool enabled)
{
    ImGuiStorage* storage          = ImGui::GetStateStorage();
    const ImGuiID id               = ImGui::GetID(label);
    const float   hoverAmount      = updateButtonHoverAmount(id, storage);
    const int     pushedColorCount = pushAnimatedMenuColors(hoverAmount);
    const bool clicked = ImGui::MenuItem(label, shortcut, pSelected, enabled);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(pushedColorCount);
    finishButtonFeedback(id, clicked, storage, hovered);
    return clicked;
}

/// @brief 绘制带图标列的反馈式 ImGui 菜单项。
/// @param label 菜单项显示文本和 ImGui ID。
/// @param icon 图标文本，可为空。
/// @param shortcut 快捷键显示文本，可为空。
/// @param selected 当前选中状态。
/// @param enabled 是否允许点击。
/// @return 菜单项本帧被激活时返回 true。
/// @warning UI 热路径：每帧菜单绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackMenuItemEx(const char* label, const char* icon,
                        const char* shortcut, bool selected, bool enabled)
{
    ImGuiStorage* storage          = ImGui::GetStateStorage();
    const ImGuiID id               = ImGui::GetID(label);
    const float   hoverAmount      = updateButtonHoverAmount(id, storage);
    const int     pushedColorCount = pushAnimatedMenuColors(hoverAmount);
    const bool    clicked =
        ImGui::MenuItemEx(label, icon, shortcut, selected, enabled);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(pushedColorCount);
    finishButtonFeedback(id, clicked, storage, hovered);
    return clicked;
}

}  // namespace MMM::UI
