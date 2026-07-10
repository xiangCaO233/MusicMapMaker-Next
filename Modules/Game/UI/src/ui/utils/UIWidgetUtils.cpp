#include "ui/utils/UIWidgetUtils.h"

#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui_internal.h"
#include "ui/layout/CLayDefs.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace MMM::UI::Utils
{

/// @brief Tooltip 动画进度存储键的盐值。
constexpr ImGuiID TOOLTIP_ANIM_AMOUNT_KEY_SALT = 0x6D6D5421u;

/// @brief Tooltip 最后一次更新帧存储键的盐值。
constexpr ImGuiID TOOLTIP_LAST_FRAME_KEY_SALT = 0x6D6D5422u;

/// @brief Tooltip 弹出动画的横向位移像素。
constexpr float TOOLTIP_SLIDE_X = 6.0f;

/// @brief Tooltip 弹出动画的纵向位移像素。
constexpr float TOOLTIP_SLIDE_Y = 4.0f;

/// @brief 项目纵向滚动区域统一使用的最小滚动条宽度，单位为逻辑像素。
constexpr float VERTICAL_SCROLLBAR_MIN_WIDTH = 18.0f;

/// @brief 计算带盐的 Tooltip 状态键。
/// @param id 控件 ID。
/// @param salt 用途盐值。
/// @return 用于 ImGuiStorage 的状态键。
ImGuiID makeTooltipStorageKey(ImGuiID id, ImGuiID salt)
{
    return id ^ salt;
}

/// @brief 读取统一 UI 动画过渡速度。
/// @return 每秒推进的线性动画进度。
/// @warning UI 热路径：只读取当前内存配置，不执行文件 IO。
float getUiAnimationTransitionSpeed()
{
    return Config::AppConfig::instance()
        .getEditorSettings()
        .aesthetics.animationTransitionSpeed();
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
    const float step = std::max(0.0f, ImGui::GetIO().DeltaTime) *
                       getUiAnimationTransitionSpeed();
    amount           = std::min(1.0f, amount + step);

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
    const bool        clicked      = ::MMM::UI::FeedbackSelectable(
        selectableId.c_str(), false, 0, ImVec2(width, height));

    if ( !tooltip.empty() ) {
        renderTooltip(tooltip.c_str());
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

    const bool open = ::MMM::UI::FeedbackCollapsingHeader(
        label, flags | (*p_state ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    *p_state = open;

    ImGui::PopStyleVar();
    win->WorkRect.Max.x = savedWRMaxX;

    return open;
}

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
                                     ImGuiTreeNodeFlags flags)
{
    ImGui::SetCursorScreenPos({ r.x, r.y });

    ImGuiWindow* win         = ImGui::GetCurrentWindow();
    float        savedWRMaxX = win->WorkRect.Max.x;
    win->WorkRect.Max.x      = r.x + r.width;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });

    const std::string hiddenLabel = "##" + id;
    const bool        open        = ::MMM::UI::FeedbackCollapsingHeader(
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
        renderTooltip(tooltip.c_str());
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

/// @brief 使用当前窗口 DPI 缩放推入纵向滚动条宽度样式。
VerticalScrollbarStyleScope::VerticalScrollbarStyleScope()
    : VerticalScrollbarStyleScope(
          Config::AppConfig::instance().getWindowContentScale())
{
}

/// @brief 使用指定 DPI 缩放推入纵向滚动条宽度样式。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：只读取当前主题并压入一个 ImGui 样式变量。
VerticalScrollbarStyleScope::VerticalScrollbarStyleScope(float dpiScale)
{
    const float scrollbarSize = std::max(
        ImGui::GetStyle().ScrollbarSize,
        std::floor(VERTICAL_SCROLLBAR_MIN_WIDTH * std::max(dpiScale, 1.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, scrollbarSize);
}

/// @brief 恢复进入作用域前的滚动条宽度样式。
/// @warning UI 热路径：只弹出一个 ImGui 样式变量。
VerticalScrollbarStyleScope::~VerticalScrollbarStyleScope()
{
    ImGui::PopStyleVar();
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

    const bool wasOpenBeforeBegin = pOpen != nullptr && *pOpen;
    pushTitleFont();
    const bool opened = ImGui::BeginPopupModal(name, pOpen, flags);
    if ( opened ) {
        ::MMM::UI::FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, pOpen);
    }
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

    const bool wasOpenBeforeBegin = pOpen != nullptr && *pOpen;
    pushTitleFont();
    const bool opened = ImGui::Begin(name, pOpen, flags);
    ::MMM::UI::FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, pOpen);
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

/// @brief 鼠标按下时播放的皮肤音频 ID。
constexpr const char* MOUSE_DOWN_SFX_KEY = "ui.click_down";

/// @brief 鼠标松开时播放的皮肤音频 ID。
constexpr const char* MOUSE_UP_SFX_KEY = "ui.click_up";

/// @brief Slider 拖动变化时播放的皮肤音频 ID。
constexpr const char* SLIDER_CHANGE_SFX_KEY = "ui.slider";

/// @brief 警告弹窗打开时播放的皮肤音频 ID。
constexpr const char* WARNING_NOTICE_SFX_KEY = "ui.notice";

/// @brief 悬浮音效的单次触发音量倍率。
constexpr float BUTTON_HOVER_SFX_VOLUME = 0.22f;

/// @brief 点击音效的单次触发音量倍率。
constexpr float BUTTON_CLICK_SFX_VOLUME = 0.36f;

/// @brief 鼠标按下音效的单次触发音量倍率。
constexpr float MOUSE_DOWN_SFX_VOLUME = 0.36f;

/// @brief 鼠标松开音效的单次触发音量倍率。
constexpr float MOUSE_UP_SFX_VOLUME = 0.34f;

/// @brief Slider 变化音效的单次触发音量倍率。
constexpr float SLIDER_CHANGE_SFX_VOLUME = 0.24f;

/// @brief 警告弹窗提示音的单次触发音量倍率。
constexpr float WARNING_NOTICE_SFX_VOLUME = 1.0f;

/// @brief Slider 最低点对应的音高偏移，单位为半音。
constexpr double SLIDER_MIN_PITCH_SEMITONES = -12.0;

/// @brief Slider 最高点对应的音高偏移，单位为半音。
constexpr double SLIDER_MAX_PITCH_SEMITONES = 12.0;

/// @brief Slider 拖动音效最小触发间隔，避免每帧堆叠播放。
constexpr float SLIDER_SFX_MIN_INTERVAL_SECONDS = 0.045f;

/// @brief 统一 UI 交互音效是否允许播放。
bool interactionFeedbackEnabled = true;

/// @brief 按钮悬浮状态存储键的盐值。
constexpr ImGuiID BUTTON_HOVERED_KEY_SALT = 0x6D6D4821u;

/// @brief 按钮悬浮过渡进度存储键的盐值。
constexpr ImGuiID BUTTON_HOVER_AMOUNT_KEY_SALT = 0x6D6D4822u;

/// @brief 按钮最后一次绘制帧存储键的盐值。
constexpr ImGuiID BUTTON_LAST_FRAME_KEY_SALT = 0x6D6D4823u;

/// @brief 按钮点击音效最后一次触发帧存储键的盐值。
constexpr ImGuiID BUTTON_CLICK_FRAME_KEY_SALT = 0x6D6D4824u;

/// @brief Slider 音效最后一次触发时间存储键的盐值。
constexpr ImGuiID SLIDER_SFX_LAST_TIME_KEY_SALT = 0x6D6D5321u;

/// @brief 菜单打开状态存储键的盐值。
constexpr ImGuiID MENU_OPEN_KEY_SALT = 0x6D6D4D21u;

/// @brief 菜单弹窗动画进度存储键的盐值。
constexpr ImGuiID MENU_POPUP_AMOUNT_KEY_SALT = 0x6D6D4D22u;

/// @brief 菜单弹窗最后一次绘制帧存储键的盐值。
constexpr ImGuiID MENU_POPUP_LAST_FRAME_KEY_SALT = 0x6D6D4D23u;

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

/// @brief 查询统一 UI 交互音效当前是否允许播放。
/// @return 当前帧允许播放时返回 true。
/// @warning UI 热路径：只读取内存标志，不执行资源操作。
bool isInteractionFeedbackEnabled()
{
    return interactionFeedbackEnabled;
}

/// @brief 将数值限制到 0 到 1。
/// @param value 输入值。
/// @return 限制后的值。
float saturate(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

/// @brief 根据范围计算 Slider 当前百分比。
/// @param value 当前值。
/// @param minValue 最小值。
/// @param maxValue 最大值。
/// @return 当前值在范围内的百分比。
float calcSliderPercent(float value, float minValue, float maxValue)
{
    const float range = maxValue - minValue;
    if ( std::abs(range) <= 0.000001f ) {
        return 0.5f;
    }
    return saturate((value - minValue) / range);
}

/// @brief 将 Slider 百分比映射到音高半音偏移。
/// @param percent 当前百分比。
/// @return 音高偏移，单位为半音。
double calcSliderPitchSemitones(float percent)
{
    const double t = static_cast<double>(saturate(percent));
    return SLIDER_MIN_PITCH_SEMITONES +
           (SLIDER_MAX_PITCH_SEMITONES - SLIDER_MIN_PITCH_SEMITONES) * t;
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

    const int currentFrame = ImGui::GetFrameCount();
    const int lastFrame    = storage->GetInt(lastFrameKey, -1);
    if ( lastFrame == currentFrame ) {
        return storage->GetFloat(amountKey, 0.0f);
    }
    const bool wasDrawnLastFrame = lastFrame == currentFrame - 1;
    const bool wasHovered =
        wasDrawnLastFrame && storage->GetInt(hoveredKey, 0) != 0;
    const ImGuiID openKey = makeButtonStorageKey(id, MENU_OPEN_KEY_SALT);
    const bool  wasOpen = wasDrawnLastFrame && storage->GetInt(openKey, 0) != 0;
    const float target  = (wasHovered || wasOpen) ? 1.0f : 0.0f;
    float       amount =
        wasDrawnLastFrame ? storage->GetFloat(amountKey, 0.0f) : 0.0f;
    const float step = std::min(1.0f,
                                std::max(0.0f, ImGui::GetIO().DeltaTime) *
                                    Utils::getUiAnimationTransitionSpeed());

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
    const int  currentFrame           = ImGui::GetFrameCount();
    const int  lastFrame              = storage->GetInt(lastFrameKey, -1);
    const bool wasDrawnLastFrame      = lastFrame == currentFrame - 1;
    const bool wasUpdatedCurrentFrame = lastFrame == currentFrame;
    const bool storedHovered          = storage->GetInt(hoveredKey, 0) != 0;
    const bool wasHovered             = wasDrawnLastFrame && storedHovered;
    const bool hoveredThisFrame =
        isHovered || (wasUpdatedCurrentFrame && storedHovered);
    const bool feedbackEnabled = isInteractionFeedbackEnabled();

    if ( feedbackEnabled && isHovered && !wasHovered &&
         !(wasUpdatedCurrentFrame && storedHovered) ) {
        Audio::AudioManager::instance().playSoundEffect(
            BUTTON_HOVER_SFX_KEY, BUTTON_HOVER_SFX_VOLUME);
    }

    const ImGuiID clickFrameKey =
        makeButtonStorageKey(id, BUTTON_CLICK_FRAME_KEY_SALT);
    const bool mouseEdgeTriggered =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if ( feedbackEnabled && clicked && !mouseEdgeTriggered &&
         storage->GetInt(clickFrameKey, -1) != currentFrame ) {
        Audio::AudioManager::instance().playSoundEffect(
            BUTTON_CLICK_SFX_KEY, BUTTON_CLICK_SFX_VOLUME);
        storage->SetInt(clickFrameKey, currentFrame);
    }

    storage->SetInt(hoveredKey, hoveredThisFrame ? 1 : 0);
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

/// @brief 压入透明 Header 颜色，避免 ImGui 默认方角高亮遮住自绘圆角背景。
/// @return 压入的样式颜色数量。
/// @warning UI 热路径：只操作 ImGui 样式栈。
int pushTransparentHeaderColors()
{
    const ImVec4 transparent{ 0.0f, 0.0f, 0.0f, 0.0f };
    ImGui::PushStyleColor(ImGuiCol_Header, transparent);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
    return 3;
}

/// @brief 圆角高亮背景绘制通道。
struct RoundedHighlightLayer {
    /// @brief 目标窗口 DrawList。
    ImDrawList* drawList{ nullptr };

    /// @brief 局部通道拆分器，避免和 ImGui 内部表格通道冲突。
    ImDrawListSplitter splitter;
};

/// @brief 开启圆角高亮的背景/前景绘制通道。
/// @param layer 输出通道状态。
/// @warning UI 热路径：只拆分当前窗口 DrawList 通道。
void beginRoundedHighlightLayer(RoundedHighlightLayer* layer)
{
    if ( !layer ) {
        return;
    }

    layer->drawList = ImGui::GetWindowDrawList();
    layer->splitter.Split(layer->drawList, 2);
    layer->splitter.SetCurrentChannel(layer->drawList, 1);
}

/// @brief 按当前样式生成圆角高亮颜色。
/// @param hoverAmount 悬浮过渡进度。
/// @param selected 当前是否为选中态。
/// @param hovered 当前是否为悬浮态。
/// @param active 当前是否为按下或打开态。
/// @return 应绘制的颜色，透明表示不绘制。
/// @warning UI 热路径：只读取 ImGui 样式颜色并做常量计算。
ImVec4 calcRoundedHighlightColor(float hoverAmount, bool selected, bool hovered,
                                 bool active)
{
    if ( !selected && !hovered && !active && hoverAmount <= 0.001f ) {
        return ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const ImVec4 baseColor   = ImGui::GetStyleColorVec4(ImGuiCol_Header);
    const ImVec4 hoverColor  = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    const ImVec4 activeColor = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
    const float  amount =
        (hovered || active) ? std::max(hoverAmount, 0.001f) : hoverAmount;
    ImVec4 color = active ? lerpColor(baseColor, activeColor, amount)
                          : lerpColor(baseColor, hoverColor, amount);

    if ( !selected && !hovered && !active ) {
        color.w *= easeOutCubic(amount);
    }
    return color;
}

/// @brief 结束圆角高亮绘制通道，并在内容下方绘制圆角背景。
/// @param layer beginRoundedHighlightLayer 输出的通道状态。
/// @param rect 高亮矩形。
/// @param color 高亮颜色。
/// @warning UI 热路径：只向当前窗口 DrawList 添加一个圆角矩形。
void endRoundedHighlightLayer(RoundedHighlightLayer* layer, const ImRect& rect,
                              const ImVec4& color)
{
    if ( !layer || !layer->drawList ) {
        return;
    }

    layer->splitter.SetCurrentChannel(layer->drawList, 0);
    if ( color.w > 0.001f ) {
        layer->drawList->AddRectFilled(
            rect.Min,
            rect.Max,
            ImGui::GetColorU32(color),
            std::max(0.0f, ImGui::GetStyle().FrameRounding));
    }
    layer->splitter.Merge(layer->drawList);
}

/// @brief 判断 Selectable 是否需要使用表格整行背景。
/// @param flags Selectable 标志。
/// @return 位于表格内且需要跨列时返回 true。
/// @warning UI 热路径：只读取当前 ImGui 表格指针。
bool shouldUseTableRowHighlight(ImGuiSelectableFlags flags)
{
    return (flags & ImGuiSelectableFlags_SpanAllColumns) != 0 && GImGui &&
           GImGui->CurrentTable != nullptr;
}

/// @brief 写入表格整行高亮背景。
/// @param color 背景颜色。
/// @warning UI 热路径：只调用 ImGui 表格行背景 API。
void setTableRowHighlight(const ImVec4& color)
{
    if ( color.w <= 0.001f || !GImGui || !GImGui->CurrentTable ) {
        return;
    }

    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                           ImGui::GetColorU32(color));
}

/// @brief 压入滑块、拖拽输入等框式控件的悬浮颜色过渡样式。
/// @param hoverAmount 当前悬浮过渡进度。
/// @param includeGrab 是否同时处理滑块抓手颜色。
/// @return 压入的样式颜色数量。
/// @warning UI 热路径：只操作 ImGui 样式栈。
int pushAnimatedFrameColors(float hoverAmount, bool includeGrab)
{
    const ImVec4 frameBase  = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    const ImVec4 frameHover = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered);
    const ImVec4 frameActive = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive);
    const ImVec4 frameMixed  = lerpColor(frameBase, frameHover, hoverAmount);
    const ImVec4 activeMixed = lerpColor(frameBase, frameActive, hoverAmount);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, frameMixed);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frameMixed);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, activeMixed);
    int pushedCount = 3;

    if ( includeGrab ) {
        const ImVec4 grabBase = ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);
        const ImVec4 grabActive =
            ImGui::GetStyleColorVec4(ImGuiCol_SliderGrabActive);
        const ImVec4 grabMixed = lerpColor(grabBase, grabActive, hoverAmount);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, grabMixed);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, grabMixed);
        pushedCount += 2;
    }

    return pushedCount;
}

/// @brief 压入 Combo 控件的悬浮颜色过渡样式。
/// @param hoverAmount 当前悬浮过渡进度。
/// @return 压入的样式颜色数量。
/// @warning UI 热路径：只操作 ImGui 样式栈。
int pushAnimatedComboColors(float hoverAmount)
{
    int pushedCount = pushAnimatedFrameColors(hoverAmount, false);
    pushedCount += pushAnimatedButtonColors(hoverAmount);
    return pushedCount;
}

/// @brief 判断上一条 Item 是否应触发点击音效。
/// @return 鼠标点击或控件激活时返回 true。
/// @warning UI 热路径：只读取 ImGui 上一条 Item 状态。
bool isLastItemFeedbackActivated()
{
    return ImGui::IsItemClicked(ImGuiMouseButton_Left) ||
           ImGui::IsItemActivated();
}

/// @brief 写回上一条 Item 的统一反馈状态。
/// @param id 独立反馈状态 ID。
/// @param clicked 上一条 Item 本帧是否激活。
/// @warning UI 热路径：只读取上一条 Item 状态并触发已预加载音效池。
void finishLastItemFeedback(ImGuiID id, bool clicked)
{
    finishButtonFeedback(
        id, clicked, ImGui::GetStateStorage(), ImGui::IsItemHovered());
}

/// @brief 在 Slider 值变化时按当前百分比触发变调音效。
/// @param id Slider 的 ImGui ID。
/// @param changed 本帧 Slider 值是否变化。
/// @param percent 当前值在可调范围内的百分比。
/// @warning UI 热路径：只访问 ImGuiStorage 并触发已预加载 SFX pool。
void playSliderChangeFeedback(ImGuiID id, bool changed, float percent)
{
    if ( !changed || !isInteractionFeedbackEnabled() ) return;

    ImGuiStorage* storage = ImGui::GetStateStorage();
    if ( !storage ) return;

    const float   currentTime = static_cast<float>(ImGui::GetTime());
    const ImGuiID timeKey =
        makeButtonStorageKey(id, SLIDER_SFX_LAST_TIME_KEY_SALT);
    const float lastTime = storage->GetFloat(timeKey, -1000.0f);
    if ( currentTime - lastTime < SLIDER_SFX_MIN_INTERVAL_SECONDS ) {
        return;
    }

    Audio::AudioManager::instance().playSoundEffect(
        SLIDER_CHANGE_SFX_KEY,
        SLIDER_CHANGE_SFX_VOLUME,
        calcSliderPitchSemitones(percent));
    storage->SetFloat(timeKey, currentTime);
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
    const float step   = std::min(1.0f,
                                  std::max(0.0f, ImGui::GetIO().DeltaTime) *
                                      Utils::getUiAnimationTransitionSpeed());

    if ( amount < target ) {
        amount = std::min(target, amount + step);
    } else {
        amount = std::max(target, amount - step);
    }

    storage->SetFloat(amountKey, amount);
    storage->SetInt(lastFrameKey, currentFrame);
    return amount;
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

/// @brief 读取指定窗口的 ImGui 状态存储。
/// @param window 目标 ImGui 窗口。
/// @return 窗口状态存储，窗口无效时返回当前窗口状态存储。
/// @warning UI 热路径：只返回已存在的 ImGuiStorage 指针。
ImGuiStorage* getWindowStorage(ImGuiWindow* window)
{
    return window ? &window->StateStorage : ImGui::GetStateStorage();
}

/// @brief 计算当前窗口浮动标题栏关闭按钮 ID。
/// @param window 当前 ImGui 窗口。
/// @return 关闭按钮 ID，无法计算时返回 0。
/// @warning UI 热路径：只读取当前 ImGuiWindow 状态。
ImGuiID getFloatingWindowCloseButtonId(ImGuiWindow* window)
{
    if ( !window || !window->HasCloseButton ) {
        return 0;
    }
    return window->GetID("#CLOSE");
}

/// @brief 计算 dock 标签自身关闭按钮 ID。
/// @param window 当前 ImGui 窗口。
/// @return dock 标签关闭按钮 ID，无法计算时返回 0。
/// @warning UI 热路径：只读取当前 ImGuiWindow 状态并复用 ImGui 内部哈希。
ImGuiID getDockTabCloseButtonId(ImGuiWindow* window)
{
    if ( !window || !window->HasCloseButton || !window->DockIsActive ||
         !window->DockNode || !window->DockNode->TabBar ) {
        return 0;
    }
    return ImHashStr("#CLOSE", 0, window->ID);
}

/// @brief 计算 dock 节点标题栏关闭按钮 ID。
/// @param node 当前窗口所属 DockNode。
/// @return dock 节点关闭按钮 ID，无法计算时返回 0。
/// @warning UI 热路径：只读取 DockNode 和 HostWindow 状态。
ImGuiID getDockNodeCloseButtonId(ImGuiDockNode* node)
{
    if ( !node || !node->HasCloseButton ) {
        return 0;
    }
    return ImHashStr("#CLOSE", 0, node->ID);
}

/// @brief 计算 dock 节点标题栏菜单按钮 ID。
/// @param node 当前窗口所属 DockNode。
/// @return dock 节点菜单按钮 ID，无法计算时返回 0。
/// @warning UI 热路径：只读取 DockNode 状态并复用 ImGui 内部哈希。
ImGuiID getDockNodeMenuButtonId(ImGuiDockNode* node)
{
    if ( !node || !node->HasWindowMenuButton ) {
        return 0;
    }
    return ImGui::DockNodeGetWindowMenuButtonId(node);
}

/// @brief 处理一个原生按钮候选的反馈。
/// @param id 按钮 ImGui ID。
/// @param storage 反馈状态存储。
/// @param hovered 当前帧是否悬浮。
/// @param clicked 当前帧是否触发关闭。
/// @warning UI 热路径：只更新反馈状态并触发已预加载音效池。
void applyCloseButtonFeedback(ImGuiID id, ImGuiStorage* storage, bool hovered,
                              bool clicked)
{
    if ( id == 0 || !storage ) {
        return;
    }

    updateButtonHoverAmount(id, storage);
    finishButtonFeedback(id, clicked, storage, hovered);
}

/// @brief 判断一个原生按钮本帧是否按下。
/// @param id 原生按钮 ID。
/// @return 鼠标按下发生在该按钮上时返回 true。
/// @warning UI 热路径：只读取 ImGui 当前交互 ID 和鼠标状态。
bool isNativeButtonClicked(ImGuiID id)
{
    const ImGuiContext& g = *GImGui;
    return g.HoveredId == id && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

/// @brief 处理一个 dock 节点原生按钮候选的反馈。
/// @param id 原生按钮 ID。
/// @param storage 反馈状态存储。
/// @warning UI 热路径：只更新反馈状态并触发已预加载音效池。
void applyDockNodeNativeButtonFeedback(ImGuiID id, ImGuiStorage* storage)
{
    if ( id == 0 || !storage ) {
        return;
    }

    const ImGuiContext& g       = *GImGui;
    const bool          hovered = g.HoveredId == id || g.ActiveId == id;
    const bool          clicked = isNativeButtonClicked(id);
    applyCloseButtonFeedback(id, storage, hovered, clicked);
}

/// @brief 递归处理 dock 树内的节点按钮反馈。
/// @param node 当前 DockNode。
/// @warning UI 热路径：只沿当前 dock 树递归，节点数量与可见停靠窗口数量同阶。
void feedbackDockNodeControlsRecursive(ImGuiDockNode* node)
{
    if ( !node ) {
        return;
    }

    feedbackDockNodeControlsRecursive(node->ChildNodes[0]);
    feedbackDockNodeControlsRecursive(node->ChildNodes[1]);

    if ( !node->HostWindow ) {
        return;
    }

    ImGuiStorage* storage = getWindowStorage(node->HostWindow);

    const ImGuiID menuButtonId = getDockNodeMenuButtonId(node);
    if ( menuButtonId != 0 ) {
        applyDockNodeNativeButtonFeedback(menuButtonId, storage);
    }

    const ImGuiID closeButtonId = getDockNodeCloseButtonId(node);
    if ( closeButtonId != 0 ) {
        applyDockNodeNativeButtonFeedback(closeButtonId, storage);
    }
}

}  // namespace

/// @brief 设置统一 UI 交互音效是否允许播放。
/// @param enabled 是否允许 hover、点击、鼠标边沿和滑块反馈音效。
/// @warning UI 热路径：每帧写入一次，只更新内存标志，不执行资源操作。
void SetInteractionFeedbackEnabled(bool enabled)
{
    interactionFeedbackEnabled = enabled;
}

/// @brief 打开警告弹窗，并在弹窗由关闭切换为打开时播放一次提示音。
/// @param popupId 弹窗显示文本和 ImGui ID。
/// @warning UI 低频路径：只查询 ImGui 弹窗状态并触发已预加载 SFX pool，
/// 不执行资源加载。
void OpenWarningPopup(const char* popupId)
{
    if ( !ImGui::IsPopupOpen(popupId) && isInteractionFeedbackEnabled() ) {
        Audio::AudioManager::instance().playSoundEffect(
            WARNING_NOTICE_SFX_KEY, WARNING_NOTICE_SFX_VOLUME);
    }
    ImGui::OpenPopup(popupId);
}

/// @brief 处理全局鼠标左右键按下与松开音效。
/// @warning UI 热路径：每帧调用一次，只读取 ImGui 鼠标边沿状态并触发已预加载
/// SFX pool，禁止执行资源加载。
void ProcessGlobalMouseFeedback()
{
    if ( !isInteractionFeedbackEnabled() ) {
        return;
    }

    if ( ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
        Audio::AudioManager::instance().playSoundEffect(MOUSE_DOWN_SFX_KEY,
                                                        MOUSE_DOWN_SFX_VOLUME);
    }

    if ( ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
         ImGui::IsMouseReleased(ImGuiMouseButton_Right) ) {
        PlayInteractionMouseUpFeedback();
    }
}

/// @brief 播放统一的鼠标松开反馈音效。
/// @warning UI 热路径：只触发已预加载 SFX pool，不执行资源加载。
void PlayInteractionMouseUpFeedback()
{
    if ( !isInteractionFeedbackEnabled() ) {
        return;
    }

    Audio::AudioManager::instance().playSoundEffect(MOUSE_UP_SFX_KEY,
                                                    MOUSE_UP_SFX_VOLUME);
}

/// @brief 绘制带统一反馈的 ImGui CollapsingHeader。
/// @param label Header 显示文本和 ImGui ID。
/// @param flags Header 标志。
/// @return Header 本帧展开时返回 true。
/// @warning UI 热路径：每帧 Header 绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags)
{
    ImGuiStorage*         storage     = ImGui::GetStateStorage();
    const ImGuiID         id          = ImGui::GetID(label);
    const float           hoverAmount = updateButtonHoverAmount(id, storage);
    RoundedHighlightLayer highlightLayer;
    beginRoundedHighlightLayer(&highlightLayer);
    const int    pushedColorCount = pushTransparentHeaderColors();
    const bool   open             = ImGui::CollapsingHeader(label, flags);
    const bool   hovered          = ImGui::IsItemHovered();
    const bool   active           = ImGui::IsItemActive();
    const bool   clicked          = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopStyleColor(pushedColorCount);
    endRoundedHighlightLayer(
        &highlightLayer,
        itemRect,
        calcRoundedHighlightColor(hoverAmount, open, hovered, active));
    finishButtonFeedback(id, clicked, storage, hovered);
    return open;
}

/// @brief 绘制带统一反馈的 ImGui Checkbox。
/// @param label Checkbox 显示文本和 ImGui ID。
/// @param value 当前布尔值指针。
/// @return 本帧值变化时返回 true。
/// @warning UI 热路径：每帧勾选控件绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackCheckbox(const char* label, bool* value)
{
    const ImGuiID id            = ImGui::GetID(label);
    const float   hoverAmount   = updateButtonHoverAmount(id);
    const int  pushedColorCount = pushAnimatedFrameColors(hoverAmount, false);
    const bool changed          = ImGui::Checkbox(label, value);
    const bool clicked          = isLastItemFeedbackActivated();
    ImGui::PopStyleColor(pushedColorCount);
    finishLastItemFeedback(id, clicked);
    return changed;
}

/// @brief 绘制带统一反馈的 ImGui RadioButton。
/// @param label RadioButton 显示文本和 ImGui ID。
/// @param active 当前是否选中。
/// @return 本帧被激活时返回 true。
/// @warning UI 热路径：每帧单选控件绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackRadioButton(const char* label, bool active)
{
    const ImGuiID id            = ImGui::GetID(label);
    const float   hoverAmount   = updateButtonHoverAmount(id);
    const int  pushedColorCount = pushAnimatedFrameColors(hoverAmount, false);
    const bool clicked          = ImGui::RadioButton(label, active);
    const bool feedbackClicked  = isLastItemFeedbackActivated();
    ImGui::PopStyleColor(pushedColorCount);
    finishLastItemFeedback(id, feedbackClicked);
    return clicked;
}

/// @brief 绘制带统一反馈的 ImGui RadioButton。
/// @param label RadioButton 显示文本和 ImGui ID。
/// @param value 当前整型值指针。
/// @param buttonValue 本按钮代表的值。
/// @return 本帧值变化时返回 true。
/// @warning UI 热路径：每帧单选控件绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackRadioButton(const char* label, int* value, int buttonValue)
{
    const ImGuiID id            = ImGui::GetID(label);
    const float   hoverAmount   = updateButtonHoverAmount(id);
    const int  pushedColorCount = pushAnimatedFrameColors(hoverAmount, false);
    const bool changed          = ImGui::RadioButton(label, value, buttonValue);
    const bool clicked          = isLastItemFeedbackActivated();
    ImGui::PopStyleColor(pushedColorCount);
    finishLastItemFeedback(id, clicked);
    return changed;
}

/// @brief 绘制带统一反馈的 ImGui Selectable。
/// @param label Selectable 显示文本和 ImGui ID。
/// @param selected 当前选中状态。
/// @param flags Selectable 标志。
/// @param size Selectable 尺寸。
/// @return 本帧被激活时返回 true。
/// @warning UI 热路径：每帧列表绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSelectable(const char* label, bool selected,
                        ImGuiSelectableFlags flags, const ImVec2& size)
{
    ImGuiStorage* storage              = ImGui::GetStateStorage();
    const ImGuiID id                   = ImGui::GetID(label);
    const float   hoverAmount          = updateButtonHoverAmount(id, storage);
    const bool    useTableRowHighlight = shouldUseTableRowHighlight(flags);
    RoundedHighlightLayer highlightLayer;
    if ( !useTableRowHighlight ) {
        beginRoundedHighlightLayer(&highlightLayer);
    }
    const int    pushedColorCount = pushTransparentHeaderColors();
    const bool   clicked = ImGui::Selectable(label, selected, flags, size);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   active  = ImGui::IsItemActive();
    const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopStyleColor(pushedColorCount);
    const ImVec4 highlightColor =
        calcRoundedHighlightColor(hoverAmount, selected, hovered, active);
    if ( useTableRowHighlight ) {
        setTableRowHighlight(highlightColor);
    } else {
        endRoundedHighlightLayer(&highlightLayer, itemRect, highlightColor);
    }
    finishButtonFeedback(id, clicked, storage, hovered);
    return clicked;
}

/// @brief 绘制带统一反馈的 ImGui Selectable。
/// @param label Selectable 显示文本和 ImGui ID。
/// @param pSelected 可选选中状态指针。
/// @param flags Selectable 标志。
/// @param size Selectable 尺寸。
/// @return 本帧被激活时返回 true。
/// @warning UI 热路径：每帧列表绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSelectable(const char* label, bool* pSelected,
                        ImGuiSelectableFlags flags, const ImVec2& size)
{
    ImGuiStorage* storage              = ImGui::GetStateStorage();
    const ImGuiID id                   = ImGui::GetID(label);
    const float   hoverAmount          = updateButtonHoverAmount(id, storage);
    const bool    useTableRowHighlight = shouldUseTableRowHighlight(flags);
    RoundedHighlightLayer highlightLayer;
    if ( !useTableRowHighlight ) {
        beginRoundedHighlightLayer(&highlightLayer);
    }
    const int    pushedColorCount = pushTransparentHeaderColors();
    const bool   clicked  = ImGui::Selectable(label, pSelected, flags, size);
    const bool   hovered  = ImGui::IsItemHovered();
    const bool   active   = ImGui::IsItemActive();
    const bool   selected = pSelected && *pSelected;
    const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopStyleColor(pushedColorCount);
    const ImVec4 highlightColor =
        calcRoundedHighlightColor(hoverAmount, selected, hovered, active);
    if ( useTableRowHighlight ) {
        setTableRowHighlight(highlightColor);
    } else {
        endRoundedHighlightLayer(&highlightLayer, itemRect, highlightColor);
    }
    finishButtonFeedback(id, clicked, storage, hovered);
    return clicked;
}

/// @brief 绘制带统一反馈的 ImGui BeginCombo。
/// @param label Combo 显示文本和 ImGui ID。
/// @param previewValue 当前预览文本。
/// @param flags Combo 标志。
/// @return 弹出列表打开时返回 true。
/// @warning UI 热路径：每帧 Combo 绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackBeginCombo(const char* label, const char* previewValue,
                        ImGuiComboFlags flags)
{
    ImGuiStorage* storage          = ImGui::GetStateStorage();
    const ImGuiID id               = ImGui::GetID(label);
    const float   hoverAmount      = updateButtonHoverAmount(id, storage);
    const int     pushedColorCount = pushAnimatedComboColors(hoverAmount);
    const bool    open    = ImGui::BeginCombo(label, previewValue, flags);
    const bool    clicked = isLastItemFeedbackActivated();
    const bool    hovered = ImGui::IsItemHovered() || open;
    ImGui::PopStyleColor(pushedColorCount);
    finishButtonFeedback(id, clicked, storage, hovered);
    return open;
}

/// @brief 结束由 FeedbackBeginCombo 打开的 Combo。
/// @warning UI 热路径：弹出列表绘制结束时调用 ImGui::EndCombo。
void FeedbackEndCombo()
{
    ImGui::EndCombo();
}

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
                   int popupMaxHeightInItems)
{
    if ( !currentItem || !items || itemsCount <= 0 ) {
        return false;
    }

    const int   previewIndex = std::clamp(*currentItem, 0, itemsCount - 1);
    const char* previewValue = items[previewIndex] ? items[previewIndex] : "";
    bool        changed      = false;

    if ( popupMaxHeightInItems > 0 ) {
        const float maxHeight = ImGui::GetTextLineHeightWithSpacing() *
                                    static_cast<float>(popupMaxHeightInItems) +
                                ImGui::GetStyle().WindowPadding.y * 2.0f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                            ImVec2(FLT_MAX, maxHeight));
    }

    if ( FeedbackBeginCombo(label, previewValue) ) {
        for ( int i = 0; i < itemsCount; ++i ) {
            const bool  selected  = i == *currentItem;
            const char* itemLabel = items[i] ? items[i] : "";
            ImGui::PushID(i);
            if ( FeedbackSelectable(itemLabel, selected) ) {
                *currentItem = i;
                changed      = true;
            }
            if ( selected ) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        FeedbackEndCombo();
    }

    return changed;
}

/// @brief 绘制带统一反馈的 ImGui Float 滑块。
/// @warning UI 热路径：每帧滑块绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSliderFloat(const char* label, float* value, float minValue,
                         float maxValue, const char* format,
                         ImGuiSliderFlags flags)
{
    const ImGuiID id               = ImGui::GetID(label);
    const float   hoverAmount      = updateButtonHoverAmount(id);
    const int     pushedColorCount = pushAnimatedFrameColors(hoverAmount, true);
    const bool    changed =
        ImGui::SliderFloat(label, value, minValue, maxValue, format, flags);
    const bool  clicked = isLastItemFeedbackActivated();
    const float percent = calcSliderPercent(*value, minValue, maxValue);
    ImGui::PopStyleColor(pushedColorCount);
    playSliderChangeFeedback(id, changed, percent);
    finishLastItemFeedback(id, clicked);
    return changed;
}

/// @brief 绘制带统一反馈的 ImGui Int 滑块。
/// @warning UI 热路径：每帧滑块绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackSliderInt(const char* label, int* value, int minValue,
                       int maxValue, const char* format, ImGuiSliderFlags flags)
{
    const ImGuiID id               = ImGui::GetID(label);
    const float   hoverAmount      = updateButtonHoverAmount(id);
    const int     pushedColorCount = pushAnimatedFrameColors(hoverAmount, true);
    const bool    changed =
        ImGui::SliderInt(label, value, minValue, maxValue, format, flags);
    const bool  clicked = isLastItemFeedbackActivated();
    const float percent = calcSliderPercent(static_cast<float>(*value),
                                            static_cast<float>(minValue),
                                            static_cast<float>(maxValue));
    ImGui::PopStyleColor(pushedColorCount);
    playSliderChangeFeedback(id, changed, percent);
    finishLastItemFeedback(id, clicked);
    return changed;
}

/// @brief 绘制带统一反馈的 ImGui 垂直 Float 滑块。
/// @warning UI 热路径：每帧滑块绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackVSliderFloat(const char* label, const ImVec2& size, float* value,
                          float minValue, float maxValue, const char* format,
                          ImGuiSliderFlags flags)
{
    const ImGuiID id               = ImGui::GetID(label);
    const float   hoverAmount      = updateButtonHoverAmount(id);
    const int     pushedColorCount = pushAnimatedFrameColors(hoverAmount, true);
    const bool    changed          = ImGui::VSliderFloat(
        label, size, value, minValue, maxValue, format, flags);
    const bool  clicked = isLastItemFeedbackActivated();
    const float percent = calcSliderPercent(*value, minValue, maxValue);
    ImGui::PopStyleColor(pushedColorCount);
    playSliderChangeFeedback(id, changed, percent);
    finishLastItemFeedback(id, clicked);
    return changed;
}

/// @brief 绘制带统一反馈的 ImGui Float 拖拽输入。
/// @warning UI 热路径：每帧拖拽输入绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackDragFloat(const char* label, float* value, float speed,
                       float minValue, float maxValue, const char* format,
                       ImGuiSliderFlags flags)
{
    const ImGuiID id            = ImGui::GetID(label);
    const float   hoverAmount   = updateButtonHoverAmount(id);
    const int  pushedColorCount = pushAnimatedFrameColors(hoverAmount, false);
    const bool changed          = ImGui::DragFloat(
        label, value, speed, minValue, maxValue, format, flags);
    const bool clicked = isLastItemFeedbackActivated();
    ImGui::PopStyleColor(pushedColorCount);
    finishLastItemFeedback(id, clicked);
    return changed;
}

/// @brief 绘制带统一反馈的 ImGui 二维 Int 拖拽输入。
/// @warning UI 热路径：每帧拖拽输入绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackDragInt2(const char* label, int values[2], float speed,
                      int minValue, int maxValue, const char* format,
                      ImGuiSliderFlags flags)
{
    const ImGuiID id            = ImGui::GetID(label);
    const float   hoverAmount   = updateButtonHoverAmount(id);
    const int  pushedColorCount = pushAnimatedFrameColors(hoverAmount, false);
    const bool changed          = ImGui::DragInt2(
        label, values, speed, minValue, maxValue, format, flags);
    const bool clicked = isLastItemFeedbackActivated();
    ImGui::PopStyleColor(pushedColorCount);
    finishLastItemFeedback(id, clicked);
    return changed;
}

/// @brief 绘制带统一反馈的 ImGui 标量拖拽输入。
/// @warning UI 热路径：每帧拖拽输入绘制路径调用，只做 ImGui 状态读写、
/// 样式栈操作和已预加载 SFX pool 的即时触发。
bool FeedbackDragScalar(const char* label, ImGuiDataType dataType, void* value,
                        float speed, const void* minValue, const void* maxValue,
                        const char* format, ImGuiSliderFlags flags)
{
    const ImGuiID id            = ImGui::GetID(label);
    const float   hoverAmount   = updateButtonHoverAmount(id);
    const int  pushedColorCount = pushAnimatedFrameColors(hoverAmount, false);
    const bool changed          = ImGui::DragScalar(
        label, dataType, value, speed, minValue, maxValue, format, flags);
    const bool clicked = isLastItemFeedbackActivated();
    ImGui::PopStyleColor(pushedColorCount);
    finishLastItemFeedback(id, clicked);
    return changed;
}

/// @brief 给上一条 ImGui Item 补充统一交互音效。
/// @param id 独立反馈状态 ID。
/// @param clicked 上一条 Item 本帧是否被激活。
/// @warning UI 热路径：用于自绘 hit zone，只做 ImGui 状态读写和已预加载
/// SFX pool 的即时触发。
void FeedbackLastItem(ImGuiID id, bool clicked)
{
    finishLastItemFeedback(id, clicked);
}

/// @brief 给当前 ImGui 窗口原生关闭按钮补充统一反馈。
/// @param wasOpenBeforeBegin 调用 ImGui::Begin 前窗口是否处于打开状态。
/// @param pOpen 传给 ImGui::Begin 的打开状态指针。
/// @warning UI 热路径：每帧窗口 Begin 后调用，只读取 ImGui 内部交互状态，
/// 并触发已预加载 SFX pool。
void FeedbackCurrentWindowCloseButton(bool wasOpenBeforeBegin, bool* pOpen)
{
    if ( !pOpen || !wasOpenBeforeBegin ) {
        return;
    }

    ImGuiWindow* window = ImGui::GetCurrentWindowRead();
    if ( !window || !window->HasCloseButton ) {
        return;
    }

    ImGuiContext& g              = *GImGui;
    const bool    closeRequested = !*pOpen;
    bool          clickHandled   = false;

    ImGuiDockNode* dockNode = window->DockNode;
    if ( dockNode ) {
        const ImGuiID nodeCloseId = getDockNodeCloseButtonId(dockNode);
        if ( nodeCloseId != 0 ) {
            const bool hovered =
                g.HoveredId == nodeCloseId || g.ActiveId == nodeCloseId;
            const bool clicked = closeRequested && dockNode->WantCloseAll;
            applyCloseButtonFeedback(nodeCloseId,
                                     getWindowStorage(dockNode->HostWindow),
                                     hovered,
                                     clicked);
            clickHandled = clickHandled || clicked;
        }

        const ImGuiID tabCloseId = getDockTabCloseButtonId(window);
        if ( tabCloseId != 0 ) {
            const bool hovered =
                g.HoveredId == tabCloseId || g.ActiveId == tabCloseId;
            const bool clicked =
                closeRequested && (dockNode->WantCloseTabId == window->TabId ||
                                   window->DockTabWantClose);
            applyCloseButtonFeedback(
                tabCloseId, getWindowStorage(window), hovered, clicked);
            clickHandled = clickHandled || clicked;
        }
    } else {
        const ImGuiID closeId = getFloatingWindowCloseButtonId(window);
        if ( closeId != 0 ) {
            const bool hovered =
                g.HoveredId == closeId || g.ActiveId == closeId;
            const bool clicked = closeRequested;
            applyCloseButtonFeedback(
                closeId, getWindowStorage(window), hovered, clicked);
            clickHandled = clickHandled || clicked;
        }
    }

    if ( closeRequested && !clickHandled ) {
        const ImGuiID fallbackId =
            window->ID ^ static_cast<ImGuiID>(BUTTON_CLICK_FRAME_KEY_SALT);
        finishButtonFeedback(fallbackId, true, getWindowStorage(window), false);
    }
}

/// @brief 给指定 DockSpace 下的原生节点按钮补充统一反馈。
/// @param dockspaceId 目标 DockSpace 节点 ID。
/// @warning UI 热路径：每帧 DockSpace 绘制后调用，只遍历当前 dock 树节点，
/// 并触发已预加载 SFX pool。
void FeedbackDockNodeControls(ImGuiID dockspaceId)
{
    if ( dockspaceId == 0 ) {
        return;
    }

    feedbackDockNodeControlsRecursive(ImGui::DockBuilderGetNode(dockspaceId));
}

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

/// @brief 绘制带统一音效反馈的 ImGui 颜色按钮。
/// @param descId 颜色按钮描述文本和 ImGui ID。
/// @param color 按钮显示颜色。
/// @param flags 颜色按钮标志。
/// @param size 按钮尺寸，语义与 ImGui::ColorButton 保持一致。
/// @return 按钮本帧被激活时返回 true。
/// @warning UI 热路径：每帧颜色按钮绘制路径调用，只做 ImGui 状态读写
/// 和已预加载 SFX pool 的即时触发。
bool FeedbackColorButton(const char* descId, const ImVec4& color,
                         ImGuiColorEditFlags flags, const ImVec2& size)
{
    const ImGuiID id      = ImGui::GetID(descId);
    const bool    clicked = ImGui::ColorButton(descId, color, flags, size);
    finishLastItemFeedback(id, isLastItemFeedbackActivated());
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

    const float           hoverAmount = updateButtonHoverAmount(id, storage);
    RoundedHighlightLayer highlightLayer;
    beginRoundedHighlightLayer(&highlightLayer);
    const int    pushedColorCount = pushTransparentHeaderColors();
    const bool   open             = ImGui::BeginMenu(label, enabled);
    const bool   clicked          = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool   hovered          = ImGui::IsItemHovered();
    const bool   active           = ImGui::IsItemActive() || open;
    const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopStyleColor(pushedColorCount);
    endRoundedHighlightLayer(
        &highlightLayer,
        itemRect,
        calcRoundedHighlightColor(hoverAmount, false, hovered, active));
    finishMenuFeedback(id, clicked, open, storage, hovered);
    if ( open ) {
        const float amount = updateMenuPopupAmount(id, storage, true);
        pushMenuPopupAnimation(amount);
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
    ImGuiStorage*         storage     = ImGui::GetStateStorage();
    const ImGuiID         id          = ImGui::GetID(label);
    const float           hoverAmount = updateButtonHoverAmount(id, storage);
    RoundedHighlightLayer highlightLayer;
    beginRoundedHighlightLayer(&highlightLayer);
    const int    pushedColorCount = pushTransparentHeaderColors();
    const bool   clicked = ImGui::MenuItem(label, shortcut, selected, enabled);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   active  = ImGui::IsItemActive();
    const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopStyleColor(pushedColorCount);
    endRoundedHighlightLayer(
        &highlightLayer,
        itemRect,
        calcRoundedHighlightColor(hoverAmount, selected, hovered, active));
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
    ImGuiStorage*         storage     = ImGui::GetStateStorage();
    const ImGuiID         id          = ImGui::GetID(label);
    const float           hoverAmount = updateButtonHoverAmount(id, storage);
    RoundedHighlightLayer highlightLayer;
    beginRoundedHighlightLayer(&highlightLayer);
    const int    pushedColorCount = pushTransparentHeaderColors();
    const bool   clicked = ImGui::MenuItem(label, shortcut, pSelected, enabled);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   active  = ImGui::IsItemActive();
    const bool   selected = pSelected && *pSelected;
    const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopStyleColor(pushedColorCount);
    endRoundedHighlightLayer(
        &highlightLayer,
        itemRect,
        calcRoundedHighlightColor(hoverAmount, selected, hovered, active));
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
    ImGuiStorage*         storage     = ImGui::GetStateStorage();
    const ImGuiID         id          = ImGui::GetID(label);
    const float           hoverAmount = updateButtonHoverAmount(id, storage);
    RoundedHighlightLayer highlightLayer;
    beginRoundedHighlightLayer(&highlightLayer);
    const int  pushedColorCount = pushTransparentHeaderColors();
    const bool clicked =
        ImGui::MenuItemEx(label, icon, shortcut, selected, enabled);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   active  = ImGui::IsItemActive();
    const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::PopStyleColor(pushedColorCount);
    endRoundedHighlightLayer(
        &highlightLayer,
        itemRect,
        calcRoundedHighlightColor(hoverAmount, selected, hovered, active));
    finishButtonFeedback(id, clicked, storage, hovered);
    return clicked;
}

}  // namespace MMM::UI
