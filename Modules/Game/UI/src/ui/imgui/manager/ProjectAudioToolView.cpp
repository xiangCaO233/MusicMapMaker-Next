#include "ui/imgui/manager/ProjectAudioToolView.h"

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/project/ProjectEvents.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/ProjectAudioPreviewControls.h"
#include "ui/imgui/manager/ProjectAudioToolSearch.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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

/// @brief 方块边缘缩放热区的逻辑厚度。
constexpr float RESIZE_HIT_THICKNESS = 8.0F;

/// @brief 选中方块缩放控制点的逻辑边长。
constexpr float RESIZE_HANDLE_SIZE = 7.0F;

/// @brief 默认方块布局间距。
constexpr float ITEM_GAP = 18.0F;

/// @brief 默认方块布局画布留白。
constexpr float CANVAS_PADDING = 24.0F;

/// @brief 任一下层方块必须保留的最小可见比例。
constexpr float MINIMUM_VISIBLE_RATIO =
    ProjectAudioToolLayout::STACK_MINIMUM_VISIBLE_RATIO;

/// @brief 方块开始吸附的逻辑像素距离。
constexpr float SNAP_THRESHOLD = 8.0F;

/// @brief 已吸附方块脱离目标所需的逻辑像素距离。
constexpr float SNAP_RELEASE_THRESHOLD = 16.0F;

/// @brief 最远方块之后保留的可滚动画布空间。
constexpr float CONTENT_END_PADDING = 80.0F;

/// @brief 鼠标在方块外触发试听控件显示的最大逻辑距离。
constexpr float AUDIO_CONTROLS_PROXIMITY = 14.0F;

/// @brief 获取当前主题下固定试听控件行所需的逻辑宽度。
float controlMinimumWidth(float dpiScale)
{
    const auto& style     = ImGui::GetStyle();
    const float safeScale = std::max(0.01F, dpiScale);
    const float spacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.x, 4.0F));
    return std::ceil(
        ProjectAudioToolLayout::calculateControlMinimumWidth(
            ImGui::GetFrameHeight(), spacing, style.FramePadding.x, 4U) /
        safeScale);
}

/// @brief 获取指定音频类型允许的最小逻辑宽度。
float minimumItemWidth(AudioTrackType type, float dpiScale)
{
    const float typeMinimum =
        type == AudioTrackType::Main ? DEFAULT_MAIN_WIDTH : DEFAULT_EFFECT_SIZE;
    return std::max(typeMinimum, controlMinimumWidth(dpiScale));
}

/// @brief 获取类型、文件名、进度条和固定按钮行所需的最小逻辑高度。
float minimumItemHeight(float dpiScale)
{
    const auto& style     = ImGui::GetStyle();
    const float safeScale = std::max(0.01F, dpiScale);
    const float progressHeight =
        std::clamp(ImGui::GetFontSize() * 0.18F, 3.0F, 7.0F);
    const float progressSpacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.y, 3.0F));
    return std::ceil(ProjectAudioToolLayout::calculateControlMinimumHeight(
                         ImGui::GetTextLineHeight(),
                         progressHeight,
                         progressSpacing,
                         ImGui::GetFrameHeight(),
                         style.FramePadding.y,
                         style.ItemSpacing.y) /
                     safeScale);
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
    return ProjectAudioToolLayout::calculateDefaultWidth(
        textWidth, horizontalPadding, minimumItemWidth(type, dpiScale));
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

/// @brief 单个音频方块内按需试听按钮的屏幕布局。
struct ItemAudioControlLayout {
    /// @brief 可直接传给通用试听控件的屏幕布局。
    ProjectAudioPreviewControlsLayout controls;

    /// @brief 文件名文本可使用区域的屏幕底边。
    float labelBottom{ 0.0F };
};

/// @brief 计算方块内固定尺寸的播放、暂停、停止和音量按钮布局。
/// @param itemRect 方块完整屏幕区域。
/// @return 由主题控件尺寸决定的统一按钮布局。
/// @warning 每个可见音频方块每帧调用，不得引入分配或阻塞操作。
ItemAudioControlLayout calculateItemAudioControlLayout(
    const ProjectAudioToolLayout::Rect& itemRect)
{
    const auto& style             = ImGui::GetStyle();
    const float horizontalPadding = std::max(1.0F, style.FramePadding.x);
    const float verticalPadding   = std::max(1.0F, style.FramePadding.y);
    const float spacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.x, 4.0F));
    const float progressHeight =
        std::clamp(ImGui::GetFontSize() * 0.18F, 3.0F, 7.0F);
    const float progressSpacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.y, 3.0F));
    const float buttonSize  = ImGui::GetFrameHeight();
    const float totalWidth  = buttonSize * 4.0F + spacing * 3.0F;
    const float totalHeight = progressHeight + progressSpacing + buttonSize;
    const float top         = itemRect.bottom() - verticalPadding - totalHeight;
    return {
        .controls =
            ProjectAudioPreviewControlsLayout{
                .topLeft =
                    {
                        itemRect.x +
                            std::max(horizontalPadding,
                                     (itemRect.width - totalWidth) * 0.5F),
                        top,
                    },
                .width           = totalWidth,
                .buttonSize      = buttonSize,
                .buttonSpacing   = spacing,
                .progressHeight  = progressHeight,
                .progressSpacing = progressSpacing,
            },
        .labelBottom =
            std::max(itemRect.y,
                     top - std::max(1.0F, style.ItemSpacing.y)),
    };
}

/// @brief 判断屏幕坐标是否位于试听进度条或按钮行组成的完整控件区。
/// @param layout 试听控件的绝对屏幕布局。
/// @param point 待检查的屏幕坐标。
/// @return 坐标位于控件区时返回 true。
/// @warning 每个可见音频方块每帧调用，不得引入分配或阻塞操作。
bool containsItemAudioControls(const ProjectAudioPreviewControlsLayout& layout,
                               ImVec2                                   point)
{
    const float controlsHeight =
        layout.progressHeight + layout.progressSpacing + layout.buttonSize;
    return point.x >= layout.topLeft.x &&
           point.x <= layout.topLeft.x + layout.width &&
           point.y >= layout.topLeft.y &&
           point.y <= layout.topLeft.y + controlsHeight;
}

/// @brief 判断逻辑点是否位于矩形内。
bool contains(const ProjectAudioToolLayout::Rect& rect, ImVec2 point)
{
    return point.x >= rect.x && point.x <= rect.right() && point.y >= rect.y &&
           point.y <= rect.bottom();
}

/// @brief 计算逻辑点到矩形的平方距离；矩形内部距离为零。
/// @warning UI 热路径：每帧仅对可见方块执行常数次浮点运算。
float squaredDistanceToRect(const ProjectAudioToolLayout::Rect& rect,
                            ImVec2                              point)
{
    const float deltaX = point.x < rect.x         ? rect.x - point.x
                         : point.x > rect.right() ? point.x - rect.right()
                                                  : 0.0F;
    const float deltaY = point.y < rect.y          ? rect.y - point.y
                         : point.y > rect.bottom() ? point.y - rect.bottom()
                                                   : 0.0F;
    return deltaX * deltaX + deltaY * deltaY;
}

/// @brief 由两个逻辑画布点构造方向无关的矩形。
ProjectAudioToolLayout::Rect rectFromPoints(ImVec2 first, ImVec2 second)
{
    const float left   = std::min(first.x, second.x);
    const float top    = std::min(first.y, second.y);
    const float right  = std::max(first.x, second.x);
    const float bottom = std::max(first.y, second.y);
    return { left, top, right - left, bottom - top };
}

/// @brief 判断外层矩形是否完整包含内层矩形。
bool containsRect(const ProjectAudioToolLayout::Rect& outer,
                  const ProjectAudioToolLayout::Rect& inner)
{
    constexpr float EPSILON = 0.5F;
    return inner.x >= outer.x - EPSILON &&
           inner.right() <= outer.right() + EPSILON &&
           inner.y >= outer.y - EPSILON &&
           inner.bottom() <= outer.bottom() + EPSILON;
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
        m_interactionBaseLabelRects.clear();
        m_batchDragEntries.clear();
        m_batchDragUnionCells.clear();
        m_marqueeBaseSelection.clear();
        m_searchBuffer.fill('\0');
        m_searchResults.clear();
        m_searchFocusRequestId.clear();
        m_renameAudioResourceId.clear();
        m_renameBuffer.fill('\0');
        m_selectedAudioResourceId.clear();
        m_selectedAudioLabel.clear();
        m_openVolumeEditorResourceId.clear();
        m_cachedProjectRoot.clear();
        m_cachedDpiScale     = 0.0F;
        m_batchDragging      = false;
        m_marqueeSelecting   = false;
        m_searchResultsDirty = true;
        return;
    }

    const std::string projectRoot = Config::pathToUtf8(project->m_projectRoot);
    const bool        projectChanged = projectRoot != m_cachedProjectRoot;
    std::unordered_set<std::string> batchSelectedResourceIds;
    if ( !projectChanged ) {
        for ( const auto& item : m_items ) {
            if ( item.batchSelected ) {
                batchSelectedResourceIds.insert(item.audioResourceId);
            }
        }
    } else {
        m_searchBuffer.fill('\0');
        m_searchFocusRequestId.clear();
    }

    m_draggingItem.reset();
    m_resizingItem.reset();
    m_batchDragging    = false;
    m_marqueeSelecting = false;
    m_batchDragEntries.clear();
    m_batchDragUnionCells.clear();
    m_marqueeBaseSelection.clear();
    m_openVolumeEditorResourceId.clear();
    m_resizeHandle = ResizeHandle::None;
    m_snapLocks    = {};

    m_cachedProjectRoot       = projectRoot;
    m_cachedDpiScale          = dpiScale;
    auto& workspace           = project->m_settings.m_workspace;
    m_selectedAudioResourceId = workspace.m_projectAudioToolSelectedResourceId;
    m_brushAudioVolume =
        std::isfinite(workspace.m_projectAudioToolBrushVolume)
            ? std::max(0.0F, workspace.m_projectAudioToolBrushVolume)
            : 1.0F;

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
    m_interactionBaseLabelRects.clear();
    m_items.reserve(project->m_audioResources.size());
    float        defaultCursorX = CANVAS_PADDING;
    float        defaultCursorY = CANVAS_PADDING;
    float        defaultRowHeight{ 0.0F };
    std::int32_t nextZOrder = 0;
    for ( const auto& resource : project->m_audioResources ) {
        Item item;
        item.audioResourceId = resource.m_id;
        item.previewPoolKey =
            makeProjectAudioPreviewPoolKey("tool/" + resource.m_id);
        item.label         = audioResourceLabel(resource);
        item.type          = resource.m_type;
        item.batchSelected = batchSelectedResourceIds.contains(resource.m_id);
        item.rect.width    = defaultItemWidth(item.label, item.type, dpiScale);
        const float defaultHeight = resource.m_type == AudioTrackType::Main
                                        ? DEFAULT_MAIN_HEIGHT
                                        : DEFAULT_EFFECT_SIZE;
        item.rect.height = std::max(defaultHeight, minimumItemHeight(dpiScale));

        const auto saved = savedPlacements.find(resource.m_id);
        if ( saved != savedPlacements.end() ) {
            const bool hasSavedWidth  = std::isfinite(saved->second.m_width) &&
                                        saved->second.m_width > 0.0F;
            const bool hasSavedHeight = std::isfinite(saved->second.m_height) &&
                                        saved->second.m_height > 0.0F;
            item.widthCustomized      = hasSavedWidth;
            item.heightCustomized     = hasSavedHeight;
            if ( hasSavedWidth ) {
                item.rect.width =
                    std::max(minimumItemWidth(item.type, dpiScale),
                             saved->second.m_width);
            }
            if ( hasSavedHeight ) {
                item.rect.height = std::max(minimumItemHeight(dpiScale),
                                            saved->second.m_height);
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
    m_searchResultsDirty = true;
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

void ProjectAudioToolView::prepareInteractionLabelRects()
{
    m_interactionBaseLabelRects.resize(m_items.size());
    const auto isMovingItem = [this](std::size_t index) {
        if ( m_batchDragging ) return m_items[index].batchSelected;
        const auto activeItem =
            m_draggingItem ? m_draggingItem : m_resizingItem;
        return activeItem && *activeItem == index;
    };

    std::size_t fixedConstraintIndex = 0;
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        if ( isMovingItem(index) ) {
            m_interactionBaseLabelRects[index] = m_items[index].rect;
            continue;
        }

        ProjectAudioToolLayout::Rect visibleRect{};
        if ( fixedConstraintIndex < m_dragVisibilityConstraints.size() ) {
            const auto& fixedVisibleCells =
                m_dragVisibilityConstraints[fixedConstraintIndex]
                    .fixedVisibleCells;
            for ( const auto& cell : fixedVisibleCells ) {
                if ( ProjectAudioToolLayout::area(cell) >
                     ProjectAudioToolLayout::area(visibleRect) ) {
                    visibleRect = cell;
                }
            }
        }
        if ( ProjectAudioToolLayout::area(visibleRect) <= 0.0F ) {
            visibleRect = m_items[index].rect;
        }
        m_interactionBaseLabelRects[index] = visibleRect;
        ++fixedConstraintIndex;
    }
    refreshInteractionLabelRects();
}

void ProjectAudioToolView::refreshInteractionLabelRects()
{
    if ( m_interactionBaseLabelRects.size() != m_items.size() ) return;

    const auto activeItem = m_draggingItem ? m_draggingItem : m_resizingItem;
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        const bool movingItem = m_batchDragging
                                    ? m_items[index].batchSelected
                                    : activeItem && *activeItem == index;
        auto visibleRect      = movingItem ? m_items[index].rect
                                           : m_interactionBaseLabelRects[index];

        if ( m_batchDragging ) {
            for ( const auto& entry : m_batchDragEntries ) {
                if ( entry.itemIndex <= index ||
                     entry.itemIndex >= m_items.size() ) {
                    continue;
                }
                visibleRect =
                    ProjectAudioToolLayout::largestVisibleCellWithOneOccluder(
                        visibleRect, m_items[entry.itemIndex].rect);
            }
        } else if ( activeItem && *activeItem > index &&
                    *activeItem < m_items.size() ) {
            visibleRect =
                ProjectAudioToolLayout::largestVisibleCellWithOneOccluder(
                    visibleRect, m_items[*activeItem].rect);
        }
        m_items[index].labelRect = visibleRect;
    }
}

void ProjectAudioToolView::persistWorkspace()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project ) return;

    auto& workspace = project->m_settings.m_workspace;
    workspace.m_projectAudioToolSelectedResourceId = m_selectedAudioResourceId;
    workspace.m_projectAudioToolBrushVolume        = m_brushAudioVolume;
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
            m_brushAudioVolume,
        }));
    return activeIndex;
}

/// @brief 按项目选项试听一次新选中的 Effect 音频资源。
/// @param item 本次完成选择的资源方块。
/// @warning 低频用户操作路径：首次试听可能同步加载音频，只允许在明确的
/// 选择动作完成后调用。
void ProjectAudioToolView::previewEffectSelection(const Item& item) const
{
    if ( item.type != AudioTrackType::Effect ) return;

    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || !project->m_settings.m_workspace
                          .m_projectAudioToolPreviewEffectOnSelection ) {
        return;
    }

    (void)controlProjectAudioPreview(*project,
                                     item.audioResourceId,
                                     item.previewPoolKey,
                                     ProjectAudioPreviewAction::Play,
                                     m_brushAudioVolume);
}

void ProjectAudioToolView::clearActiveItem()
{
    m_selectedAudioResourceId.clear();
    m_selectedAudioLabel.clear();
    m_selectedAudioTrackType = AudioTrackType::Effect;
    Logic::EditorEngine::instance().pushCommand(
        Logic::LogicCommand(Logic::CmdSetBrushAudioResource{
            {},
            AudioTrackType::Effect,
            m_brushAudioVolume,
        }));
}

void ProjectAudioToolView::beginItemDrag(std::size_t itemIndex,
                                         ImVec2      mousePosition)
{
    const auto activeIndex = activateItem(itemIndex);
    if ( !activeIndex ) return;

    m_draggingItem       = *activeIndex;
    m_itemDragStartMouse = mousePosition;
    m_itemDragMoved      = false;
    const auto& item     = m_items[*activeIndex];
    m_dragOffset         = {
        mousePosition.x - item.rect.x,
        mousePosition.y - item.rect.y,
    };
    m_snapLocks = {};
    rebuildInteractionConstraints();
    prepareInteractionLabelRects();
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
    prepareInteractionLabelRects();
}

void ProjectAudioToolView::clearBatchSelection()
{
    for ( auto& item : m_items ) {
        item.batchSelected = false;
    }
}

std::size_t ProjectAudioToolView::batchSelectionCount() const
{
    return static_cast<std::size_t>(std::ranges::count_if(
        m_items, [](const Item& item) { return item.batchSelected; }));
}

void ProjectAudioToolView::rebuildSearchResults()
{
    m_searchResultsDirty = false;
    m_searchResults.clear();
    const std::string_view query =
        ProjectAudioToolSearch::trimAsciiWhitespace(m_searchBuffer.data());
    if ( query.empty() ) {
        m_searchHighlightedIndex = 0;
        return;
    }

    m_searchResults.reserve(m_items.size());
    for ( const auto& item : m_items ) {
        const auto labelScore =
            ProjectAudioToolSearch::scoreCandidate(item.label, query);
        const auto resourceIdScore =
            ProjectAudioToolSearch::scoreCandidate(item.audioResourceId, query);
        if ( !labelScore && !resourceIdScore ) continue;

        SearchResult result;
        result.audioResourceId = item.audioResourceId;
        result.displayLabel =
            item.type == AudioTrackType::Main ? "[MAIN] " : "[FX] ";
        result.displayLabel.append(item.label);
        result.score =
            std::max(labelScore.value_or(std::numeric_limits<int>::min()),
                     resourceIdScore.value_or(std::numeric_limits<int>::min()));
        m_searchResults.push_back(std::move(result));
    }
    std::ranges::sort(m_searchResults,
                      [](const SearchResult& lhs, const SearchResult& rhs) {
                          if ( lhs.score != rhs.score )
                              return lhs.score > rhs.score;
                          if ( lhs.displayLabel != rhs.displayLabel ) {
                              return lhs.displayLabel < rhs.displayLabel;
                          }
                          return lhs.audioResourceId < rhs.audioResourceId;
                      });
    m_searchHighlightedIndex = 0;
}

void ProjectAudioToolView::requestSearchResultFocus(
    const std::string& audioResourceId)
{
    m_searchFocusRequestId = audioResourceId;
}

void ProjectAudioToolView::requestItemRename(std::size_t itemIndex)
{
    if ( itemIndex >= m_items.size() ) return;
    const auto& item         = m_items[itemIndex];
    m_renameAudioResourceId  = item.audioResourceId;
    m_shouldOpenRenamePopup  = true;
    m_shouldFocusRenameInput = true;
    const std::size_t copySize =
        std::min(item.label.size(), m_renameBuffer.size() - 1U);
    m_renameBuffer.fill('\0');
    std::memcpy(m_renameBuffer.data(), item.label.data(), copySize);
    m_renameBuffer[copySize] = '\0';
}

void ProjectAudioToolView::renderRenamePopup(float dpiScale)
{
    const std::string popupTitle =
        std::string(TR("ui.file_manager.rename_title")) +
        "###ProjectAudioToolRenamePopup";
    if ( m_shouldOpenRenamePopup ) {
        FeedbackOpenPopup(popupTitle.c_str());
        m_shouldOpenRenamePopup = false;
    }

    bool                           open = true;
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( !modalScope.begin(popupTitle.c_str(),
                           &open,
                           ImGuiWindowFlags_NoCollapse,
                           { 380.0F * dpiScale, 0.0F }) ) {
        return;
    }

    ImGui::TextUnformatted(TR("ui.file_manager.rename_label").data());
    if ( m_shouldFocusRenameInput ) {
        ImGui::SetKeyboardFocusHere();
        m_shouldFocusRenameInput = false;
    }
    const bool enterPressed =
        ImGui::InputText("##ProjectAudioToolRenameInput",
                         m_renameBuffer.data(),
                         m_renameBuffer.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll);
    const ImVec2 buttonSize{ 132.0F * dpiScale, 0.0F };
    const bool   confirmClicked =
        FeedbackButton(TR("ui.file_manager.context.rename").data(), buttonSize);
    if ( (enterPressed || confirmClicked) &&
         !m_renameAudioResourceId.empty() ) {
        Logic::EditorEngine::instance().pushCommand(
            Logic::LogicCommand(Logic::CmdRenameAudioResource{
                .id          = m_renameAudioResourceId,
                .newFileName = m_renameBuffer.data(),
            }));
        m_renameAudioResourceId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if ( FeedbackButton(TR("ui.common.cancel").data(), buttonSize) ) {
        m_renameAudioResourceId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProjectAudioToolView::beginBatchDrag(std::size_t itemIndex,
                                          ImVec2      mousePosition)
{
    if ( itemIndex >= m_items.size() || !m_items[itemIndex].batchSelected ) {
        return;
    }

    std::vector<Item> reorderedItems;
    reorderedItems.reserve(m_items.size());
    for ( auto& item : m_items ) {
        if ( !item.batchSelected ) {
            reorderedItems.push_back(std::move(item));
        }
    }
    for ( auto& item : m_items ) {
        if ( item.batchSelected ) {
            reorderedItems.push_back(std::move(item));
        }
    }
    m_items = std::move(reorderedItems);
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        m_items[index].zOrder = static_cast<std::int32_t>(index);
    }

    m_batchDragEntries.clear();
    std::vector<ProjectAudioToolLayout::Rect> selectedRects;
    selectedRects.reserve(batchSelectionCount());
    bool hasBounds = false;
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        auto& item = m_items[index];
        if ( !item.batchSelected ) continue;

        m_batchDragEntries.push_back(BatchDragEntry{
            .itemIndex = index,
            .startRect = item.rect,
        });
        selectedRects.push_back(item.rect);
        item.labelRect = item.rect;
        if ( !hasBounds ) {
            m_batchDragInitialBounds = item.rect;
            hasBounds                = true;
        } else {
            const float right =
                std::max(m_batchDragInitialBounds.right(), item.rect.right());
            const float bottom =
                std::max(m_batchDragInitialBounds.bottom(), item.rect.bottom());
            m_batchDragInitialBounds.x =
                std::min(m_batchDragInitialBounds.x, item.rect.x);
            m_batchDragInitialBounds.y =
                std::min(m_batchDragInitialBounds.y, item.rect.y);
            m_batchDragInitialBounds.width = right - m_batchDragInitialBounds.x;
            m_batchDragInitialBounds.height =
                bottom - m_batchDragInitialBounds.y;
        }
    }
    if ( !hasBounds ) return;

    m_batchDragCurrentBounds = m_batchDragInitialBounds;
    m_batchDragUnionCells =
        ProjectAudioToolLayout::buildUnionCells(selectedRects);
    m_batchDragOffset = {
        mousePosition.x - m_batchDragInitialBounds.x,
        mousePosition.y - m_batchDragInitialBounds.y,
    };
    m_batchDragging = true;
    m_snapLocks     = {};
    rebuildBatchDragConstraints();
    prepareInteractionLabelRects();
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

void ProjectAudioToolView::rebuildBatchDragConstraints()
{
    m_dragSnapTargets.clear();
    m_dragVisibilityConstraints.clear();
    if ( !m_batchDragging || m_batchDragEntries.empty() ) return;

    const std::size_t fixedItemCount =
        m_items.size() - m_batchDragEntries.size();
    m_dragSnapTargets.reserve(fixedItemCount);
    m_dragVisibilityConstraints.reserve(fixedItemCount);
    for ( std::size_t baseIndex = 0; baseIndex < fixedItemCount; ++baseIndex ) {
        m_dragSnapTargets.push_back(m_items[baseIndex].rect);

        std::vector<ProjectAudioToolLayout::Rect> fixedOccluders;
        for ( std::size_t higherIndex = baseIndex + 1;
              higherIndex < fixedItemCount;
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
    if ( item.batchSelected ) {
        constexpr ImU32 BATCH_SELECTION_FILL = IM_COL32(76, 255, 190, 38);
        drawList.AddRectFilled(
            minimum, maximum, BATCH_SELECTION_FILL, rounding);
    }

    const auto labelRect = toScreenRect(item.labelRect, canvasOrigin, dpiScale);
    const auto audioControlLayout = calculateItemAudioControlLayout(screenRect);
    const float horizontalPadding = std::max(1.0F, style.FramePadding.x);
    const float verticalPadding   = std::max(1.0F, style.FramePadding.y);
    const float labelBottom =
        audioControlLayout.labelBottom > labelRect.y
            ? std::min(labelRect.bottom(), audioControlLayout.labelBottom)
            : labelRect.bottom();
    const float labelHeight = std::max(1.0F, labelBottom - labelRect.y);
    const float filenameHeight =
        std::min(ImGui::GetTextLineHeight(), labelHeight);
    const ImVec2 labelStart{
        labelRect.x + horizontalPadding,
        std::max(labelRect.y, labelBottom - verticalPadding - filenameHeight),
    };
    const float labelWidth =
        std::max(1.0F, labelRect.width - horizontalPadding * 2.0F);
    drawList.PushClipRect(
        { labelRect.x, labelRect.y }, { labelRect.right(), labelBottom }, true);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06F, 0.08F, 0.11F, 1.0F));
    Utils::drawScrollingText(
        item.label, labelStart, labelWidth, filenameHeight);
    ImGui::PopStyleColor();
    drawList.PopClipRect();

    const char* typeLabel = item.type == AudioTrackType::Main ? "MAIN" : "FX";
    drawList.AddText(
        { minimum.x + horizontalPadding, minimum.y + verticalPadding },
        ImGui::GetColorU32(ImVec4(0.06F, 0.08F, 0.11F, 0.78F)),
        typeLabel);

    if ( item.batchSelected ) {
        constexpr ImU32 BATCH_SELECTION_BORDER = IM_COL32(76, 255, 190, 235);
        const float     inset = std::max(2.0F, 3.0F * dpiScale);
        drawList.AddRect({ minimum.x + inset, minimum.y + inset },
                         { maximum.x - inset, maximum.y - inset },
                         BATCH_SELECTION_BORDER,
                         std::max(0.0F, rounding - inset),
                         0,
                         std::max(2.0F, 2.25F * dpiScale));
    }

    if ( selected && !item.batchSelected ) {
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

    ImGui::TextWrapped("%s", TR("ui.project_audio_tool.hint").data());

    auto& previewEffectOnSelection =
        project->m_settings.m_workspace
            .m_projectAudioToolPreviewEffectOnSelection;
    if ( ::MMM::UI::FeedbackCheckbox(
             TR("ui.project_audio_tool.preview_effect_on_selection").data(),
             &previewEffectOnSelection) ) {
        Logic::EditorEngine::instance().saveProject();
    }

    ImGui::SetNextItemWidth(-1.0F);
    const bool searchSubmitted =
        ImGui::InputTextWithHint("##ProjectAudioToolSearch",
                                 TR("ui.project_audio_tool.search_hint").data(),
                                 m_searchBuffer.data(),
                                 m_searchBuffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_AutoSelectAll);
    const bool searchInputActive = ImGui::IsItemActive();
    if ( ImGui::IsItemEdited() ) {
        m_searchResultsDirty = true;
    }
    if ( m_searchResultsDirty ) {
        rebuildSearchResults();
    }

    bool searchHighlightMoved = false;
    if ( searchInputActive && !m_searchResults.empty() ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow) ) {
            m_searchHighlightedIndex =
                (m_searchHighlightedIndex + 1) % m_searchResults.size();
            searchHighlightMoved = true;
        } else if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow) ) {
            m_searchHighlightedIndex =
                (m_searchHighlightedIndex + m_searchResults.size() - 1) %
                m_searchResults.size();
            searchHighlightMoved = true;
        }
    }
    if ( searchSubmitted && !m_searchResults.empty() ) {
        requestSearchResultFocus(
            m_searchResults[m_searchHighlightedIndex].audioResourceId);
    }

    const std::string_view searchQuery =
        ProjectAudioToolSearch::trimAsciiWhitespace(m_searchBuffer.data());
    const float statusHeight =
        ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if ( !searchQuery.empty() ) {
        if ( m_searchResults.empty() ) {
            ImGui::TextDisabled("%s", TR("ui.search.no_results").data());
        } else {
            ImGui::TextDisabled(
                "%s: %zu",
                TR("ui.project_audio_tool.search_results").data(),
                m_searchResults.size());
            const ImGuiStyle& style           = ImGui::GetStyle();
            const float       resultRowHeight = ImGui::GetFrameHeight();
            const float       splitterHeight =
                std::max(6.0F * dpiScale, style.SeparatorSize * 4.0F);
            const float minimumCanvasHeight =
                resultRowHeight * 3.0F + style.WindowPadding.y * 2.0F;
            const float reservedHeight = minimumCanvasHeight + statusHeight +
                                         splitterHeight +
                                         style.ItemSpacing.y * 3.0F;
            const float searchLayoutAvailableHeight =
                ImGui::GetContentRegionAvail().y;
            const float resultListHeight =
                ProjectAudioToolSearch::calculateResultPaneHeight(
                    m_searchResultPaneHeight,
                    resultRowHeight,
                    m_searchResults.size(),
                    style.WindowPadding.y,
                    searchLayoutAvailableHeight,
                    reservedHeight);
            ImGui::BeginChild("ProjectAudioToolSearchResults",
                              { 0.0F, resultListHeight },
                              true);
            if ( searchHighlightMoved ) {
                const float targetScroll =
                    (static_cast<float>(m_searchHighlightedIndex) + 0.5F) *
                        resultRowHeight -
                    resultListHeight * 0.5F;
                ImGui::SetScrollY(std::max(0.0F, targetScroll));
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_searchResults.size()),
                          resultRowHeight);
            while ( clipper.Step() ) {
                for ( int resultIndex = clipper.DisplayStart;
                      resultIndex < clipper.DisplayEnd;
                      ++resultIndex ) {
                    auto& result =
                        m_searchResults[static_cast<std::size_t>(resultIndex)];
                    ImGui::PushID(result.audioResourceId.c_str());
                    const bool clicked = FeedbackSelectable(
                        result.displayLabel.c_str(),
                        static_cast<std::size_t>(resultIndex) ==
                            m_searchHighlightedIndex,
                        ImGuiSelectableFlags_None,
                        { 0.0F, resultRowHeight });
                    if ( ImGui::IsItemHovered() ) {
                        m_searchHighlightedIndex =
                            static_cast<std::size_t>(resultIndex);
                    }
                    if ( clicked ) {
                        m_searchHighlightedIndex =
                            static_cast<std::size_t>(resultIndex);
                        requestSearchResultFocus(result.audioResourceId);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            const ImVec2 splitterStart = ImGui::GetCursorScreenPos();
            const float  splitterWidth =
                std::max(1.0F, ImGui::GetContentRegionAvail().x);
            ImGui::InvisibleButton("##ProjectAudioToolSearchResultSplitter",
                                   { splitterWidth, splitterHeight });
            const bool splitterActive  = ImGui::IsItemActive();
            const bool splitterHovered = ImGui::IsItemHovered();
            if ( splitterActive ) {
                m_searchResultPaneHeight =
                    ProjectAudioToolSearch::calculateResultPaneHeight(
                        resultListHeight + ImGui::GetIO().MouseDelta.y,
                        resultRowHeight,
                        m_searchResults.size(),
                        style.WindowPadding.y,
                        searchLayoutAvailableHeight,
                        reservedHeight);
            }
            if ( splitterHovered || splitterActive ) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }

            const ImU32 splitterColor = ImGui::GetColorU32(
                splitterActive ? ImGuiCol_SeparatorActive
                               : (splitterHovered ? ImGuiCol_SeparatorHovered
                                                  : ImGuiCol_Separator));
            const float splitterLineY = splitterStart.y + splitterHeight * 0.5F;
            ImGui::GetWindowDrawList()->AddLine(
                { splitterStart.x, splitterLineY },
                { splitterStart.x + splitterWidth, splitterLineY },
                splitterColor,
                std::max(1.0F, style.SeparatorSize));
        }
    }
    ImGui::Separator();
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

    if ( !m_searchFocusRequestId.empty() ) {
        const auto requestedItem =
            std::ranges::find_if(m_items, [this](const Item& item) {
                return item.audioResourceId == m_searchFocusRequestId;
            });
        if ( requestedItem != m_items.end() ) {
            const auto activeIndex = activateItem(static_cast<std::size_t>(
                std::distance(m_items.begin(), requestedItem)));
            if ( activeIndex ) {
                previewEffectSelection(m_items[*activeIndex]);
                const auto& activeRect = m_items[*activeIndex].rect;
                const float targetScrollX =
                    (activeRect.x + activeRect.width * 0.5F) * dpiScale -
                    visibleSizePixels.x * 0.5F;
                const float targetScrollY =
                    (activeRect.y + activeRect.height * 0.5F) * dpiScale -
                    visibleSizePixels.y * 0.5F;
                ImGui::SetScrollX(std::max(0.0F, targetScrollX));
                ImGui::SetScrollY(std::max(0.0F, targetScrollY));
                persistWorkspace();
            }
        }
        m_searchFocusRequestId.clear();
    }

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

    ImDrawList* drawList             = ImGui::GetWindowDrawList();
    const bool  audioControlsEnabled = !m_draggingItem && !m_resizingItem &&
                                       !m_batchDragging && !m_marqueeSelecting;
    std::optional<std::size_t> audioControlsItem;
    if ( audioControlsEnabled && !m_openVolumeEditorResourceId.empty() ) {
        const auto openVolumeItem = std::ranges::find(
            m_items, m_openVolumeEditorResourceId, &Item::audioResourceId);
        if ( openVolumeItem != m_items.end() &&
             isVisible(openVolumeItem->rect, visibleCanvas) ) {
            audioControlsItem = static_cast<std::size_t>(
                std::distance(m_items.begin(), openVolumeItem));
        } else {
            m_openVolumeEditorResourceId.clear();
        }
    }
    if ( audioControlsEnabled && !audioControlsItem && canvasHovered ) {
        audioControlsItem = hoveredItem;
    }
    if ( audioControlsEnabled && !audioControlsItem && canvasHovered ) {
        const float maximumDistanceSquared =
            AUDIO_CONTROLS_PROXIMITY * AUDIO_CONTROLS_PROXIMITY;
        float nearestDistanceSquared = maximumDistanceSquared;
        for ( std::size_t reverse = m_items.size(); reverse > 0; --reverse ) {
            const std::size_t index = reverse - 1U;
            if ( !isVisible(m_items[index].rect, visibleCanvas) ) continue;
            const float distanceSquared =
                squaredDistanceToRect(m_items[index].rect, mouseLogical);
            if ( distanceSquared < nearestDistanceSquared ) {
                nearestDistanceSquared = distanceSquared;
                audioControlsItem      = index;
            }
        }
    }

    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        const auto& item = m_items[index];
        if ( !isVisible(item.rect, visibleCanvas) ) continue;

        drawItem(item,
                 canvasOrigin,
                 dpiScale,
                 hoveredItem == index,
                 (m_draggingItem == index || m_resizingItem == index ||
                  (m_batchDragging && item.batchSelected)) &&
                     ImGui::IsMouseDown(ImGuiMouseButton_Left),
                 *drawList);
    }

    bool        audioControlsHovered = false;
    std::string volumeEditorResourceId;
    bool        brushVolumeChanged = false;
    if ( audioControlsItem && *audioControlsItem < m_items.size() ) {
        auto& item = m_items[*audioControlsItem];
        if ( item.previewPoolKey.empty() ) {
            item.previewPoolKey =
                makeProjectAudioPreviewPoolKey("tool/" + item.audioResourceId);
        }
        const auto itemScreenRect =
            toScreenRect(item.rect, canvasOrigin, dpiScale);
        const auto controls = calculateItemAudioControlLayout(itemScreenRect);
        const bool controlsPointerInside = containsItemAudioControls(
            controls.controls, ImGui::GetIO().MousePos);
        const auto result =
            renderProjectAudioPreviewControls(item.audioResourceId.c_str(),
                                              *project,
                                              item.audioResourceId,
                                              item.previewPoolKey,
                                              m_brushAudioVolume,
                                              &m_brushAudioVolume,
                                              controls.controls,
                                              controlsPointerInside);
        audioControlsHovered =
            controlsPointerInside || result.hovered || result.volumeEditorOpen;
        if ( result.volumeEditorOpen ) {
            m_openVolumeEditorResourceId = item.audioResourceId;
            volumeEditorResourceId       = item.audioResourceId;
            brushVolumeChanged = brushVolumeChanged || result.volumeChanged;
        } else if ( m_openVolumeEditorResourceId == item.audioResourceId ) {
            m_openVolumeEditorResourceId.clear();
        }
    }

    if ( !volumeEditorResourceId.empty() ) {
        const auto volumeItem = std::ranges::find(
            m_items, volumeEditorResourceId, &Item::audioResourceId);
        const bool resourceChanged =
            m_selectedAudioResourceId != volumeEditorResourceId;
        if ( resourceChanged && volumeItem != m_items.end() ) {
            if ( activateItem(static_cast<std::size_t>(
                     std::distance(m_items.begin(), volumeItem))) ) {
                rebuildLabelRects();
            }
        } else if ( brushVolumeChanged ) {
            Logic::EditorEngine::instance().pushCommand(
                Logic::LogicCommand(Logic::CmdSetBrushAudioResource{
                    m_selectedAudioResourceId,
                    m_selectedAudioTrackType,
                    m_brushAudioVolume,
                }));
        }
        if ( resourceChanged || brushVolumeChanged ) {
            persistWorkspace();
        }
    }

    if ( hoveredItem && !audioControlsHovered && !m_draggingItem &&
         !m_resizingItem && !m_batchDragging && !m_marqueeSelecting &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
        requestItemRename(*hoveredItem);
    }
    ResizeHandle hoveredResizeHandle = ResizeHandle::None;
    if ( m_resizingItem ) {
        hoveredResizeHandle = m_resizeHandle;
    } else if ( hoveredItem && !audioControlsHovered &&
                !m_items[*hoveredItem].batchSelected ) {
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

    const auto refreshMarqueeSelection = [&]() {
        if ( m_marqueeBaseSelection.size() != m_items.size() ) return;
        const auto selectionRect = rectFromPoints(m_marqueeStart, m_marqueeEnd);
        const bool selectionValid =
            selectionRect.width > 0.5F && selectionRect.height > 0.5F;
        const auto selectionMode =
            Config::AppConfig::instance().getEditorSettings().selectionMode;
        for ( std::size_t index = 0; index < m_items.size(); ++index ) {
            bool selected = m_marqueeBaseSelection[index] != 0;
            if ( selectionValid ) {
                selected =
                    selected ||
                    (selectionMode == Config::SelectionMode::Strict
                         ? containsRect(selectionRect, m_items[index].rect)
                         : ProjectAudioToolLayout::intersection(
                               selectionRect, m_items[index].rect)
                               .has_value());
            }
            m_items[index].batchSelected = selected;
        }
    };

    if ( canvasHovered && !audioControlsHovered && !m_draggingItem &&
         !m_resizingItem && !m_batchDragging && !m_marqueeSelecting &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        if ( hoveredItem ) {
            if ( m_items[*hoveredItem].batchSelected ) {
                beginBatchDrag(*hoveredItem, mouseLogical);
            } else if ( hoveredResizeHandle != ResizeHandle::None ) {
                clearBatchSelection();
                beginItemResize(
                    *hoveredItem, hoveredResizeHandle, mouseLogical);
            } else {
                clearBatchSelection();
                beginItemDrag(*hoveredItem, mouseLogical);
            }
        } else {
            if ( !m_selectedAudioResourceId.empty() ) {
                clearActiveItem();
                persistWorkspace();
            }
            const bool additiveSelection =
                ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
            m_marqueeSelecting = true;
            m_marqueeStart     = mouseLogical;
            m_marqueeEnd       = mouseLogical;
            m_marqueeBaseSelection.assign(m_items.size(), 0);
            if ( additiveSelection ) {
                for ( std::size_t index = 0; index < m_items.size(); ++index ) {
                    m_marqueeBaseSelection[index] =
                        m_items[index].batchSelected ? 1 : 0;
                }
            } else {
                clearBatchSelection();
            }
        }
    }

    if ( m_marqueeSelecting ) {
        m_marqueeEnd = mouseLogical;
        refreshMarqueeSelection();
        if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            m_marqueeSelecting = false;
            m_marqueeBaseSelection.clear();
        }
    }

    if ( m_draggingItem && *m_draggingItem < m_items.size() ) {
        auto&       item   = m_items[*m_draggingItem];
        const float deltaX = mouseLogical.x - m_itemDragStartMouse.x;
        const float deltaY = mouseLogical.y - m_itemDragStartMouse.y;
        const float dragThreshold =
            ImGui::GetIO().MouseDragThreshold / dpiScale;
        m_itemDragMoved = m_itemDragMoved || deltaX * deltaX + deltaY * deltaY >
                                                 dragThreshold * dragThreshold;
        if ( ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            if ( m_itemDragMoved ) {
                ProjectAudioToolLayout::Rect rawRect{
                    mouseLogical.x - m_dragOffset.x,
                    mouseLogical.y - m_dragOffset.y,
                    item.rect.width,
                    item.rect.height,
                };
                rawRect =
                    ProjectAudioToolLayout::snapRect(rawRect,
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
                refreshInteractionLabelRects();
            }
        } else {
            rebuildLabelRects();
            m_interactionBaseLabelRects.clear();
            if ( !m_itemDragMoved ) {
                previewEffectSelection(item);
            }
            persistWorkspace();
            m_draggingItem.reset();
            m_itemDragMoved = false;
            m_dragSnapTargets.clear();
            m_dragVisibilityConstraints.clear();
            m_snapLocks = {};
        }
    }

    if ( m_batchDragging && !m_batchDragEntries.empty() ) {
        if ( ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            ProjectAudioToolLayout::Rect rawBounds{
                mouseLogical.x - m_batchDragOffset.x,
                mouseLogical.y - m_batchDragOffset.y,
                m_batchDragInitialBounds.width,
                m_batchDragInitialBounds.height,
            };
            rawBounds = ProjectAudioToolLayout::snapRect(rawBounds,
                                                         visibleCanvas,
                                                         m_dragSnapTargets,
                                                         SNAP_THRESHOLD,
                                                         SNAP_RELEASE_THRESHOLD,
                                                         m_snapLocks);
            m_batchDragCurrentBounds =
                ProjectAudioToolLayout::constrainTranslatedVisibility(
                    m_batchDragCurrentBounds,
                    rawBounds,
                    m_batchDragInitialBounds,
                    m_batchDragUnionCells,
                    ProjectAudioToolLayout::Rect{
                        0.0F,
                        0.0F,
                        contentLogical.x,
                        contentLogical.y,
                    },
                    m_dragVisibilityConstraints,
                    MINIMUM_VISIBLE_RATIO);
            const float deltaX =
                m_batchDragCurrentBounds.x - m_batchDragInitialBounds.x;
            const float deltaY =
                m_batchDragCurrentBounds.y - m_batchDragInitialBounds.y;
            for ( const auto& entry : m_batchDragEntries ) {
                if ( entry.itemIndex >= m_items.size() ) continue;
                auto& item = m_items[entry.itemIndex];
                item.rect  = {
                    entry.startRect.x + deltaX,
                    entry.startRect.y + deltaY,
                    entry.startRect.width,
                    entry.startRect.height,
                };
                item.labelRect = item.rect;
            }
            refreshInteractionLabelRects();
        } else {
            rebuildLabelRects();
            m_interactionBaseLabelRects.clear();
            persistWorkspace();
            m_batchDragging = false;
            m_batchDragEntries.clear();
            m_batchDragUnionCells.clear();
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

            const float minimumWidth  = minimumItemWidth(item.type, dpiScale);
            const float minimumHeight = minimumItemHeight(dpiScale);
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
                                       bottom - minimumHeight);
                rawRect.height = bottom - rawRect.y;
            } else if ( verticalEdge == ResizeEdge::Maximum ) {
                rawRect.height =
                    std::max(minimumHeight,
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
                                                       minimumHeight,
                                                       SNAP_THRESHOLD,
                                                       SNAP_RELEASE_THRESHOLD,
                                                       m_snapLocks);
            item.rect = ProjectAudioToolLayout::constrainResizeVisibility(
                item.rect,
                rawRect,
                m_dragVisibilityConstraints,
                MINIMUM_VISIBLE_RATIO);
            item.labelRect = item.rect;
            refreshInteractionLabelRects();
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
            m_interactionBaseLabelRects.clear();
            persistWorkspace();
            m_resizingItem.reset();
            m_resizeHandle = ResizeHandle::None;
            m_dragSnapTargets.clear();
            m_dragVisibilityConstraints.clear();
            m_snapLocks = {};
        }
    }

    std::optional<ProjectAudioToolLayout::Rect> snapGuideBounds;
    if ( m_batchDragging ) {
        snapGuideBounds = m_batchDragCurrentBounds;
    } else {
        const auto activeItem =
            m_draggingItem ? m_draggingItem : m_resizingItem;
        if ( activeItem && *activeItem < m_items.size() ) {
            snapGuideBounds = m_items[*activeItem].rect;
        }
    }
    if ( snapGuideBounds ) {
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
                 *snapGuideBounds, *m_snapLocks.x.targetLine, true) ) {
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
                 *snapGuideBounds, *m_snapLocks.y.targetLine, false) ) {
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

    if ( m_marqueeSelecting ) {
        const auto selectionRect =
            toScreenRect(rectFromPoints(m_marqueeStart, m_marqueeEnd),
                         canvasOrigin,
                         dpiScale);
        const ImVec2 minimum{ selectionRect.x, selectionRect.y };
        const ImVec2 maximum{ selectionRect.right(), selectionRect.bottom() };
        drawList->AddRectFilled(minimum, maximum, IM_COL32(120, 170, 255, 42));
        drawList->AddRect(minimum,
                          maximum,
                          IM_COL32(120, 170, 255, 180),
                          0.0F,
                          0,
                          std::max(1.5F, 1.5F * dpiScale));
    }

    // 绝对定位控件全部提交完成后再声明画布内容边界。提前提交覆盖整张画布的
    // Dummy 会让后续回退到方块坐标的 ImGui Item 落入非流式布局状态，在
    // 新版 ImGui 中可能被裁成残片；末尾 Dummy 同时满足滚动范围和边界检查。
    ImGui::SetCursorScreenPos(canvasCursor);
    ImGui::Dummy({ contentLogical.x * dpiScale, contentLogical.y * dpiScale });
    ImGui::EndChild();
    renderRenamePopup(dpiScale);
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
    const std::size_t batchCount = batchSelectionCount();
    if ( batchCount > 0 ) {
        ImGui::SameLine();
        ImGui::TextDisabled(
            "| %s: %zu",
            TR("ui.project_audio_tool.status_batch_selected").data(),
            batchCount);
    }
}

}  // namespace MMM::UI
