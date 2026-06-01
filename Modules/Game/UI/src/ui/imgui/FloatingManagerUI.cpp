#include "ui/imgui/FloatingManagerUI.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "ui/ISubView.h"
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

    const float shortage = minValue - currentSize;

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
    setAxisValue(
        sibling->SizeRef,
        axis,
        std::max(siblingMin, getAxisValue(sibling->SizeRef, axis) - shortage));

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
DockResizeOverrun getDockResizeOverrun(ImGuiDockNode* node,
                                       ImVec2 minWindowSize, ImVec2 mousePos)
{
    DockResizeOverrun result;
    if ( !node || !node->ParentNode ) {
        return result;
    }

    ImGuiDockNode*  parent = node->ParentNode;
    const ImGuiAxis axis   = parent->SplitAxis;
    if ( axis != ImGuiAxis_X && axis != ImGuiAxis_Y ) {
        return result;
    }

    const float minValue =
        axis == ImGuiAxis_X ? minWindowSize.x : minWindowSize.y;
    const float parentPos    = getAxisValue(parent->Pos, axis);
    const float parentSize   = getAxisValue(parent->Size, axis);
    const float mouseAxis    = getAxisValue(mousePos, axis);
    const bool  isFirstChild = parent->ChildNodes[0] == node;
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
                          if ( e.targetFloatManagerName == this->m_name ) {
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
        m_isVisible                  = false;  // 已激活再点击 -> 隐藏
        m_hasSeenUsableSize          = false;
        m_requestShowSizeReset       = false;
        m_wasDocked                  = false;
        m_minResizeLockActive        = false;
        m_minResizeLockAxis          = -1;
        m_restoreMouseAfterDockSpace = false;
    } else {
        m_currentSubViewId           = subViewId;
        m_isVisible                  = true;  // 切换或显示
        m_hasSeenUsableSize          = false;
        m_requestShowSizeReset       = true;
        m_minResizeLockActive        = false;
        m_minResizeLockAxis          = -1;
        m_restoreMouseAfterDockSpace = false;
    }
}

void FloatingManagerUI::restoreSubViewState(const std::string& subViewId,
                                            bool               visible)
{
    const bool willShow =
        visible && m_subViews.find(subViewId) != m_subViews.end();
    const bool changedVisibility = willShow != m_isVisible;
    const bool changedSubView    = subViewId != m_currentSubViewId;
    m_currentSubViewId           = subViewId;
    m_isVisible                  = willShow;
    if ( changedVisibility || changedSubView ) {
        m_hasSeenUsableSize          = false;
        m_requestShowSizeReset       = willShow;
        m_wasDocked                  = false;
        m_minResizeLockActive        = false;
        m_minResizeLockAxis          = -1;
        m_restoreMouseAfterDockSpace = false;
    }
}

bool FloatingManagerUI::isVisible() const
{
    return m_isVisible;
}

const std::string& FloatingManagerUI::getCurrentSubViewId() const
{
    return m_currentSubViewId;
}

/// @brief 在主 DockSpace 渲染前应用停靠最小尺寸约束。
void FloatingManagerUI::applyDockResizeConstraintsBeforeDockSpace(
    UIManager* sourceManager)
{
    if ( !m_isVisible ) return;

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
    ImVec2         currentWindowSize =
        clampDockNodeToMinSize(window, dockNode->Size, minWindowSize);
    const DockResizeOverrun dockResizeOverrun =
        getDockResizeOverrun(dockNode, minWindowSize, ImGui::GetMousePos());

    if ( !dockResizeOverrun.valid ||
         !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        m_minResizeLockActive = false;
        m_minResizeLockAxis   = -1;
        return;
    }

    const float axisSize =
        getAxisValue(currentWindowSize, dockResizeOverrun.axis);
    const float axisMinSize =
        getAxisValue(minWindowSize, dockResizeOverrun.axis);
    const bool atMinBoundary = axisSize <= axisMinSize + 0.5f;

    if ( atMinBoundary && dockResizeOverrun.overrun > 0.0f ) {
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
            hideCurrentSubView(sourceManager);
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

/// @brief 主 DockSpace 渲染后恢复被临时钳住的鼠标位置。
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
void FloatingManagerUI::hideCurrentSubView(UIManager* sourceManager)
{
    if ( !m_isVisible || m_currentSubViewId.empty() ) {
        m_isVisible = false;
        return;
    }

    MMM::Event::UISubViewToggleEvent evt;
    evt.sourceUiName           = m_name;
    evt.uiManager              = sourceManager;
    evt.targetFloatManagerName = m_name;
    evt.subViewId              = m_currentSubViewId;
    evt.showSubView            = false;
    MMM::Event::EventBus::instance().publish(evt);
    m_isVisible                  = false;
    m_hasSeenUsableSize          = false;
    m_requestShowSizeReset       = false;
    m_wasDocked                  = false;
    m_minResizeLockActive        = false;
    m_minResizeLockAxis          = -1;
    m_restoreMouseAfterDockSpace = false;
}

void FloatingManagerUI::update(UIManager* sourceManager)
{
    if ( !m_isVisible ) return;

    const float dpiScale =
        MMM::Config::AppConfig::instance().getWindowContentScale();
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        hideCurrentSubView(sourceManager);
        return;
    }

    const ImVec2 minWindowSize =
        toWindowMinSize(it->second->getMinContentSize(dpiScale), dpiScale);
    ImGui::SetNextWindowSizeConstraints(minWindowSize,
                                        ImVec2(FLT_MAX, FLT_MAX));
    if ( m_requestShowSizeReset || !m_hasSeenUsableSize ) {
        ImGui::SetNextWindowSize(minWindowSize, ImGuiCond_Always);
    }

    // 使用 ### 后缀强制固定 ImGui 内部窗口
    // ID，即使显示的标题变化也不会丢失停靠状态
    std::string    windowName = m_currentSubViewId + "###" + m_name;
    LayoutContext  lctx{ m_layoutCtx, windowName };
    ImVec2         currentWindowSize = ImGui::GetWindowSize();
    const bool     isDocked          = ImGui::IsWindowDocked();
    ImGuiWindow*   currentWindow     = ImGui::GetCurrentWindow();
    ImGuiDockNode* dockNode =
        isDocked && currentWindow ? currentWindow->DockNode : nullptr;
    const DockResizeOverrun dockResizeOverrun =
        getDockResizeOverrun(dockNode, minWindowSize, ImGui::GetMousePos());
    if ( m_wasDocked && !isDocked ) {
        currentWindowSize.x = std::max(currentWindowSize.x, minWindowSize.x);
        currentWindowSize.y = minWindowSize.y;
        ImGui::SetWindowSize(currentWindowSize, ImGuiCond_Always);
    }
    m_wasDocked            = isDocked;
    m_requestShowSizeReset = false;
    if ( isDocked ) {
        currentWindowSize = clampDockNodeToMinSize(
            currentWindow, currentWindowSize, minWindowSize);
    }

    const bool belowMin =
        isBelowMinWindowSize(currentWindowSize, minWindowSize);
    if ( !belowMin ) {
        m_hasSeenUsableSize = true;
    }

    if ( !isDocked || !dockResizeOverrun.valid ||
         !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        m_minResizeLockActive = false;
        m_minResizeLockAxis   = -1;
    } else {
        const float axisSize =
            getAxisValue(currentWindowSize, dockResizeOverrun.axis);
        const float axisMinSize =
            getAxisValue(minWindowSize, dockResizeOverrun.axis);
        const bool atMinBoundary = axisSize <= axisMinSize + 0.5f;

        if ( atMinBoundary && dockResizeOverrun.overrun > 0.0f ) {
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
                hideCurrentSubView(sourceManager);
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
