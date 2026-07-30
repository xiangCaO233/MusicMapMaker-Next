#include "ui/imgui/manager/ProjectAudioToolView.h"

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/project/ProjectEvents.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace MMM::UI
{
namespace
{

/// @brief Effect 方块的默认逻辑边长。
constexpr float DEFAULT_EFFECT_SIZE = 92.0F;

/// @brief Main 方块的默认和最小逻辑宽度。
constexpr float DEFAULT_MAIN_WIDTH = 202.0F;

/// @brief Main 方块的默认逻辑高度。
constexpr float DEFAULT_MAIN_HEIGHT = 92.0F;

/// @brief Effect 方块允许用户缩小到的最小逻辑宽度。
constexpr float MINIMUM_EFFECT_WIDTH = 48.0F;

/// @brief 所有方块允许用户缩小到的最小逻辑高度。
constexpr float MINIMUM_ITEM_HEIGHT = 48.0F;

/// @brief 方块边缘缩放热区的逻辑厚度。
constexpr float RESIZE_HIT_THICKNESS = 8.0F;

/// @brief 选中方块缩放控制点的逻辑边长。
constexpr float RESIZE_HANDLE_SIZE = 7.0F;

/// @brief 默认方块布局间距。
constexpr float ITEM_GAP = 18.0F;

/// @brief 默认方块布局画布留白。
constexpr float CANVAS_PADDING = 24.0F;

/// @brief 任一下层方块必须保留的最小可见比例。
constexpr float MINIMUM_VISIBLE_RATIO = 0.35F;

/// @brief 方块开始吸附的逻辑像素距离。
constexpr float SNAP_THRESHOLD = 8.0F;

/// @brief 已吸附方块脱离目标所需的逻辑像素距离。
constexpr float SNAP_RELEASE_THRESHOLD = 16.0F;

/// @brief 最远方块之后保留的可滚动画布空间。
constexpr float CONTENT_END_PADDING = 80.0F;

/// @brief 获取指定音频类型允许的最小逻辑宽度。
float minimumItemWidth(AudioTrackType type)
{
    return type == AudioTrackType::Main ? DEFAULT_MAIN_WIDTH
                                        : MINIMUM_EFFECT_WIDTH;
}

/// @brief 按当前字体测量结果计算可完整显示文件名的默认逻辑宽度。
float defaultItemWidth(std::string_view label, AudioTrackType type,
                       float dpiScale)
{
    const float safeScale = std::max(0.01F, dpiScale);
    const float textWidth =
        ImGui::CalcTextSize(label.data(), label.data() + label.size()).x /
        safeScale;
    const float horizontalPadding =
        ImGui::GetStyle().FramePadding.x / safeScale;
    const float defaultMinimum =
        type == AudioTrackType::Main ? DEFAULT_MAIN_WIDTH : DEFAULT_EFFECT_SIZE;
    return ProjectAudioToolLayout::calculateDefaultWidth(
        textWidth, horizontalPadding, defaultMinimum);
}

/// @brief 将项目音频资源路径转换为方块显示标签。
std::string audioResourceLabel(const AudioResource& resource)
{
    const auto        filename = Config::utf8ToPath(resource.m_path).filename();
    const std::string label    = Config::pathToUtf8(filename);
    return label.empty() ? resource.m_id : label;
}

/// @brief 判断逻辑矩形是否与当前可见区域相交。
bool isVisible(const ProjectAudioToolLayout::Rect& rect,
               const ProjectAudioToolLayout::Rect& visible)
{
    return ProjectAudioToolLayout::intersection(rect, visible).has_value();
}

/// @brief 转换逻辑画布矩形为屏幕像素矩形。
ProjectAudioToolLayout::Rect toScreenRect(
    const ProjectAudioToolLayout::Rect& rect, ImVec2 origin, float dpiScale)
{
    return {
        origin.x + rect.x * dpiScale,
        origin.y + rect.y * dpiScale,
        rect.width * dpiScale,
        rect.height * dpiScale,
    };
}

/// @brief 判断逻辑点是否位于矩形内。
bool contains(const ProjectAudioToolLayout::Rect& rect, ImVec2 point)
{
    return point.x >= rect.x && point.x <= rect.right() && point.y >= rect.y &&
           point.y <= rect.bottom();
}

/// @brief 判断矩形指定轴上的边缘或中心是否与目标参考线重合。
bool alignsWithTargetLine(const ProjectAudioToolLayout::Rect& rect,
                          float targetLine, bool horizontal)
{
    constexpr float EPSILON = 0.25F;
    if ( horizontal ) {
        return std::abs(rect.x - targetLine) <= EPSILON ||
               std::abs(rect.x + rect.width * 0.5F - targetLine) <= EPSILON ||
               std::abs(rect.right() - targetLine) <= EPSILON;
    }
    return std::abs(rect.y - targetLine) <= EPSILON ||
           std::abs(rect.y + rect.height * 0.5F - targetLine) <= EPSILON ||
           std::abs(rect.bottom() - targetLine) <= EPSILON;
}

/// @brief 绘制一条与主画布布局调整一致的半透明虚线吸附参考线。
/// @warning UI 拖动热路径：仅吸附生效时调用，按可见画布单轴生成短线段。
void drawSnapGuide(ImDrawList& drawList, const ImVec2& start, const ImVec2& end,
                   ImU32 color, float thickness, float dashLength,
                   float gapLength)
{
    const float deltaX = end.x - start.x;
    const float deltaY = end.y - start.y;
    const float length = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    if ( length <= 0.0F ) return;

    dashLength       = std::max(1.0F, dashLength);
    gapLength        = std::max(1.0F, gapLength);
    const float dx   = deltaX / length;
    const float dy   = deltaY / length;
    const float step = dashLength + gapLength;
    for ( float distance = 0.0F; distance < length; distance += step ) {
        const float segmentEnd = std::min(distance + dashLength, length);
        drawList.AddLine(
            { start.x + dx * distance, start.y + dy * distance },
            { start.x + dx * segmentEnd, start.y + dy * segmentEnd },
            color,
            thickness);
    }
}

}  // namespace

ProjectAudioToolView::ProjectAudioToolView(const std::string& name)
    : IUIView(name)
{
    auto& eventBus      = Event::EventBus::instance();
    m_projectSavedSubId = eventBus.subscribe<Event::ProjectSavedEvent>(
        [this](const Event::ProjectSavedEvent&) {
            m_itemsDirty.store(true, std::memory_order_release);
        });
    m_audioMutationSubId =
        eventBus.subscribe<Event::AudioResourceMutationResultEvent>(
            [this](const Event::AudioResourceMutationResultEvent& event) {
                if ( event.m_success ) {
                    m_itemsDirty.store(true, std::memory_order_release);
                }
            });
}

ProjectAudioToolView::~ProjectAudioToolView()
{
    auto& eventBus = Event::EventBus::instance();
    if ( m_projectSavedSubId != 0 ) {
        eventBus.unsubscribe<Event::ProjectSavedEvent>(m_projectSavedSubId);
    }
    if ( m_audioMutationSubId != 0 ) {
        eventBus.unsubscribe<Event::AudioResourceMutationResultEvent>(
            m_audioMutationSubId);
    }
}

void ProjectAudioToolView::requestFocus()
{
    m_requestFocus = true;
}

void ProjectAudioToolView::rebuildItems(float visibleWidth, float dpiScale)
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        m_items.clear();
        m_selectedAudioResourceId.clear();
        m_selectedAudioLabel.clear();
        m_cachedProjectRoot.clear();
        m_cachedDpiScale = 0.0F;
        return;
    }

    m_draggingItem.reset();
    m_resizingItem.reset();
    m_resizeHandle = ResizeHandle::None;
    m_snapLocks    = {};

    m_cachedProjectRoot       = Config::pathToUtf8(project->m_projectRoot);
    m_cachedDpiScale          = dpiScale;
    auto& workspace           = project->m_settings.m_workspace;
    m_selectedAudioResourceId = workspace.m_projectAudioToolSelectedResourceId;

    std::unordered_map<std::string, ProjectAudioToolItemPlacement>
        savedPlacements;
    savedPlacements.reserve(workspace.m_projectAudioToolPlacements.size());
    for ( const auto& placement : workspace.m_projectAudioToolPlacements ) {
        if ( !placement.m_audioResourceId.empty() ) {
            savedPlacements.insert_or_assign(placement.m_audioResourceId,
                                             placement);
        }
    }

    m_items.clear();
    m_items.reserve(project->m_audioResources.size());
    float        defaultCursorX = CANVAS_PADDING;
    float        defaultCursorY = CANVAS_PADDING;
    float        defaultRowHeight{ 0.0F };
    std::int32_t nextZOrder = 0;
    for ( const auto& resource : project->m_audioResources ) {
        Item item;
        item.audioResourceId = resource.m_id;
        item.label           = audioResourceLabel(resource);
        item.type            = resource.m_type;
        item.rect.width  = defaultItemWidth(item.label, item.type, dpiScale);
        item.rect.height = resource.m_type == AudioTrackType::Main
                               ? DEFAULT_MAIN_HEIGHT
                               : DEFAULT_EFFECT_SIZE;

        const auto saved = savedPlacements.find(resource.m_id);
        if ( saved != savedPlacements.end() ) {
            const bool hasSavedWidth  = std::isfinite(saved->second.m_width) &&
                                        saved->second.m_width > 0.0F;
            const bool hasSavedHeight = std::isfinite(saved->second.m_height) &&
                                        saved->second.m_height > 0.0F;
            item.widthCustomized      = hasSavedWidth;
            item.heightCustomized     = hasSavedHeight;
            if ( hasSavedWidth ) {
                item.rect.width = std::max(minimumItemWidth(item.type),
                                           saved->second.m_width);
            }
            if ( hasSavedHeight ) {
                item.rect.height =
                    std::max(MINIMUM_ITEM_HEIGHT, saved->second.m_height);
            }
        }
        if ( saved != savedPlacements.end() &&
             std::isfinite(saved->second.m_x) &&
             std::isfinite(saved->second.m_y) ) {
            item.rect.x = std::max(0.0F, saved->second.m_x);
            item.rect.y = std::max(0.0F, saved->second.m_y);
            item.zOrder = saved->second.m_zOrder;
        } else if ( resource.m_type == AudioTrackType::Main ) {
            if ( defaultCursorX > CANVAS_PADDING ) {
                defaultCursorY += defaultRowHeight + ITEM_GAP;
                defaultCursorX   = CANVAS_PADDING;
                defaultRowHeight = 0.0F;
            }
            item.rect.x = defaultCursorX;
            item.rect.y = defaultCursorY;
            item.zOrder = nextZOrder;
            defaultCursorY += item.rect.height + ITEM_GAP;
        } else {
            const float defaultRight =
                std::max(CANVAS_PADDING + item.rect.width,
                         visibleWidth - CANVAS_PADDING);
            if ( defaultCursorX > CANVAS_PADDING &&
                 defaultCursorX + item.rect.width > defaultRight ) {
                defaultCursorY += defaultRowHeight + ITEM_GAP;
                defaultCursorX   = CANVAS_PADDING;
                defaultRowHeight = 0.0F;
            }
            item.rect.x = defaultCursorX;
            item.rect.y = defaultCursorY;
            item.zOrder = nextZOrder;
            defaultCursorX += item.rect.width + ITEM_GAP;
            defaultRowHeight = std::max(defaultRowHeight, item.rect.height);
        }
        nextZOrder = std::max(nextZOrder, item.zOrder + 1);
        m_items.push_back(std::move(item));
    }

    std::ranges::sort(m_items, [](const Item& lhs, const Item& rhs) {
        if ( lhs.zOrder != rhs.zOrder ) return lhs.zOrder < rhs.zOrder;
        return lhs.audioResourceId < rhs.audioResourceId;
    });
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        m_items[index].zOrder = static_cast<std::int32_t>(index);
    }

    const bool selectedStillExists =
        std::ranges::any_of(m_items, [this](const Item& item) {
            return item.audioResourceId == m_selectedAudioResourceId;
        });
    if ( !selectedStillExists ) {
        m_selectedAudioResourceId.clear();
        m_selectedAudioLabel.clear();
        workspace.m_projectAudioToolSelectedResourceId.clear();
    } else {
        const auto selected =
            std::ranges::find_if(m_items, [this](const Item& item) {
                return item.audioResourceId == m_selectedAudioResourceId;
            });
        if ( selected != m_items.end() ) {
            m_selectedAudioLabel     = selected->label;
            m_selectedAudioTrackType = selected->type;
        }
    }
    rebuildLabelRects();
}

void ProjectAudioToolView::rebuildLabelRects()
{
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        std::vector<ProjectAudioToolLayout::Rect> occluders;
        for ( std::size_t higher = index + 1; higher < m_items.size();
              ++higher ) {
            if ( ProjectAudioToolLayout::intersection(m_items[index].rect,
                                                      m_items[higher].rect) ) {
                occluders.push_back(m_items[higher].rect);
            }
        }
        m_items[index].labelRect = ProjectAudioToolLayout::largestVisibleCell(
            m_items[index].rect, occluders);
    }
}

void ProjectAudioToolView::persistWorkspace()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project ) return;

    auto& workspace = project->m_settings.m_workspace;
    workspace.m_projectAudioToolSelectedResourceId = m_selectedAudioResourceId;
    workspace.m_projectAudioToolPlacements.clear();
    workspace.m_projectAudioToolPlacements.reserve(m_items.size());
    for ( const auto& item : m_items ) {
        workspace.m_projectAudioToolPlacements.push_back(
            ProjectAudioToolItemPlacement{
                .m_audioResourceId = item.audioResourceId,
                .m_x               = item.rect.x,
                .m_y               = item.rect.y,
                .m_width  = item.widthCustomized ? item.rect.width : 0.0F,
                .m_height = item.heightCustomized ? item.rect.height : 0.0F,
                .m_zOrder = item.zOrder,
            });
    }
    engine.saveProject();
}

std::optional<std::size_t> ProjectAudioToolView::activateItem(
    std::size_t itemIndex)
{
    if ( itemIndex >= m_items.size() ) return std::nullopt;

    Item selected = std::move(m_items[itemIndex]);
    m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(itemIndex));
    m_items.push_back(std::move(selected));
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        m_items[index].zOrder = static_cast<std::int32_t>(index);
    }

    if ( m_items.empty() ) return std::nullopt;
    const std::size_t activeIndex = m_items.size() - 1;
    auto&             item        = m_items[activeIndex];
    item.labelRect                = item.rect;
    m_selectedAudioResourceId     = item.audioResourceId;
    m_selectedAudioLabel          = item.label;
    m_selectedAudioTrackType      = item.type;
    Logic::EditorEngine::instance().pushCommand(
        Logic::LogicCommand(Logic::CmdSetBrushAudioResource{
            item.audioResourceId,
            item.type,
        }));
    return activeIndex;
}

void ProjectAudioToolView::beginItemDrag(std::size_t itemIndex,
                                         ImVec2      mousePosition)
{
    const auto activeIndex = activateItem(itemIndex);
    if ( !activeIndex ) return;

    m_draggingItem   = *activeIndex;
    const auto& item = m_items[*activeIndex];
    m_dragOffset     = {
        mousePosition.x - item.rect.x,
        mousePosition.y - item.rect.y,
    };
    m_snapLocks = {};
    rebuildInteractionConstraints();
}

void ProjectAudioToolView::beginItemResize(std::size_t  itemIndex,
                                           ResizeHandle handle,
                                           ImVec2       mousePosition)
{
    const auto activeIndex = activateItem(itemIndex);
    if ( !activeIndex || handle == ResizeHandle::None ) return;

    m_resizingItem        = *activeIndex;
    m_resizeHandle        = handle;
    m_resizeStartRect     = m_items[*activeIndex].rect;
    m_resizePointerOffset = {};
    switch ( handle ) {
    case ResizeHandle::Left:
    case ResizeHandle::TopLeft:
    case ResizeHandle::BottomLeft:
        m_resizePointerOffset.x = mousePosition.x - m_resizeStartRect.x;
        break;
    case ResizeHandle::Right:
    case ResizeHandle::TopRight:
    case ResizeHandle::BottomRight:
        m_resizePointerOffset.x = mousePosition.x - m_resizeStartRect.right();
        break;
    default: break;
    }
    switch ( handle ) {
    case ResizeHandle::Top:
    case ResizeHandle::TopLeft:
    case ResizeHandle::TopRight:
        m_resizePointerOffset.y = mousePosition.y - m_resizeStartRect.y;
        break;
    case ResizeHandle::Bottom:
    case ResizeHandle::BottomLeft:
    case ResizeHandle::BottomRight:
        m_resizePointerOffset.y = mousePosition.y - m_resizeStartRect.bottom();
        break;
    default: break;
    }
    m_snapLocks = {};
    rebuildInteractionConstraints();
}

ProjectAudioToolView::ResizeHandle ProjectAudioToolView::hitTestResizeHandle(
    const Item& item, ImVec2 mousePosition) const
{
    const bool nearLeft =
        std::abs(mousePosition.x - item.rect.x) <= RESIZE_HIT_THICKNESS;
    const bool nearRight =
        std::abs(mousePosition.x - item.rect.right()) <= RESIZE_HIT_THICKNESS;
    const bool nearTop =
        std::abs(mousePosition.y - item.rect.y) <= RESIZE_HIT_THICKNESS;
    const bool nearBottom =
        std::abs(mousePosition.y - item.rect.bottom()) <= RESIZE_HIT_THICKNESS;
    if ( nearLeft && nearTop ) return ResizeHandle::TopLeft;
    if ( nearRight && nearTop ) return ResizeHandle::TopRight;
    if ( nearLeft && nearBottom ) return ResizeHandle::BottomLeft;
    if ( nearRight && nearBottom ) return ResizeHandle::BottomRight;
    if ( nearLeft ) return ResizeHandle::Left;
    if ( nearRight ) return ResizeHandle::Right;
    if ( nearTop ) return ResizeHandle::Top;
    if ( nearBottom ) return ResizeHandle::Bottom;
    return ResizeHandle::None;
}

void ProjectAudioToolView::rebuildInteractionConstraints()
{
    m_dragSnapTargets.clear();
    m_dragVisibilityConstraints.clear();
    const auto activeItem = m_draggingItem ? m_draggingItem : m_resizingItem;
    if ( !activeItem || *activeItem >= m_items.size() ) return;

    const std::size_t movingIndex = *activeItem;
    m_dragSnapTargets.reserve(m_items.size() - 1);
    m_dragVisibilityConstraints.reserve(m_items.size() - 1);
    for ( std::size_t baseIndex = 0; baseIndex < m_items.size(); ++baseIndex ) {
        if ( baseIndex == movingIndex ) continue;
        m_dragSnapTargets.push_back(m_items[baseIndex].rect);

        std::vector<ProjectAudioToolLayout::Rect> fixedOccluders;
        for ( std::size_t higherIndex = baseIndex + 1;
              higherIndex < movingIndex;
              ++higherIndex ) {
            if ( ProjectAudioToolLayout::intersection(
                     m_items[baseIndex].rect, m_items[higherIndex].rect) ) {
                fixedOccluders.push_back(m_items[higherIndex].rect);
            }
        }
        m_dragVisibilityConstraints.push_back(
            ProjectAudioToolLayout::prepareVisibilityConstraint(
                m_items[baseIndex].rect, fixedOccluders));
    }
}

void ProjectAudioToolView::drawItem(const Item& item, ImVec2 canvasOrigin,
                                    float dpiScale, bool hovered, bool pressed,
                                    ImDrawList& drawList) const
{
    const auto   screenRect = toScreenRect(item.rect, canvasOrigin, dpiScale);
    const ImVec2 minimum{ screenRect.x, screenRect.y };
    const ImVec2 maximum{ screenRect.right(), screenRect.bottom() };
    const bool   selected = item.audioResourceId == m_selectedAudioResourceId;

    const auto&  style = ImGui::GetStyle();
    const ImVec4 mainColor{ 0.89F, 0.56F, 0.23F, 0.95F };
    const ImVec4 effectColor{ 0.22F, 0.67F, 0.88F, 0.95F };
    ImVec4 fill = item.type == AudioTrackType::Main ? mainColor : effectColor;
    if ( hovered ) {
        fill.x = std::min(1.0F, fill.x * 1.13F);
        fill.y = std::min(1.0F, fill.y * 1.13F);
        fill.z = std::min(1.0F, fill.z * 1.13F);
    }
    if ( pressed ) {
        fill.x *= 0.82F;
        fill.y *= 0.82F;
        fill.z *= 0.82F;
    }
    const float rounding = style.FrameRounding;
    drawList.AddRectFilled(
        minimum, maximum, ImGui::ColorConvertFloat4ToU32(fill), rounding);
    drawList.AddRect(
        minimum,
        maximum,
        ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_Border),
        rounding,
        0,
        selected ? std::max(3.0F * dpiScale, style.FrameBorderSize)
                 : std::max(1.0F, style.FrameBorderSize));

    const auto labelRect = toScreenRect(item.labelRect, canvasOrigin, dpiScale);
    const float  horizontalPadding = std::max(1.0F, style.FramePadding.x);
    const float  verticalPadding   = std::max(1.0F, style.FramePadding.y);
    const ImVec2 labelStart{
        labelRect.x + horizontalPadding,
        labelRect.y +
            std::max(0.0F,
                     (labelRect.height - ImGui::GetTextLineHeight()) * 0.5F),
    };
    const float labelWidth =
        std::max(1.0F, labelRect.width - horizontalPadding * 2.0F);
    drawList.PushClipRect({ labelRect.x, labelRect.y },
                          { labelRect.right(), labelRect.bottom() },
                          true);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06F, 0.08F, 0.11F, 1.0F));
    Utils::drawScrollingText(
        item.label, labelStart, labelWidth, labelRect.height);
    ImGui::PopStyleColor();
    drawList.PopClipRect();

    const char* typeLabel = item.type == AudioTrackType::Main ? "MAIN" : "FX";
    drawList.AddText(
        { minimum.x + horizontalPadding, minimum.y + verticalPadding },
        ImGui::GetColorU32(ImVec4(0.06F, 0.08F, 0.11F, 0.78F)),
        typeLabel);

    if ( selected ) {
        const float halfHandle = RESIZE_HANDLE_SIZE * dpiScale * 0.5F;
        const std::array<ImVec2, 8> handleCenters{
            ImVec2{ minimum.x, minimum.y },
            ImVec2{ (minimum.x + maximum.x) * 0.5F, minimum.y },
            ImVec2{ maximum.x, minimum.y },
            ImVec2{ minimum.x, (minimum.y + maximum.y) * 0.5F },
            ImVec2{ maximum.x, (minimum.y + maximum.y) * 0.5F },
            ImVec2{ minimum.x, maximum.y },
            ImVec2{ (minimum.x + maximum.x) * 0.5F, maximum.y },
            ImVec2{ maximum.x, maximum.y },
        };
        for ( const auto center : handleCenters ) {
            const ImVec2 handleMinimum{ center.x - halfHandle,
                                        center.y - halfHandle };
            const ImVec2 handleMaximum{ center.x + halfHandle,
                                        center.y + halfHandle };
            drawList.AddRectFilled(handleMinimum,
                                   handleMaximum,
                                   ImGui::GetColorU32(ImGuiCol_Text),
                                   std::min(rounding, halfHandle));
            drawList.AddRect(handleMinimum,
                             handleMaximum,
                             ImGui::GetColorU32(ImGuiCol_WindowBg),
                             std::min(rounding, halfHandle));
        }
    }
}

ImVec2 ProjectAudioToolView::calculateContentSize(float visibleWidth,
                                                  float visibleHeight) const
{
    float right  = visibleWidth;
    float bottom = visibleHeight;
    for ( const auto& item : m_items ) {
        right  = std::max(right, item.rect.right() + CONTENT_END_PADDING);
        bottom = std::max(bottom, item.rect.bottom() + CONTENT_END_PADDING);
    }
    return { right, bottom };
}

void ProjectAudioToolView::update(UIManager* sourceManager)
{
    if ( m_requestFocus ) {
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    ImGui::SetNextWindowSize({ 720.0F * dpiScale, 520.0F * dpiScale },
                             ImGuiCond_FirstUseEver);
    std::string title =
        std::string(TR("title.project_audio_tool")) + "###ProjectAudioTool";
    LayoutContext layoutContext(
        m_layoutCtx, title, true, ImGuiWindowFlags_None, &m_isOpen);

    if ( !sourceManager || sourceManager->isProjectTransitionInProgress() ) {
        Utils::renderProjectTransitionPlaceholder();
        return;
    }
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        ImGui::TextUnformatted(TR("ui.project_audio_tool.no_project").data());
        return;
    }

    ImGui::TextUnformatted(TR("ui.project_audio_tool.hint").data());
    ImGui::Separator();
    const float statusHeight =
        ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImVec2 childSize = ImGui::GetContentRegionAvail();
    childSize.y      = std::max(1.0F, childSize.y - statusHeight);
    ImGui::BeginChild(
        "ProjectAudioToolCanvas",
        childSize,
        true,
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    const ImVec2 visibleSizePixels = ImGui::GetContentRegionAvail();
    const float  visibleWidth  = std::max(1.0F, visibleSizePixels.x / dpiScale);
    const float  visibleHeight = std::max(1.0F, visibleSizePixels.y / dpiScale);
    const std::string projectRoot = Config::pathToUtf8(project->m_projectRoot);
    if ( m_itemsDirty.exchange(false, std::memory_order_acq_rel) ||
         projectRoot != m_cachedProjectRoot ||
         std::abs(dpiScale - m_cachedDpiScale) > 1e-4F ) {
        rebuildItems(visibleWidth, dpiScale);
    }

    const ImVec2 canvasCursor = ImGui::GetCursorScreenPos();
    const ImVec2 scroll{ ImGui::GetScrollX(), ImGui::GetScrollY() };
    const ImVec2 canvasOrigin = canvasCursor;
    const ImVec2 contentLogical =
        calculateContentSize(visibleWidth, visibleHeight);
    ImGui::InvisibleButton(
        "ProjectAudioToolCanvasSurface",
        { contentLogical.x * dpiScale, contentLogical.y * dpiScale },
        ImGuiButtonFlags_MouseButtonLeft);

    const ProjectAudioToolLayout::Rect visibleCanvas{
        scroll.x / dpiScale,
        scroll.y / dpiScale,
        visibleWidth,
        visibleHeight,
    };
    const bool canvasHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImVec2 mouseLogical{
        (ImGui::GetIO().MousePos.x - canvasOrigin.x) / dpiScale,
        (ImGui::GetIO().MousePos.y - canvasOrigin.y) / dpiScale,
    };
    std::optional<std::size_t> hoveredItem;
    if ( canvasHovered ) {
        for ( std::size_t reverse = m_items.size(); reverse > 0; --reverse ) {
            if ( contains(m_items[reverse - 1].rect, mouseLogical) ) {
                hoveredItem = reverse - 1;
                break;
            }
        }
    }
    ResizeHandle hoveredResizeHandle = ResizeHandle::None;
    if ( m_resizingItem ) {
        hoveredResizeHandle = m_resizeHandle;
    } else if ( hoveredItem ) {
        hoveredResizeHandle =
            hitTestResizeHandle(m_items[*hoveredItem], mouseLogical);
    }
    switch ( hoveredResizeHandle ) {
    case ResizeHandle::Left:
    case ResizeHandle::Right:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        break;
    case ResizeHandle::Top:
    case ResizeHandle::Bottom:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        break;
    case ResizeHandle::TopLeft:
    case ResizeHandle::BottomRight:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        break;
    case ResizeHandle::TopRight:
    case ResizeHandle::BottomLeft:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
        break;
    case ResizeHandle::None: break;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        const auto& item = m_items[index];
        if ( isVisible(item.rect, visibleCanvas) ) {
            drawItem(item,
                     canvasOrigin,
                     dpiScale,
                     hoveredItem == index,
                     (m_draggingItem == index || m_resizingItem == index) &&
                         ImGui::IsMouseDown(ImGuiMouseButton_Left),
                     *drawList);
        }
    }

    if ( canvasHovered && !m_draggingItem && !m_resizingItem &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        if ( hoveredItem ) {
            if ( hoveredResizeHandle != ResizeHandle::None ) {
                beginItemResize(
                    *hoveredItem, hoveredResizeHandle, mouseLogical);
            } else {
                beginItemDrag(*hoveredItem, mouseLogical);
            }
        }
    }

    if ( m_draggingItem && *m_draggingItem < m_items.size() ) {
        auto& item = m_items[*m_draggingItem];
        if ( ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            ProjectAudioToolLayout::Rect rawRect{
                mouseLogical.x - m_dragOffset.x,
                mouseLogical.y - m_dragOffset.y,
                item.rect.width,
                item.rect.height,
            };
            rawRect   = ProjectAudioToolLayout::snapRect(rawRect,
                                                         visibleCanvas,
                                                         m_dragSnapTargets,
                                                         SNAP_THRESHOLD,
                                                         SNAP_RELEASE_THRESHOLD,
                                                         m_snapLocks);
            item.rect = ProjectAudioToolLayout::constrainVisibility(
                rawRect,
                ProjectAudioToolLayout::Rect{
                    0.0F,
                    0.0F,
                    contentLogical.x,
                    contentLogical.y,
                },
                m_dragVisibilityConstraints,
                MINIMUM_VISIBLE_RATIO);
            item.labelRect = item.rect;
        } else {
            rebuildLabelRects();
            persistWorkspace();
            m_draggingItem.reset();
            m_dragSnapTargets.clear();
            m_dragVisibilityConstraints.clear();
            m_snapLocks = {};
        }
    }

    if ( m_resizingItem && *m_resizingItem < m_items.size() ) {
        auto& item = m_items[*m_resizingItem];
        if ( ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            using ProjectAudioToolLayout::ResizeEdge;
            ResizeEdge horizontalEdge = ResizeEdge::None;
            ResizeEdge verticalEdge   = ResizeEdge::None;
            switch ( m_resizeHandle ) {
            case ResizeHandle::Left:
            case ResizeHandle::TopLeft:
            case ResizeHandle::BottomLeft:
                horizontalEdge = ResizeEdge::Minimum;
                break;
            case ResizeHandle::Right:
            case ResizeHandle::TopRight:
            case ResizeHandle::BottomRight:
                horizontalEdge = ResizeEdge::Maximum;
                break;
            default: break;
            }
            switch ( m_resizeHandle ) {
            case ResizeHandle::Top:
            case ResizeHandle::TopLeft:
            case ResizeHandle::TopRight:
                verticalEdge = ResizeEdge::Minimum;
                break;
            case ResizeHandle::Bottom:
            case ResizeHandle::BottomLeft:
            case ResizeHandle::BottomRight:
                verticalEdge = ResizeEdge::Maximum;
                break;
            default: break;
            }

            const float minimumWidth             = minimumItemWidth(item.type);
            ProjectAudioToolLayout::Rect rawRect = m_resizeStartRect;
            if ( horizontalEdge == ResizeEdge::Minimum ) {
                const float right = m_resizeStartRect.right();
                rawRect.x = std::clamp(mouseLogical.x - m_resizePointerOffset.x,
                                       0.0F,
                                       right - minimumWidth);
                rawRect.width = right - rawRect.x;
            } else if ( horizontalEdge == ResizeEdge::Maximum ) {
                rawRect.width =
                    std::max(minimumWidth,
                             mouseLogical.x - m_resizePointerOffset.x -
                                 m_resizeStartRect.x);
            }
            if ( verticalEdge == ResizeEdge::Minimum ) {
                const float bottom = m_resizeStartRect.bottom();
                rawRect.y = std::clamp(mouseLogical.y - m_resizePointerOffset.y,
                                       0.0F,
                                       bottom - MINIMUM_ITEM_HEIGHT);
                rawRect.height = bottom - rawRect.y;
            } else if ( verticalEdge == ResizeEdge::Maximum ) {
                rawRect.height =
                    std::max(MINIMUM_ITEM_HEIGHT,
                             mouseLogical.y - m_resizePointerOffset.y -
                                 m_resizeStartRect.y);
            }
            rawRect =
                ProjectAudioToolLayout::snapResizeRect(rawRect,
                                                       horizontalEdge,
                                                       verticalEdge,
                                                       visibleCanvas,
                                                       m_dragSnapTargets,
                                                       minimumWidth,
                                                       MINIMUM_ITEM_HEIGHT,
                                                       SNAP_THRESHOLD,
                                                       SNAP_RELEASE_THRESHOLD,
                                                       m_snapLocks);
            item.rect = ProjectAudioToolLayout::constrainResizeVisibility(
                item.rect,
                rawRect,
                m_dragVisibilityConstraints,
                MINIMUM_VISIBLE_RATIO);
            item.labelRect = item.rect;
            if ( horizontalEdge != ResizeEdge::None &&
                 std::abs(item.rect.width - m_resizeStartRect.width) > 1e-4F ) {
                item.widthCustomized = true;
            }
            if ( verticalEdge != ResizeEdge::None &&
                 std::abs(item.rect.height - m_resizeStartRect.height) >
                     1e-4F ) {
                item.heightCustomized = true;
            }
        } else {
            rebuildLabelRects();
            persistWorkspace();
            m_resizingItem.reset();
            m_resizeHandle = ResizeHandle::None;
            m_dragSnapTargets.clear();
            m_dragVisibilityConstraints.clear();
            m_snapLocks = {};
        }
    }

    const auto activeItem = m_draggingItem ? m_draggingItem : m_resizingItem;
    if ( activeItem && *activeItem < m_items.size() ) {
        const auto&  item = m_items[*activeItem];
        const ImVec2 visibleMinimum{
            canvasOrigin.x + visibleCanvas.x * dpiScale,
            canvasOrigin.y + visibleCanvas.y * dpiScale,
        };
        const ImVec2 visibleMaximum{
            canvasOrigin.x + visibleCanvas.right() * dpiScale,
            canvasOrigin.y + visibleCanvas.bottom() * dpiScale,
        };
        constexpr ImU32 SNAP_GUIDE_COLOR   = IM_COL32(255, 218, 96, 150);
        const float     snapGuideThickness = std::max(1.0F, 1.25F * dpiScale);
        const float     snapGuideDash      = std::max(4.0F, 6.0F * dpiScale);
        const float     snapGuideGap       = std::max(3.0F, 4.0F * dpiScale);
        if ( m_snapLocks.x.targetLine &&
             alignsWithTargetLine(
                 item.rect, *m_snapLocks.x.targetLine, true) ) {
            const float guideX =
                canvasOrigin.x + *m_snapLocks.x.targetLine * dpiScale;
            drawSnapGuide(*drawList,
                          { guideX, visibleMinimum.y },
                          { guideX, visibleMaximum.y },
                          SNAP_GUIDE_COLOR,
                          snapGuideThickness,
                          snapGuideDash,
                          snapGuideGap);
        }
        if ( m_snapLocks.y.targetLine &&
             alignsWithTargetLine(
                 item.rect, *m_snapLocks.y.targetLine, false) ) {
            const float guideY =
                canvasOrigin.y + *m_snapLocks.y.targetLine * dpiScale;
            drawSnapGuide(*drawList,
                          { visibleMinimum.x, guideY },
                          { visibleMaximum.x, guideY },
                          SNAP_GUIDE_COLOR,
                          snapGuideThickness,
                          snapGuideDash,
                          snapGuideGap);
        }
    }

    ImGui::EndChild();
    ImGui::Separator();
    if ( m_selectedAudioResourceId.empty() ) {
        ImGui::TextUnformatted(TR("ui.project_audio_tool.status_none").data());
    } else {
        ImGui::Text(
            "%s: %s  %s",
            TR("ui.project_audio_tool.status_selected").data(),
            m_selectedAudioTrackType == AudioTrackType::Main ? "MAIN" : "FX",
            m_selectedAudioLabel.c_str());
    }
}

}  // namespace MMM::UI
