#include "ui/imgui/FloatingManagerUI.h"
#include "canvas/Basic2DCanvas.h"
#include "canvas/TimelineCanvas.h"
#include "config/AppConfig.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/ISubView.h"
#include "ui/UIManager.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace MMM::UI
{
namespace
{
struct DockResizeOverrun {
    bool      valid{ false };
    ImGuiAxis axis{ ImGuiAxis_None };
    float     boundary{ 0.0f };
    float     overrun{ 0.0f };
};

/// @brief 一次 dock resize 手势实际命中的 split 节点和当前子树。
struct DockResizeTarget {
    /// @brief 是否成功解析到 split 和子节点。
    bool valid{ false };

    /// @brief 当前浮动窗口所在的 split 子树节点。
    ImGuiDockNode* node{ nullptr };

    /// @brief 被拖拽的父 split 节点。
    ImGuiDockNode* parent{ nullptr };

    /// @brief 被拖拽 split 的轴向。
    ImGuiAxis axis{ ImGuiAxis_None };

    /// @brief 当前子树是否为父 split 的第一个子节点。
    bool isFirstChild{ true };
};

/// @brief 判断窗口尺寸是否已经被压到当前内容最小尺寸以下。
/// @warning UI 热路径：每帧仅做尺寸比较。
bool isBelowMinWindowSize(ImVec2 size, ImVec2 minWindowSize)
{
    return size.x < minWindowSize.x - 0.5f || size.y < minWindowSize.y - 0.5f;
}

/// @brief 判断是否已经越过本次低于最小尺寸起点后的收起缓冲距离。
/// @warning UI 热路径：每帧仅做阈值计算。
bool shouldCollapseAfterMinDragStart(float currentShortage, float startShortage,
                                     ImVec2 minWindowSize, ImGuiAxis axis)
{
    const float minAxisSize =
        axis == ImGuiAxis_X ? minWindowSize.x : minWindowSize.y;
    const float collapseDistance = std::floor(minAxisSize * 0.5f);
    return currentShortage - startShortage > collapseDistance;
}

/// @brief 判断原生 dock 分隔线是否已经拉出超过当前内容最小尺寸的一半。
/// @warning UI 热路径：侧边栏收回后拖拽时每帧仅做阈值计算。
bool shouldExpandAfterCollapsedDragStart(float currentSize, float startSize,
                                         ImVec2 minWindowSize, ImGuiAxis axis)
{
    const float minAxisSize =
        axis == ImGuiAxis_X ? minWindowSize.x : minWindowSize.y;
    const float expandDistance = std::floor(minAxisSize * 0.5f);
    return currentSize - startSize > expandDistance;
}

/// @brief 读取 ImVec2 在指定轴上的值。
float getAxisValue(ImVec2 value, ImGuiAxis axis)
{
    return axis == ImGuiAxis_X ? value.x : value.y;
}

/// @brief 写入 ImVec2 在指定轴上的值。
void setAxisValue(ImVec2& value, ImGuiAxis axis, float axisValue)
{
    if ( axis == ImGuiAxis_X ) {
        value.x = axisValue;
    } else {
        value.y = axisValue;
    }
}

/// @brief 根据 split 父子节点构造 resize 目标。
/// @warning UI 热路径：只校验 DockNode 关系和 split 轴向。
DockResizeTarget makeDockResizeTarget(ImGuiDockNode* node,
                                      ImGuiDockNode* parent)
{
    DockResizeTarget target;
    if ( !node || !parent ) {
        return target;
    }

    const bool isFirstChild  = parent->ChildNodes[0] == node;
    const bool isSecondChild = parent->ChildNodes[1] == node;
    if ( !isFirstChild && !isSecondChild ) {
        return target;
    }

    const ImGuiAxis axis = parent->SplitAxis;
    if ( axis != ImGuiAxis_X && axis != ImGuiAxis_Y ) {
        return target;
    }

    target.valid        = true;
    target.node         = node;
    target.parent       = parent;
    target.axis         = axis;
    target.isFirstChild = isFirstChild;
    return target;
}

/// @brief 设置指定 dock 节点轴向尺寸，并同步补偿同一父 split 的兄弟节点。
/// @warning UI 热路径：仅在自动收回拖拽续接期间更新当前 DockNode 几何。
float resizeDockNodeAxis(ImGuiDockNode* node, ImGuiAxis axis, float targetSize,
                         float nodeMinSize)
{
    if ( !node ) {
        return targetSize;
    }

    ImGuiDockNode* parent = node->ParentNode;
    if ( !parent || parent->SplitAxis != axis ) {
        const float clampedSize = std::max(nodeMinSize, targetSize);
        setAxisValue(node->Size, axis, clampedSize);
        setAxisValue(node->SizeRef, axis, clampedSize);
        return clampedSize;
    }

    ImGuiDockNode* sibling = parent->ChildNodes[0] == node
                                 ? parent->ChildNodes[1]
                                 : parent->ChildNodes[0];
    if ( !sibling ) {
        return targetSize;
    }

    const float siblingMin = 1.0f;
    const float parentPos  = getAxisValue(parent->Pos, axis);
    const float parentSize =
        std::max(nodeMinSize + siblingMin, getAxisValue(parent->Size, axis));
    const float clampedSize =
        std::clamp(targetSize, nodeMinSize, parentSize - siblingMin);
    const float siblingSize  = std::max(siblingMin, parentSize - clampedSize);
    const bool  isFirstChild = parent->ChildNodes[0] == node;

    setAxisValue(node->Size, axis, clampedSize);
    setAxisValue(node->SizeRef, axis, clampedSize);
    setAxisValue(sibling->Size, axis, siblingSize);
    setAxisValue(sibling->SizeRef, axis, siblingSize);

    if ( isFirstChild ) {
        setAxisValue(node->Pos, axis, parentPos);
        setAxisValue(sibling->Pos, axis, parentPos + clampedSize);
    } else {
        setAxisValue(sibling->Pos, axis, parentPos);
        setAxisValue(node->Pos, axis, parentPos + siblingSize);
    }

    if ( node->HostWindow ) {
        ImGui::MarkIniSettingsDirty(node->HostWindow);
    }

    return clampedSize;
}

/// @brief 将 resize 手势命中的 split 子树锁定到当前内容最小尺寸。
/// @warning UI 热路径：仅在 dock 分割线越过最小尺寸时写回命中的 DockNode
/// 几何，阻止压缩继续向兄弟子树传播。
float lockDockResizeTargetToMinSize(const DockResizeTarget& target,
                                    ImVec2                  minWindowSize)
{
    if ( !target.valid || !target.node ) {
        return 0.0f;
    }

    const float minAxisSize = getAxisValue(minWindowSize, target.axis);
    return resizeDockNodeAxis(
        target.node, target.axis, minAxisSize, minAxisSize);
}

/// @brief 将正在续接的自动收回拖拽距离换算为当前侧边栏宽度。
/// @warning UI 热路径：仅在鼠标按下拖拽时做一维距离计算。
float getCollapsedResumeDragSize(float mouseAxis, bool isFirstChild,
                                 float startMouseAxis)
{
    const float dragDistance =
        isFirstChild ? mouseAxis - startMouseAxis : startMouseAxis - mouseAxis;
    return std::max(1.0f, dragDistance);
}

/// @brief 获取自动收回透明拖拽热区宽度。
/// @warning UI 热路径：侧边栏收回后每帧只读取 ImGui 样式和 DPI。
float getCollapsedOverlayHitSize(float dpiScale)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return std::ceil(std::max(
        8.0f * std::max(1.0f, dpiScale),
        style.DockingSeparatorSize + style.WindowBorderHoverPadding * 2.0f));
}

/// @brief 计算自动收回透明拖拽热区的屏幕矩形。
/// @warning UI 热路径：仅根据 DockHost 窗口几何和收回侧向计算一个矩形。
ImRect makeCollapsedOverlayRect(const ImGuiWindow* hostWindow, ImGuiAxis axis,
                                bool isFirstChild, float hitSize)
{
    if ( !hostWindow ) {
        return ImRect();
    }

    if ( axis == ImGuiAxis_Y ) {
        const float boundary = isFirstChild
                                   ? hostWindow->Pos.y
                                   : hostWindow->Pos.y + hostWindow->Size.y;
        return ImRect(ImVec2(hostWindow->Pos.x, boundary - hitSize * 0.5f),
                      ImVec2(hostWindow->Pos.x + hostWindow->Size.x,
                             boundary + hitSize * 0.5f));
    }

    const float boundary = isFirstChild
                               ? hostWindow->Pos.x
                               : hostWindow->Pos.x + hostWindow->Size.x;
    return ImRect(ImVec2(boundary - hitSize * 0.5f, hostWindow->Pos.y),
                  ImVec2(boundary + hitSize * 0.5f,
                         hostWindow->Pos.y + hostWindow->Size.y));
}

/// @brief 计算自动收回分隔线 hover/active 样式的可见矩形。
/// @warning UI 热路径：仅根据主 DockHost 几何和鼠标拖拽位置计算绘制矩形。
ImRect makeCollapsedSeparatorRect(const ImGuiWindow* hostWindow, ImGuiAxis axis,
                                  float separatorAxis, float thickness)
{
    if ( !hostWindow ) {
        return ImRect();
    }

    if ( axis == ImGuiAxis_Y ) {
        return ImRect(
            ImVec2(hostWindow->Pos.x, separatorAxis - thickness * 0.5f),
            ImVec2(hostWindow->Pos.x + hostWindow->Size.x,
                   separatorAxis + thickness * 0.5f));
    }

    return ImRect(ImVec2(separatorAxis - thickness * 0.5f, hostWindow->Pos.y),
                  ImVec2(separatorAxis + thickness * 0.5f,
                         hostWindow->Pos.y + hostWindow->Size.y));
}

/// @brief 夹住指定 dock 节点轴向尺寸，并补偿同一父 split 中的兄弟节点。
/// @warning UI 热路径：仅在 dock 尺寸越界时修改当前 DockNode 及兄弟 SizeRef。
void clampDockNodeAxis(ImGuiDockNode* node, ImGuiAxis axis, float minValue)
{
    if ( !node ) {
        return;
    }

    const float currentSize = getAxisValue(node->Size, axis);
    if ( currentSize >= minValue ) {
        return;
    }

    ImGuiDockNode* parent = node->ParentNode;
    if ( !parent || parent->SplitAxis != axis ) {
        setAxisValue(node->Size, axis, minValue);
        setAxisValue(node->SizeRef,
                     axis,
                     std::max(getAxisValue(node->SizeRef, axis), minValue));
        return;
    }

    ImGuiDockNode* sibling = parent->ChildNodes[0] == node
                                 ? parent->ChildNodes[1]
                                 : parent->ChildNodes[0];
    if ( !sibling ) {
        return;
    }

    const float siblingMin   = 1.0f;
    const float parentPos    = getAxisValue(parent->Pos, axis);
    const float parentSize   = getAxisValue(parent->Size, axis);
    const float siblingSize  = std::max(siblingMin, parentSize - minValue);
    const bool  isFirstChild = parent->ChildNodes[0] == node;

    setAxisValue(node->Size, axis, minValue);
    setAxisValue(node->SizeRef,
                 axis,
                 std::max(getAxisValue(node->SizeRef, axis), minValue));
    setAxisValue(sibling->Size, axis, siblingSize);
    setAxisValue(sibling->SizeRef, axis, siblingSize);

    if ( isFirstChild ) {
        setAxisValue(node->Pos, axis, parentPos);
        setAxisValue(sibling->Pos, axis, parentPos + minValue);
    } else {
        setAxisValue(sibling->Pos, axis, parentPos);
        setAxisValue(node->Pos, axis, parentPos + siblingSize);
    }
}

/// @brief 将子视图内容最小尺寸换算为 ImGui 窗口最小尺寸。
/// @warning UI 热路径：每帧只读取当前样式和常量配置。
ImVec2 toWindowMinSize(ImVec2 contentMinSize, float dpiScale)
{
    auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    float windowPadding =
        std::floor(aesthetics.windowPadding * std::max(1.0f, dpiScale));
    float titleHeight = ImGui::GetFrameHeightWithSpacing();
    return ImVec2(
        std::max(1.0f, contentMinSize.x + windowPadding * 2.0f),
        std::max(1.0f, contentMinSize.y + windowPadding * 2.0f + titleHeight));
}

/// @brief 将指定停靠节点夹到最小窗口尺寸，阻止 dock 分割线继续压缩。
/// @warning UI 热路径：仅在当前窗口已停靠且尺寸越界时更新 DockNode 尺寸缓存。
ImVec2 clampDockNodeToMinSize(ImGuiWindow* window, ImVec2 currentSize,
                              ImVec2 minWindowSize)
{
    if ( !isBelowMinWindowSize(currentSize, minWindowSize) ) {
        return currentSize;
    }

    if ( !window || !window->DockNode ) {
        return currentSize;
    }

    ImGuiDockNode* node        = window->DockNode;
    bool           changedSize = false;
    if ( node->Size.x < minWindowSize.x ) {
        clampDockNodeAxis(node, ImGuiAxis_X, minWindowSize.x);
        window->Size.x = std::max(window->Size.x, minWindowSize.x);
        currentSize.x  = minWindowSize.x;
        changedSize    = true;
    }
    if ( node->Size.y < minWindowSize.y ) {
        clampDockNodeAxis(node, ImGuiAxis_Y, minWindowSize.y);
        window->Size.y = std::max(window->Size.y, minWindowSize.y);
        currentSize.y  = minWindowSize.y;
        changedSize    = true;
    }

    if ( changedSize && node->HostWindow ) {
        ImGui::MarkIniSettingsDirty(node->HostWindow);
    }
    return currentSize;
}

/// @brief 计算鼠标越过当前 dock split 最小尺寸手柄的距离。
/// @warning UI 热路径：每帧只读取 DockNode 几何和鼠标位置。
DockResizeOverrun getDockResizeOverrun(const DockResizeTarget& target,
                                       ImVec2 minWindowSize, ImVec2 mousePos)
{
    DockResizeOverrun result;
    if ( !target.valid || !target.parent || !target.node ) {
        return result;
    }

    ImGuiDockNode*  parent = target.parent;
    const ImGuiAxis axis   = target.axis;
    if ( axis != ImGuiAxis_X && axis != ImGuiAxis_Y ) {
        return result;
    }

    const float minValue =
        axis == ImGuiAxis_X ? minWindowSize.x : minWindowSize.y;
    const float parentPos    = getAxisValue(parent->Pos, axis);
    const float parentSize   = getAxisValue(parent->Size, axis);
    const float mouseAxis    = getAxisValue(mousePos, axis);
    const bool  isFirstChild = target.isFirstChild;
    const float boundary =
        isFirstChild ? parentPos + minValue
                     : parentPos + std::max(1.0f, parentSize - minValue);
    const float overrun =
        isFirstChild ? boundary - mouseAxis : mouseAxis - boundary;

    result.valid    = true;
    result.axis     = axis;
    result.boundary = boundary;
    result.overrun  = std::max(0.0f, overrun);
    return result;
}

/// @brief 判断鼠标位置是否落在指定 split 的分割条交互范围内。
/// @warning UI 热路径：仅读取 DockNode 几何和 ImGui 样式。
bool isDockSplitResizeHandleHit(ImGuiDockNode* parent, ImVec2 mousePos)
{
    if ( !parent ) {
        return false;
    }

    ImGuiDockNode* child0 = parent->ChildNodes[0];
    ImGuiDockNode* child1 = parent->ChildNodes[1];
    if ( !child0 || !child1 ) {
        return false;
    }

    const ImGuiAxis axis = parent->SplitAxis;
    if ( axis != ImGuiAxis_X && axis != ImGuiAxis_Y ) {
        return false;
    }

    ImRect splitterRect;
    splitterRect.Min = child0->Pos;
    splitterRect.Max = child1->Pos;
    splitterRect.Min[axis] += child0->Size[axis];
    splitterRect.Max[axis ^ 1] += child1->Size[axis ^ 1];

    const ImGuiStyle& style = ImGui::GetStyle();
    const float       hoverExtend =
        std::max(style.WindowBorderHoverPadding, style.DockingSeparatorSize);
    splitterRect.Expand(axis == ImGuiAxis_Y ? ImVec2(0.0f, hoverExtend)
                                            : ImVec2(hoverExtend, 0.0f));
    return splitterRect.Contains(mousePos);
}

/// @brief 从当前窗口 DockNode 向上查找鼠标命中的 resize split。
/// @warning UI 热路径：点击帧最多沿当前 DockNode 父链向上扫描。
DockResizeTarget findDockResizeTargetAtPos(ImGuiDockNode* node, ImVec2 mousePos)
{
    ImGuiDockNode* child = node;
    for ( ImGuiDockNode* parent = node ? node->ParentNode : nullptr; parent;
          child = parent, parent = parent->ParentNode ) {
        DockResizeTarget target = makeDockResizeTarget(child, parent);
        if ( target.valid && isDockSplitResizeHandleHit(parent, mousePos) ) {
            return target;
        }
    }
    return DockResizeTarget{};
}

/// @brief 根据拖拽开始时保存的 ID 重新解析当前 resize split。
/// @warning UI 热路径：拖拽期间每帧最多沿当前 DockNode 父链向上扫描。
DockResizeTarget findDockResizeTargetByIds(ImGuiDockNode* node, ImGuiID splitId,
                                           ImGuiID childId)
{
    if ( !node || splitId == 0 || childId == 0 ) {
        return DockResizeTarget{};
    }

    ImGuiDockNode* child = node;
    for ( ImGuiDockNode* parent = node->ParentNode; parent;
          child = parent, parent = parent->ParentNode ) {
        if ( parent->ID == splitId && child->ID == childId ) {
            return makeDockResizeTarget(child, parent);
        }
    }
    return DockResizeTarget{};
}

/// @brief 查找指定 DockNode 所属的最近轴向 split 子树。
/// @warning UI 热路径：只在需要保护主画布尺寸时沿 DockNode 父链向上扫描。
DockResizeTarget findDockNodeAxisTarget(ImGuiDockNode* node, ImGuiAxis axis)
{
    if ( !node || (axis != ImGuiAxis_X && axis != ImGuiAxis_Y) ) {
        return DockResizeTarget{};
    }

    ImGuiDockNode* child = node;
    for ( ImGuiDockNode* parent = node->ParentNode; parent;
          child = parent, parent = parent->ParentNode ) {
        DockResizeTarget target = makeDockResizeTarget(child, parent);
        if ( target.valid && target.axis == axis ) {
            return target;
        }
    }

    return makeDockResizeTarget(node, node->ParentNode);
}
}  // namespace

FloatingManagerUI::FloatingManagerUI(const std::string& name)
    : IUIView(name), ITextureLoader(name)
{
    // 订阅切换事件
    m_subId = MMM::Event::EventBus::instance()
                  .subscribe<MMM::Event::UISubViewToggleEvent>(
                      [this](const MMM::Event::UISubViewToggleEvent& e) {
                          XINFO(
                              "FloatingManagerUI get event, targetName:{}, "
                              "targetSubViewId:{}",
                              e.targetFloatManagerName,
                              e.subViewId);
                          // 核心逻辑：只处理发给“我”的指令
                          if ( e.targetFloatManagerName == this->m_name &&
                               e.sourceUiName != this->m_name ) {
                              if ( e.showSubView ) {
                                  this->restoreSubViewState(e.subViewId, true);
                              } else {
                                  this->restoreSubViewState(e.subViewId, false);
                              }
                          }
                      });
}

FloatingManagerUI::~FloatingManagerUI()
{
    MMM::Event::EventBus::instance()
        .unsubscribe<MMM::Event::UISubViewToggleEvent>(m_subId);
}

///@brief 注册子视图到这个管理器
void FloatingManagerUI::registerSubView(const std::string&        subViewId,
                                        std::unique_ptr<ISubView> subView)
{
    m_subViews[subViewId] = std::move(subView);
    // 不在此处自动 toggle，避免初始化时状态错乱
}

///@brief 核心切换逻辑
void FloatingManagerUI::toggleSubView(const std::string& subViewId)
{
    if ( m_currentSubViewId == subViewId && m_isVisible ) {
        m_isVisible                        = false;  // 已激活再点击 -> 隐藏
        m_hasSeenUsableSize                = false;
        m_requestShowSizeReset             = false;
        m_wasDocked                        = false;
        m_minResizeLockActive              = false;
        m_isAutoCollapsed                  = false;
        m_minResizeLockAxis                = -1;
        m_dockResizeGestureActive          = false;
        m_dockResizeGestureAxis            = -1;
        m_dockResizeGestureSplitId         = 0;
        m_dockResizeGestureChildId         = 0;
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        m_collapsedDockId                  = 0;
        m_restoreMouseAfterDockSpace       = false;
        clearCanvasDockSizeProtection();
    } else {
        m_currentSubViewId                 = subViewId;
        m_isVisible                        = true;  // 切换或显示
        m_hasSeenUsableSize                = false;
        m_requestShowSizeReset             = true;
        m_minResizeLockActive              = false;
        m_isAutoCollapsed                  = false;
        m_minResizeLockAxis                = -1;
        m_dockResizeGestureActive          = false;
        m_dockResizeGestureAxis            = -1;
        m_dockResizeGestureSplitId         = 0;
        m_dockResizeGestureChildId         = 0;
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        m_collapsedDockId                  = 0;
        m_restoreMouseAfterDockSpace       = false;
        clearCanvasDockSizeProtection();
    }
}

void FloatingManagerUI::restoreSubViewState(const std::string& subViewId,
                                            bool               visible)
{
    const bool willShow =
        visible && m_subViews.find(subViewId) != m_subViews.end();
    const bool changedVisibility         = willShow != m_isVisible;
    const bool changedSubView            = subViewId != m_currentSubViewId;
    const bool clearCollapsedPlaceholder = !willShow && m_isAutoCollapsed;
    m_currentSubViewId                   = subViewId;
    m_isVisible                          = willShow;
    if ( changedVisibility || changedSubView || clearCollapsedPlaceholder ) {
        m_hasSeenUsableSize                = false;
        m_requestShowSizeReset             = willShow;
        m_wasDocked                        = false;
        m_minResizeLockActive              = false;
        m_isAutoCollapsed                  = false;
        m_minResizeLockAxis                = -1;
        m_dockResizeGestureActive          = false;
        m_dockResizeGestureAxis            = -1;
        m_dockResizeGestureSplitId         = 0;
        m_dockResizeGestureChildId         = 0;
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        m_collapsedDockId                  = 0;
        m_restoreMouseAfterDockSpace       = false;
        clearCanvasDockSizeProtection();
    }
}

void FloatingManagerUI::updateDockResizeGesture(ImGuiDockNode* dockNode)
{
    const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if ( !dockNode || !dockNode->ParentNode || !mouseDown ) {
        m_dockResizeGestureActive  = false;
        m_dockResizeGestureAxis    = -1;
        m_dockResizeGestureSplitId = 0;
        m_dockResizeGestureChildId = 0;
        m_minResizeLockActive      = false;
        m_minResizeLockAxis        = -1;
        if ( !mouseDown ) {
            m_collapsedResizeDragActive        = false;
            m_collapsedResizeDragAxis          = -1;
            m_collapsedResizeResumeStateLogged = false;
            m_collapsedResizeResumeApplyLogged = false;
            clearCanvasDockSizeProtection();
        }
        return;
    }

    if ( !ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        return;
    }

    const ImVec2           clickPos = ImGui::GetIO().MouseClickedPos[0];
    const DockResizeTarget target =
        findDockResizeTargetAtPos(dockNode, clickPos);
    m_dockResizeGestureActive = target.valid;
    m_dockResizeGestureAxis   = target.valid ? target.axis : -1;
    m_dockResizeGestureSplitId =
        target.valid && target.parent ? target.parent->ID : 0;
    m_dockResizeGestureChildId =
        target.valid && target.node ? target.node->ID : 0;
    if ( !m_dockResizeGestureActive ) {
        m_minResizeLockActive      = false;
        m_minResizeLockAxis        = -1;
        m_dockResizeGestureSplitId = 0;
        m_dockResizeGestureChildId = 0;
        clearCanvasDockSizeProtection();
    }
}

void FloatingManagerUI::captureCanvasDockSizeProtection(
    UIManager* sourceManager, int axis)
{
    if ( !sourceManager || !m_canvasDockSizeProtection.empty() ||
         (axis != ImGuiAxis_X && axis != ImGuiAxis_Y) ) {
        return;
    }

    const ImGuiAxis dockAxis = axis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    auto            captureDockNode = [&](ImGuiID dockId) {
        if ( dockId == 0 ) {
            return;
        }

        ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockId);
        if ( !dockNode ) {
            return;
        }

        DockResizeTarget target = findDockNodeAxisTarget(dockNode, dockAxis);
        ImGuiDockNode*   node   = target.valid ? target.node : dockNode;
        if ( !node ) {
            return;
        }

        const ImGuiID protectedDockId = node->ID;
        const bool    alreadyCaptured = std::any_of(
            m_canvasDockSizeProtection.begin(),
            m_canvasDockSizeProtection.end(),
            [protectedDockId](const CanvasDockSizeSnapshot& snapshot) {
                return snapshot.dockId == protectedDockId;
            });
        if ( alreadyCaptured ) {
            return;
        }

        const float axisSize = getAxisValue(node->Size, dockAxis);
        if ( axisSize <= 1.0f ) {
            return;
        }

        m_canvasDockSizeProtection.push_back(
            CanvasDockSizeSnapshot{ protectedDockId, axisSize });
    };

    auto&         engine = Logic::EditorEngine::instance();
    const int32_t size   = engine.getSessionCount();
    m_canvasDockSizeProtection.reserve(static_cast<size_t>(size) + 1U);

    for ( int32_t i = 0; i < size; ++i ) {
        const Logic::SessionEntry* entry = engine.getSessionEntry(i);
        if ( !entry || entry->cameraId.empty() ) {
            continue;
        }

        auto* canvas =
            sourceManager->getView<Canvas::Basic2DCanvas>(entry->cameraId);
        if ( !canvas ) {
            continue;
        }

        captureDockNode(canvas->getDockId());
    }

    auto* timeline =
        sourceManager->getView<Canvas::TimelineCanvas>("TimelineWindow");
    if ( timeline ) {
        captureDockNode(timeline->getDockId());
    }
}

void FloatingManagerUI::restoreCanvasDockSizeProtection(int axis)
{
    if ( m_canvasDockSizeProtection.empty() ||
         (axis != ImGuiAxis_X && axis != ImGuiAxis_Y) ) {
        return;
    }

    const ImGuiAxis dockAxis = axis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    for ( const CanvasDockSizeSnapshot& snapshot :
          m_canvasDockSizeProtection ) {
        if ( snapshot.dockId == 0 || snapshot.axisSize <= 1.0f ) {
            continue;
        }

        ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(snapshot.dockId);
        if ( !dockNode ) {
            continue;
        }

        (void)resizeDockNodeAxis(dockNode, dockAxis, snapshot.axisSize, 1.0f);
    }
}

void FloatingManagerUI::clearCanvasDockSizeProtection()
{
    m_canvasDockSizeProtection.clear();
}

/// @brief 记录自动收回前所在 DockNode 的分割轴和侧向。
void FloatingManagerUI::rememberCollapsedDockPlacement(ImGuiDockNode* dockNode)
{
    if ( !dockNode || !dockNode->ParentNode ) {
        m_collapsedDockAxis         = ImGuiAxis_X;
        m_collapsedDockId           = dockNode ? dockNode->ID : 0;
        m_collapsedDockIsFirstChild = true;
        return;
    }

    const ImGuiAxis  axis   = dockNode->ParentNode->SplitAxis;
    DockResizeTarget target = findDockResizeTargetByIds(
        dockNode, m_dockResizeGestureSplitId, m_dockResizeGestureChildId);
    if ( !target.valid ) {
        target = makeDockResizeTarget(dockNode, dockNode->ParentNode);
    }

    const ImGuiAxis rememberedAxis = target.valid ? target.axis : axis;
    m_collapsedDockAxis =
        rememberedAxis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    m_collapsedDockId = dockNode->ID;
    m_collapsedDockIsFirstChild =
        target.valid ? target.isFirstChild
                     : dockNode->ParentNode->ChildNodes[0] == dockNode;
}

bool FloatingManagerUI::isVisible() const
{
    return m_isVisible;
}

const std::string& FloatingManagerUI::getCurrentSubViewId() const
{
    return m_currentSubViewId;
}

/// @brief 判断当前可见子视图是否需要并行准备。
/// @param snapshot 当前帧 UI 快照。
/// @return 当前子视图需要准备时返回 true。
bool FloatingManagerUI::needsParallelUiPrepare(
    const UiFrameSnapshot& snapshot) const
{
    if ( !m_isVisible ) {
        return false;
    }

    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        return false;
    }

    IParallelUiPreparable* preparable = it->second->asParallelUiPreparable();
    return preparable && preparable->needsParallelUiPrepare(snapshot);
}

/// @brief 在线程池中准备当前可见子视图数据。
/// @param snapshot 当前帧 UI 快照。
void FloatingManagerUI::prepareUiFrameData(const UiFrameSnapshot& snapshot)
{
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        return;
    }

    if ( IParallelUiPreparable* preparable =
             it->second->asParallelUiPreparable() ) {
        preparable->prepareUiFrameData(snapshot);
    }
}

/// @brief 将当前子视图后台准备结果切换到主线程可读状态。
void FloatingManagerUI::swapPreparedUiFrameData()
{
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        return;
    }

    if ( IParallelUiPreparable* preparable =
             it->second->asParallelUiPreparable() ) {
        preparable->swapPreparedUiFrameData();
    }
}

/// @brief 在主 DockSpace 渲染前应用停靠最小尺寸约束。
void FloatingManagerUI::applyDockResizeConstraintsBeforeDockSpace(
    UIManager* sourceManager)
{
    if ( !m_isVisible ) return;
    if ( m_collapsedResizeDragActive &&
         ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        return;
    }

    const float dpiScale =
        MMM::Config::AppConfig::instance().getWindowContentScale();
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        hideCurrentSubView(sourceManager);
        return;
    }

    const ImVec2 minWindowSize =
        toWindowMinSize(it->second->getMinContentSize(dpiScale), dpiScale);
    const std::string windowName = m_currentSubViewId + "###" + m_name;
    ImGuiWindow*      window     = ImGui::FindWindowByName(windowName.c_str());
    if ( !window || !window->DockNode ) {
        return;
    }

    ImGuiDockNode* dockNode = window->DockNode;
    updateDockResizeGesture(dockNode);
    ImVec2 currentWindowSize =
        clampDockNodeToMinSize(window, dockNode->Size, minWindowSize);
    DockResizeTarget resizeTarget = findDockResizeTargetByIds(
        dockNode, m_dockResizeGestureSplitId, m_dockResizeGestureChildId);
    if ( !resizeTarget.valid && m_dockResizeGestureActive ) {
        DockResizeTarget directTarget =
            makeDockResizeTarget(dockNode, dockNode->ParentNode);
        if ( directTarget.valid &&
             m_dockResizeGestureAxis == static_cast<int>(directTarget.axis) ) {
            resizeTarget = directTarget;
        }
    }
    const ImVec2            dockResizeMousePos = m_restoreMouseAfterDockSpace
                                                     ? m_mousePosBeforeDockClamp
                                                     : ImGui::GetMousePos();
    const DockResizeOverrun dockResizeOverrun =
        getDockResizeOverrun(resizeTarget, minWindowSize, dockResizeMousePos);

    if ( !dockResizeOverrun.valid || !m_dockResizeGestureActive ||
         m_dockResizeGestureAxis != static_cast<int>(dockResizeOverrun.axis) ||
         !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        m_minResizeLockActive = false;
        m_minResizeLockAxis   = -1;
        if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            clearCanvasDockSizeProtection();
        }
        return;
    }

    captureCanvasDockSizeProtection(sourceManager, dockResizeOverrun.axis);

    const float axisSize =
        getAxisValue(currentWindowSize, dockResizeOverrun.axis);
    const float axisMinSize =
        getAxisValue(minWindowSize, dockResizeOverrun.axis);
    const bool atMinBoundary = axisSize <= axisMinSize + 0.5f;

    if ( atMinBoundary && dockResizeOverrun.overrun > 0.0f ) {
        const float lockedAxisSize =
            lockDockResizeTargetToMinSize(resizeTarget, minWindowSize);
        if ( resizeTarget.node == dockNode && lockedAxisSize > 0.0f ) {
            setAxisValue(
                currentWindowSize, dockResizeOverrun.axis, lockedAxisSize);
        }
        restoreCanvasDockSizeProtection(dockResizeOverrun.axis);

        if ( !m_minResizeLockActive ||
             m_minResizeLockAxis != static_cast<int>(dockResizeOverrun.axis) ) {
            m_minResizeLockActive       = true;
            m_minResizeLockAxis         = dockResizeOverrun.axis;
            m_minResizeLockStartOverrun = dockResizeOverrun.overrun;
        }

        if ( shouldCollapseAfterMinDragStart(dockResizeOverrun.overrun,
                                             m_minResizeLockStartOverrun,
                                             minWindowSize,
                                             dockResizeOverrun.axis) ) {
            rememberCollapsedDockPlacement(dockNode);
            hideCurrentSubView(sourceManager, true);
            restoreDockResizeMouseAfterDockSpace();
            return;
        }

        ImGuiIO& io = ImGui::GetIO();
        if ( !m_restoreMouseAfterDockSpace ) {
            m_mousePosBeforeDockClamp    = io.MousePos;
            m_mouseDeltaBeforeDockClamp  = io.MouseDelta;
            m_restoreMouseAfterDockSpace = true;
        }
        setAxisValue(
            io.MousePos, dockResizeOverrun.axis, dockResizeOverrun.boundary);
        setAxisValue(io.MouseDelta, dockResizeOverrun.axis, 0.0f);
    } else if ( m_minResizeLockActive && dockResizeOverrun.overrun <= 0.0f ) {
        m_minResizeLockActive = false;
        m_minResizeLockAxis   = -1;
    }
}

/// @brief 恢复被临时钳住的鼠标位置。
void FloatingManagerUI::restoreDockResizeMouseAfterDockSpace()
{
    if ( !m_restoreMouseAfterDockSpace ) {
        return;
    }

    ImGui::GetIO().MousePos      = m_mousePosBeforeDockClamp;
    ImGui::GetIO().MouseDelta    = m_mouseDeltaBeforeDockClamp;
    m_restoreMouseAfterDockSpace = false;
}

/// @brief 隐藏当前子视图并同步取消侧边栏选中状态。
void FloatingManagerUI::hideCurrentSubView(UIManager* sourceManager,
                                           bool       keepCollapsedPlaceholder)
{
    if ( !m_isVisible || m_currentSubViewId.empty() ) {
        m_isVisible = false;
        m_isAutoCollapsed =
            keepCollapsedPlaceholder && !m_currentSubViewId.empty() &&
            m_subViews.find(m_currentSubViewId) != m_subViews.end();
        return;
    }

    MMM::Event::UISubViewToggleEvent evt;
    evt.sourceUiName           = m_name;
    evt.uiManager              = sourceManager;
    evt.targetFloatManagerName = m_name;
    evt.subViewId              = m_currentSubViewId;
    evt.showSubView            = false;
    MMM::Event::EventBus::instance().publish(evt);
    m_isVisible                        = false;
    m_hasSeenUsableSize                = false;
    m_requestShowSizeReset             = false;
    m_wasDocked                        = false;
    m_minResizeLockActive              = false;
    m_isAutoCollapsed                  = keepCollapsedPlaceholder;
    m_minResizeLockAxis                = -1;
    m_dockResizeGestureActive          = false;
    m_dockResizeGestureAxis            = -1;
    m_dockResizeGestureSplitId         = 0;
    m_dockResizeGestureChildId         = 0;
    m_collapsedResizeDragActive        = false;
    m_collapsedResizeDragAxis          = -1;
    m_collapsedResizeResumeStateLogged = false;
    m_collapsedResizeResumeApplyLogged = false;
    m_restoreMouseAfterDockSpace       = false;
    clearCanvasDockSizeProtection();
}

/// @brief 从自动收回透明拖拽热区恢复当前子视图并同步侧边栏选中状态。
void FloatingManagerUI::showCurrentSubViewFromCollapsedOverlay(
    UIManager* sourceManager)
{
    if ( m_currentSubViewId.empty() ||
         m_subViews.find(m_currentSubViewId) == m_subViews.end() ) {
        m_isAutoCollapsed = false;
        return;
    }

    MMM::Event::UISubViewToggleEvent evt;
    evt.sourceUiName           = m_name;
    evt.uiManager              = sourceManager;
    evt.targetFloatManagerName = m_name;
    evt.subViewId              = m_currentSubViewId;
    evt.showSubView            = true;
    MMM::Event::EventBus::instance().publish(evt);

    m_isVisible                        = true;
    m_hasSeenUsableSize                = true;
    m_requestShowSizeReset             = false;
    m_wasDocked                        = true;
    m_minResizeLockActive              = false;
    m_isAutoCollapsed                  = false;
    m_minResizeLockAxis                = -1;
    m_dockResizeGestureActive          = true;
    m_dockResizeGestureAxis            = m_collapsedResizeDragAxis;
    m_dockResizeGestureSplitId         = 0;
    m_dockResizeGestureChildId         = 0;
    m_collapsedResizeDragActive        = true;
    m_collapsedResizeResumeStateLogged = false;
    m_collapsedResizeResumeApplyLogged = false;
    m_restoreMouseAfterDockSpace       = false;
    clearCanvasDockSizeProtection();
}

/// @brief 绘制自动收回后的透明拖拽热区，并在拖出阈值后恢复子视图。
bool FloatingManagerUI::renderCollapsedResizeOverlay(UIManager* sourceManager)
{
    if ( !m_isAutoCollapsed || m_currentSubViewId.empty() ) {
        return false;
    }

    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        m_isAutoCollapsed = false;
        return false;
    }

    const float dpiScale =
        MMM::Config::AppConfig::instance().getWindowContentScale();
    const ImVec2 minWindowSize =
        toWindowMinSize(it->second->getMinContentSize(dpiScale), dpiScale);
    ImGuiWindow* hostWindow = ImGui::FindWindowByName("RightDockHost");
    if ( !hostWindow ) {
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        return false;
    }

    const ImGuiAxis axis =
        m_collapsedDockAxis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    const float  hitSize = getCollapsedOverlayHitSize(dpiScale);
    const ImRect hitRect = makeCollapsedOverlayRect(
        hostWindow, axis, m_collapsedDockIsFirstChild, hitSize);
    if ( hitRect.GetWidth() <= 0.0f || hitRect.GetHeight() <= 0.0f ) {
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        return false;
    }

    ImGui::SetNextWindowPos(hitRect.Min, ImGuiCond_Always);
    ImGui::SetNextWindowSize(hitRect.GetSize(), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    const std::string overlayName = "##CollapsedResizeOverlay_" + m_name;
    ImGuiWindowFlags  overlayFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin(overlayName.c_str(), nullptr, overlayFlags);
    ImGui::SetCursorScreenPos(hitRect.Min);
    ImGui::InvisibleButton("##CollapsedResizeHotZone", hitRect.GetSize());

    const bool overlayActive =
        ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool overlayHovered = ImGui::IsItemHovered();
    if ( overlayHovered || overlayActive ) {
        ImGui::SetMouseCursor(axis == ImGuiAxis_Y ? ImGuiMouseCursor_ResizeNS
                                                  : ImGuiMouseCursor_ResizeEW);
    }

    bool  shouldShowSubView = false;
    float mouseAxis         = 0.0f;
    float dragDistance      = 0.0f;
    float separatorAxis = axis == ImGuiAxis_Y
                              ? (m_collapsedDockIsFirstChild
                                     ? hostWindow->Pos.y
                                     : hostWindow->Pos.y + hostWindow->Size.y)
                              : (m_collapsedDockIsFirstChild
                                     ? hostWindow->Pos.x
                                     : hostWindow->Pos.x + hostWindow->Size.x);

    if ( !overlayActive ) {
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
    } else {
        mouseAxis = getAxisValue(ImGui::GetMousePos(), axis);
        if ( !m_collapsedResizeDragActive ||
             m_collapsedResizeDragAxis != static_cast<int>(axis) ) {
            m_collapsedResizeDragActive         = true;
            m_collapsedResizeDragAxis           = axis;
            m_collapsedResizeDragStartMouseAxis = mouseAxis;
        }

        dragDistance = m_collapsedDockIsFirstChild
                           ? mouseAxis - m_collapsedResizeDragStartMouseAxis
                           : m_collapsedResizeDragStartMouseAxis - mouseAxis;
        separatorAxis += (m_collapsedDockIsFirstChild ? 1.0f : -1.0f) *
                         std::max(0.0f, dragDistance);
        if ( shouldExpandAfterCollapsedDragStart(
                 std::max(0.0f, dragDistance), 0.0f, minWindowSize, axis) ) {
            shouldShowSubView = true;
        }
    }

    if ( overlayHovered || overlayActive ) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float       thickness =
            std::max(1.0f, std::floor(style.DockingSeparatorSize));
        const ImRect separatorRect = makeCollapsedSeparatorRect(
            hostWindow, axis, separatorAxis, thickness);
        const ImU32 separatorColor =
            ImGui::GetColorU32(overlayActive ? ImGuiCol_SeparatorActive
                                             : ImGuiCol_SeparatorHovered);
        ImGui::GetForegroundDrawList()->AddRectFilled(
            separatorRect.Min, separatorRect.Max, separatorColor);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);

    if ( shouldShowSubView ) {
        XINFO(
            "Sidebar collapsed resize threshold: manager={}, subView={}, "
            "axis={}, dockId={}, firstChild={}, dragDistance={}, "
            "minAxisSize={}, startMouseAxis={}, mouseAxis={}",
            m_name,
            m_currentSubViewId,
            static_cast<int>(axis),
            m_collapsedDockId,
            m_collapsedDockIsFirstChild,
            std::max(0.0f, dragDistance),
            getAxisValue(minWindowSize, axis),
            m_collapsedResizeDragStartMouseAxis,
            mouseAxis);
        showCurrentSubViewFromCollapsedOverlay(sourceManager);
        return true;
    }
    return false;
}

void FloatingManagerUI::update(UIManager* sourceManager)
{
    if ( !m_isVisible ) {
        if ( !renderCollapsedResizeOverlay(sourceManager) ) {
            return;
        }
    }

    const float dpiScale =
        MMM::Config::AppConfig::instance().getWindowContentScale();
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        hideCurrentSubView(sourceManager);
        return;
    }

    const ImVec2 minWindowSize =
        toWindowMinSize(it->second->getMinContentSize(dpiScale), dpiScale);
    const bool resumeCollapsedResize =
        m_collapsedResizeDragActive &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        (m_collapsedResizeDragAxis == ImGuiAxis_X ||
         m_collapsedResizeDragAxis == ImGuiAxis_Y);
    const ImGuiAxis resumeAxis =
        m_collapsedResizeDragAxis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    const float resumeMouseAxis =
        resumeCollapsedResize ? getAxisValue(ImGui::GetMousePos(), resumeAxis)
                              : 0.0f;
    const float requestedAxisSize =
        resumeCollapsedResize
            ? getCollapsedResumeDragSize(resumeMouseAxis,
                                         m_collapsedDockIsFirstChild,
                                         m_collapsedResizeDragStartMouseAxis)
            : 0.0f;
    const ImVec2 activeMinWindowSize =
        resumeCollapsedResize ? ImVec2(1.0f, 1.0f) : minWindowSize;
    ImGui::SetNextWindowSizeConstraints(activeMinWindowSize,
                                        ImVec2(FLT_MAX, FLT_MAX));
    if ( !resumeCollapsedResize &&
         (m_requestShowSizeReset || !m_hasSeenUsableSize) ) {
        ImGui::SetNextWindowSize(minWindowSize, ImGuiCond_Always);
    }

    // 使用 ### 后缀强制固定 ImGui 内部窗口
    // ID，即使显示的标题变化也不会丢失停靠状态
    std::string    windowName   = m_currentSubViewId + "###" + m_name;
    const ImGuiID  resumeDockId = resumeCollapsedResize ? m_collapsedDockId : 0;
    LayoutContext  lctx{ m_layoutCtx,     windowName,
                         false,           ImGuiWindowFlags_NoTitleBar,
                         nullptr,         resumeDockId,
                         ImGuiCond_Always };
    ImVec2         currentWindowSize = ImGui::GetWindowSize();
    const bool     isDocked          = ImGui::IsWindowDocked();
    ImGuiWindow*   currentWindow     = ImGui::GetCurrentWindow();
    ImGuiDockNode* dockNode =
        isDocked && currentWindow ? currentWindow->DockNode : nullptr;
    if ( resumeCollapsedResize ) {
        m_dockResizeGestureActive  = true;
        m_dockResizeGestureAxis    = resumeAxis;
        m_dockResizeGestureSplitId = 0;
        m_dockResizeGestureChildId = 0;
        m_minResizeLockActive      = false;
        m_minResizeLockAxis        = -1;
        if ( !m_collapsedResizeResumeStateLogged ) {
            XINFO(
                "Sidebar collapsed resize resume begin: manager={}, "
                "subView={}, axis={}, savedDockId={}, windowDockId={}, "
                "dockNodeId={}, isDocked={}, hasDockNode={}, hasParent={}, "
                "firstChild={}, requestedAxisSize={}, startMouseAxis={}, "
                "mouseAxis={}",
                m_name,
                m_currentSubViewId,
                static_cast<int>(resumeAxis),
                m_collapsedDockId,
                currentWindow ? currentWindow->DockId : 0,
                dockNode ? dockNode->ID : 0,
                isDocked,
                dockNode != nullptr,
                dockNode && dockNode->ParentNode,
                m_collapsedDockIsFirstChild,
                requestedAxisSize,
                m_collapsedResizeDragStartMouseAxis,
                resumeMouseAxis);
            m_collapsedResizeResumeStateLogged = true;
        }
    } else {
        updateDockResizeGesture(dockNode);
    }
    DockResizeTarget resizeTarget = findDockResizeTargetByIds(
        dockNode, m_dockResizeGestureSplitId, m_dockResizeGestureChildId);
    if ( !resizeTarget.valid && m_dockResizeGestureActive ) {
        DockResizeTarget directTarget =
            makeDockResizeTarget(dockNode, dockNode->ParentNode);
        if ( directTarget.valid &&
             m_dockResizeGestureAxis == static_cast<int>(directTarget.axis) ) {
            resizeTarget = directTarget;
        }
    }
    const ImVec2            dockResizeMousePos = m_restoreMouseAfterDockSpace
                                                     ? m_mousePosBeforeDockClamp
                                                     : ImGui::GetMousePos();
    const DockResizeOverrun dockResizeOverrun =
        getDockResizeOverrun(resizeTarget, minWindowSize, dockResizeMousePos);
    if ( resumeCollapsedResize && isDocked && dockNode ) {
        const float appliedAxisSize =
            resizeDockNodeAxis(dockNode, resumeAxis, requestedAxisSize, 1.0f);
        setAxisValue(currentWindow->Size, resumeAxis, appliedAxisSize);
        setAxisValue(currentWindow->SizeFull, resumeAxis, appliedAxisSize);
        currentWindowSize = dockNode->Size;
        if ( !m_collapsedResizeResumeApplyLogged ) {
            XINFO(
                "Sidebar collapsed resize resume applied: manager={}, "
                "subView={}, axis={}, dockNodeId={}, parentAxis={}, "
                "requestedAxisSize={}, appliedAxisSize={}, nodeAxisSize={}",
                m_name,
                m_currentSubViewId,
                static_cast<int>(resumeAxis),
                dockNode->ID,
                dockNode->ParentNode
                    ? static_cast<int>(dockNode->ParentNode->SplitAxis)
                    : -1,
                requestedAxisSize,
                appliedAxisSize,
                getAxisValue(dockNode->Size, resumeAxis));
            m_collapsedResizeResumeApplyLogged = true;
        }
        ImGui::SetMouseCursor(resumeAxis == ImGuiAxis_Y
                                  ? ImGuiMouseCursor_ResizeNS
                                  : ImGuiMouseCursor_ResizeEW);
        if ( ImGuiWindow* hostWindow = dockNode->HostWindow ) {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float       thickness =
                std::max(1.0f, std::floor(style.DockingSeparatorSize));
            const float separatorAxis =
                m_collapsedDockIsFirstChild
                    ? getAxisValue(dockNode->Pos, resumeAxis) + appliedAxisSize
                    : getAxisValue(dockNode->Pos, resumeAxis);
            const ImRect separatorRect = makeCollapsedSeparatorRect(
                hostWindow, resumeAxis, separatorAxis, thickness);
            ImGui::GetForegroundDrawList()->AddRectFilled(
                separatorRect.Min,
                separatorRect.Max,
                ImGui::GetColorU32(ImGuiCol_SeparatorActive));
        }
    }
    if ( !resumeCollapsedResize && m_wasDocked && !isDocked ) {
        currentWindowSize.x = std::max(currentWindowSize.x, minWindowSize.x);
        currentWindowSize.y = minWindowSize.y;
        ImGui::SetWindowSize(currentWindowSize, ImGuiCond_Always);
    }
    m_wasDocked = resumeCollapsedResize ? (m_wasDocked || isDocked) : isDocked;
    m_requestShowSizeReset = false;
    if ( isDocked && !resumeCollapsedResize ) {
        currentWindowSize = clampDockNodeToMinSize(
            currentWindow, currentWindowSize, minWindowSize);
    }

    const bool belowMin =
        isBelowMinWindowSize(currentWindowSize, minWindowSize);
    if ( !belowMin ) {
        m_hasSeenUsableSize = true;
    }

    if ( !isDocked || !dockResizeOverrun.valid || !m_dockResizeGestureActive ||
         m_dockResizeGestureAxis != static_cast<int>(dockResizeOverrun.axis) ||
         !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        m_minResizeLockActive = false;
        m_minResizeLockAxis   = -1;
        if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            clearCanvasDockSizeProtection();
        }
    } else {
        captureCanvasDockSizeProtection(sourceManager, dockResizeOverrun.axis);

        const float axisSize =
            getAxisValue(currentWindowSize, dockResizeOverrun.axis);
        const float axisMinSize =
            getAxisValue(minWindowSize, dockResizeOverrun.axis);
        const bool atMinBoundary = axisSize <= axisMinSize + 0.5f;

        if ( atMinBoundary && dockResizeOverrun.overrun > 0.0f ) {
            const float lockedAxisSize =
                lockDockResizeTargetToMinSize(resizeTarget, minWindowSize);
            if ( resizeTarget.node == dockNode && lockedAxisSize > 0.0f ) {
                setAxisValue(
                    currentWindowSize, dockResizeOverrun.axis, lockedAxisSize);
            }
            restoreCanvasDockSizeProtection(dockResizeOverrun.axis);

            if ( !m_minResizeLockActive ||
                 m_minResizeLockAxis !=
                     static_cast<int>(dockResizeOverrun.axis) ) {
                m_minResizeLockActive       = true;
                m_minResizeLockAxis         = dockResizeOverrun.axis;
                m_minResizeLockStartOverrun = dockResizeOverrun.overrun;
            }

            if ( shouldCollapseAfterMinDragStart(dockResizeOverrun.overrun,
                                                 m_minResizeLockStartOverrun,
                                                 minWindowSize,
                                                 dockResizeOverrun.axis) ) {
                rememberCollapsedDockPlacement(dockNode);
                hideCurrentSubView(sourceManager, true);
                return;
            }
        } else if ( m_minResizeLockActive &&
                    dockResizeOverrun.overrun <= 0.0f ) {
            m_minResizeLockActive = false;
            m_minResizeLockAxis   = -1;
        }
    }

    it->second->onUpdate(lctx, sourceManager);
}

/// @brief 是否需要重载
bool FloatingManagerUI::needReload()
{
    // 仅加载一次
    return std::exchange(m_needReload, false);
}

/// @brief 重载纹理 (当前无 ISubView 同时继承 ITextureLoader,预留接口)
void FloatingManagerUI::reloadTextures(vk::PhysicalDevice& /*physicalDevice*/,
                                       vk::Device& /*logicalDevice*/,
                                       vk::CommandPool& /*cmdPool*/,
                                       vk::Queue& /*queue*/)
{
}

}  // namespace MMM::UI
