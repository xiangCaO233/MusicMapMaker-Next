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
#include <unordered_map>

namespace MMM::UI
{
namespace
{

/// @brief Effect 方块的逻辑边长。
constexpr float EFFECT_SIZE = 92.0F;

/// @brief Main 方块的逻辑宽度。
constexpr float MAIN_WIDTH = 202.0F;

/// @brief Main 方块的逻辑高度。
constexpr float MAIN_HEIGHT = 92.0F;

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

void ProjectAudioToolView::rebuildItems(float visibleWidth)
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        m_items.clear();
        m_selectedAudioResourceId.clear();
        m_selectedAudioLabel.clear();
        m_cachedProjectRoot.clear();
        return;
    }

    m_cachedProjectRoot       = Config::pathToUtf8(project->m_projectRoot);
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
    const float usableWidth =
        std::max(EFFECT_SIZE, visibleWidth - CANVAS_PADDING * 2.0F);
    const int effectColumns =
        std::max(1,
                 static_cast<int>(std::floor((usableWidth + ITEM_GAP) /
                                             (EFFECT_SIZE + ITEM_GAP))));
    int          mainIndex   = 0;
    int          effectIndex = 0;
    std::int32_t nextZOrder  = 0;
    for ( const auto& resource : project->m_audioResources ) {
        Item item;
        item.audioResourceId = resource.m_id;
        item.label           = audioResourceLabel(resource);
        item.type            = resource.m_type;
        item.rect.width =
            resource.m_type == AudioTrackType::Main ? MAIN_WIDTH : EFFECT_SIZE;
        item.rect.height =
            resource.m_type == AudioTrackType::Main ? MAIN_HEIGHT : EFFECT_SIZE;

        const auto saved = savedPlacements.find(resource.m_id);
        if ( saved != savedPlacements.end() &&
             std::isfinite(saved->second.m_x) &&
             std::isfinite(saved->second.m_y) ) {
            item.rect.x = std::max(0.0F, saved->second.m_x);
            item.rect.y = std::max(0.0F, saved->second.m_y);
            item.zOrder = saved->second.m_zOrder;
        } else if ( resource.m_type == AudioTrackType::Main ) {
            item.rect.x = CANVAS_PADDING;
            item.rect.y = CANVAS_PADDING + static_cast<float>(mainIndex) *
                                               (MAIN_HEIGHT + ITEM_GAP);
            item.zOrder = nextZOrder;
            ++mainIndex;
        } else {
            const float effectTop =
                CANVAS_PADDING +
                static_cast<float>(mainIndex) * (MAIN_HEIGHT + ITEM_GAP);
            item.rect.x = CANVAS_PADDING +
                          static_cast<float>(effectIndex % effectColumns) *
                              (EFFECT_SIZE + ITEM_GAP);
            item.rect.y =
                effectTop + static_cast<float>(effectIndex / effectColumns) *
                                (EFFECT_SIZE + ITEM_GAP);
            item.zOrder = nextZOrder;
            ++effectIndex;
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
                .m_zOrder          = item.zOrder,
            });
    }
    engine.saveProject();
}

void ProjectAudioToolView::beginItemDrag(std::size_t itemIndex,
                                         ImVec2      mousePosition)
{
    if ( itemIndex >= m_items.size() ) return;

    Item selected = std::move(m_items[itemIndex]);
    m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(itemIndex));
    m_items.push_back(std::move(selected));
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        m_items[index].zOrder = static_cast<std::int32_t>(index);
    }

    m_draggingItem = m_items.empty()
                         ? std::optional<std::size_t>{}
                         : std::optional<std::size_t>{ m_items.size() - 1 };
    if ( !m_draggingItem ) return;
    const auto& item = m_items[*m_draggingItem];
    m_dragOffset     = {
        mousePosition.x - item.rect.x,
        mousePosition.y - item.rect.y,
    };
    m_selectedAudioResourceId = item.audioResourceId;
    m_selectedAudioLabel      = item.label;
    m_selectedAudioTrackType  = item.type;
    Logic::EditorEngine::instance().pushCommand(
        Logic::LogicCommand(Logic::CmdSetBrushAudioResource{
            item.audioResourceId,
            item.type,
        }));
    m_snapLocks = {};
    rebuildDragConstraints();
}

void ProjectAudioToolView::rebuildDragConstraints()
{
    m_dragSnapTargets.clear();
    m_dragVisibilityConstraints.clear();
    if ( !m_draggingItem || *m_draggingItem >= m_items.size() ) return;

    const std::size_t movingIndex = *m_draggingItem;
    m_dragSnapTargets.reserve(m_items.size() - 1);
    m_dragVisibilityConstraints.reserve(m_items.size() - 1);
    for ( std::size_t baseIndex = 0; baseIndex < m_items.size(); ++baseIndex ) {
        if ( baseIndex == movingIndex ) continue;
        m_dragSnapTargets.push_back(m_items[baseIndex].rect);

        ProjectAudioToolLayout::VisibilityConstraint constraint;
        constraint.base = m_items[baseIndex].rect;
        for ( std::size_t higherIndex = baseIndex + 1;
              higherIndex < movingIndex;
              ++higherIndex ) {
            constraint.fixedOccluders.push_back(m_items[higherIndex].rect);
        }
        m_dragVisibilityConstraints.push_back(std::move(constraint));
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
         projectRoot != m_cachedProjectRoot ) {
        rebuildItems(visibleWidth);
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

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        const auto& item = m_items[index];
        if ( isVisible(item.rect, visibleCanvas) ) {
            drawItem(item,
                     canvasOrigin,
                     dpiScale,
                     hoveredItem == index,
                     m_draggingItem == index &&
                         ImGui::IsMouseDown(ImGuiMouseButton_Left),
                     *drawList);
        }
    }

    if ( canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        if ( hoveredItem ) {
            beginItemDrag(*hoveredItem, mouseLogical);
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
        } else {
            rebuildLabelRects();
            persistWorkspace();
            m_draggingItem.reset();
            m_dragSnapTargets.clear();
            m_dragVisibilityConstraints.clear();
            m_snapLocks = {};
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
