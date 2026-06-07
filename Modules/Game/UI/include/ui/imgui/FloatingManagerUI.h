#pragma once

#include "event/core/EventBus.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/ISubView.h"
#include "ui/ITextureLoader.h"
#include <string>

struct ImGuiDockNode;

namespace MMM::UI
{
class ISubView;

class FloatingManagerUI : public ITextureLoader,
                          virtual public IUIView,
                          public IParallelUiPreparable
{
public:
    ///@brief 初始化时显示的的窗口id
    FloatingManagerUI(const std::string& name);
    FloatingManagerUI(FloatingManagerUI&&)                 = default;
    FloatingManagerUI(const FloatingManagerUI&)            = default;
    FloatingManagerUI& operator=(FloatingManagerUI&&)      = delete;
    FloatingManagerUI& operator=(const FloatingManagerUI&) = delete;
    ~FloatingManagerUI() override;

    ///@brief 注册子视图到这个管理器
    void registerSubView(const std::string&        subViewId,
                         std::unique_ptr<ISubView> subView);

    ///@brief 核心切换逻辑
    void toggleSubView(const std::string& subViewId);

    /// @brief 恢复浮动管理器显示的子视图状态。
    /// @param subViewId 需要显示的子视图 ID。
    /// @param visible 是否显示浮动管理器。
    void restoreSubViewState(const std::string& subViewId, bool visible);

    /// @brief 判断浮动管理器当前是否可见。
    /// @return 当前是否显示。
    bool isVisible() const;

    /// @brief 获取当前显示的子视图 ID。
    /// @return 当前子视图 ID；未显示任何子视图时可能为空。
    const std::string& getCurrentSubViewId() const;

    /// @brief 在主 DockSpace 渲染前应用停靠最小尺寸约束。
    /// @param sourceManager 当前 UIManager。
    /// @warning UI 热路径：每帧 DockSpace
    /// 绘制前执行；只允许读取当前子视图尺寸并修正 ImGui DockNode。
    void applyDockResizeConstraintsBeforeDockSpace(UIManager* sourceManager);

    /// @brief 主 DockSpace 渲染后恢复被临时钳住的鼠标位置。
    /// @warning UI 热路径：每帧 DockSpace 绘制后执行；只在曾钳住鼠标时写回。
    void restoreDockResizeMouseAfterDockSpace();

    /// @brief 更新当前浮动子视图，或维持自动收回后的透明拖拽热区。
    /// @param sourceManager 当前 UIManager。
    /// @warning UI 热路径：每帧执行；仅允许当前子视图绘制、DockNode
    /// 几何修正和轻量鼠标交互处理。
    void update(UIManager* sourceManager) override;

    void* getActualInstance() override { return this; }

    /// @brief 安全转换为 UI 并行准备接口。
    /// @return 当前浮动管理器的并行准备接口。
    IParallelUiPreparable* asParallelUiPreparable() override { return this; }

    /// @brief 判断当前可见子视图是否需要并行准备。
    /// @param snapshot 当前帧 UI 快照。
    /// @return 当前子视图需要准备时返回 true。
    /// @warning UI 热路径：每帧只查找当前子视图并检查脏位。
    bool needsParallelUiPrepare(const UiFrameSnapshot& snapshot) const override;

    /// @brief 在线程池中准备当前可见子视图数据。
    /// @param snapshot 当前帧 UI 快照。
    /// @warning 后台线程路径：仅委托子视图的纯数据准备逻辑。
    void prepareUiFrameData(const UiFrameSnapshot& snapshot) override;

    /// @brief 将当前子视图后台准备结果切换到主线程可读状态。
    void swapPreparedUiFrameData() override;

    /// @brief 是否需要重载
    bool needReload() override;

    /// @brief 重载纹理
    virtual void reloadTextures(vk::PhysicalDevice& physicalDevice,
                                vk::Device&         logicalDevice,
                                vk::CommandPool&    cmdPool,
                                vk::Queue&          queue) override;

private:
    /// @brief 隐藏当前子视图并同步取消侧边栏选中状态。
    /// @param sourceManager 当前 UIManager。
    /// @param keepCollapsedPlaceholder 是否保留自动收回后的透明拖拽热区。
    void hideCurrentSubView(UIManager* sourceManager,
                            bool       keepCollapsedPlaceholder = false);

    /// @brief 绘制自动收回后的透明拖拽热区，并在拖出阈值后恢复子视图。
    /// @param sourceManager 当前 UIManager。
    /// @return 本帧是否已经恢复子视图并需要立刻继续绘制真实 dock 窗口。
    /// @warning UI 热路径：侧边栏自动收回后每帧执行；只处理主 DockHost
    /// 边界、鼠标拖拽和阈值判断，禁止加入文件系统或复杂遍历。
    bool renderCollapsedResizeOverlay(UIManager* sourceManager);

    /// @brief 从自动收回透明拖拽热区恢复当前子视图并同步侧边栏选中状态。
    /// @param sourceManager 当前 UIManager。
    void showCurrentSubViewFromCollapsedOverlay(UIManager* sourceManager);

    /// @brief 记录自动收回前所在 DockNode 的分割轴和侧向。
    /// @param dockNode 当前子视图所在 DockNode。
    /// @warning UI 热路径：仅在触发自动收回时读取一次 DockNode 父子关系。
    void rememberCollapsedDockPlacement(ImGuiDockNode* dockNode);

    /// @brief 更新当前左键手势是否从 dock 分割线开始。
    /// @param dockNode 当前子视图所在 DockNode。
    /// @warning UI 热路径：每帧最多读取鼠标状态和 DockNode 几何，不做遍历。
    void updateDockResizeGesture(ImGuiDockNode* dockNode);

    ///@brief 是否需要重载
    bool m_needReload{ true };

    ///@brief 是否显示此浮窗
    bool m_isVisible = false;

    /// @brief 显示后是否已经见过满足当前子视图最小尺寸的窗口大小。
    bool m_hasSeenUsableSize{ false };

    /// @brief 下一次显示时是否请求恢复到当前内容最小尺寸。
    bool m_requestShowSizeReset{ false };

    /// @brief 上一帧窗口是否处于停靠状态，用于检测拖出成为浮窗的瞬间。
    bool m_wasDocked{ false };

    /// @brief 当前是否锁定在最小尺寸边界并等待继续拖拽触发收起。
    bool m_minResizeLockActive{ false };

    /// @brief 当前子视图是否自动收回为透明拖拽热区。
    bool m_isAutoCollapsed{ false };

    /// @brief 最小尺寸锁定的 dock split 轴，-1 表示无锁定。
    int m_minResizeLockAxis{ -1 };

    /// @brief 进入最小尺寸锁定时鼠标已经越过手柄的距离。
    float m_minResizeLockStartOverrun{ 0.0f };

    /// @brief 当前左键手势是否从 dock 分割线开始。
    bool m_dockResizeGestureActive{ false };

    /// @brief 当前 dock resize 手势所在轴，-1 表示无手势。
    int m_dockResizeGestureAxis{ -1 };

    /// @brief 自动收回透明热区当前是否正在拖拽展开。
    bool m_collapsedResizeDragActive{ false };

    /// @brief 自动收回透明热区拖拽所在轴，-1 表示无拖拽。
    int m_collapsedResizeDragAxis{ -1 };

    /// @brief 自动收回透明热区开始拖拽时鼠标在拖拽轴上的位置。
    float m_collapsedResizeDragStartMouseAxis{ 0.0f };

    /// @brief 自动收回拖拽恢复真实 dock 窗口后的首帧状态是否已经输出。
    bool m_collapsedResizeResumeStateLogged{ false };

    /// @brief 自动收回拖拽成功续接 DockNode 尺寸后是否已经输出。
    bool m_collapsedResizeResumeApplyLogged{ false };

    /// @brief 自动收回前所在 dock split 轴，默认按左侧栏处理。
    int m_collapsedDockAxis{ 0 };

    /// @brief 自动收回前所在 DockNode ID，用于同帧恢复时强制回到原停靠节点。
    ImGuiID m_collapsedDockId{ 0 };

    /// @brief 自动收回前是否位于 split 第一个子节点，决定从左/上还是右/下拉出。
    bool m_collapsedDockIsFirstChild{ true };

    /// @brief 本帧 DockSpace 前是否临时钳住过 ImGui 鼠标坐标。
    bool m_restoreMouseAfterDockSpace{ false };

    /// @brief DockSpace 前被钳住前的真实鼠标位置。
    ImVec2 m_mousePosBeforeDockClamp{ 0.0f, 0.0f };

    /// @brief DockSpace 前被钳住前的真实鼠标位移。
    ImVec2 m_mouseDeltaBeforeDockClamp{ 0.0f, 0.0f };

    ///@brief 当前子视图id
    std::string m_currentSubViewId;

    ///@brief 子视图表
    std::unordered_map<std::string, std::unique_ptr<ISubView>> m_subViews;

    ///@brief 订阅事件id
    MMM::Event::SubscriptionID m_subId;
};

}  // namespace MMM::UI
