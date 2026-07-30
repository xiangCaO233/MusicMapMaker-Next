#pragma once

#include "event/core/EventBus.h"
#include "mmm/project/AudioResource.h"
#include "ui/IUIView.h"
#include "ui/imgui/manager/ProjectAudioToolLayout.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace MMM::UI
{

/// @brief 以可自由排布方块展示项目 Main/Effect 资源的画笔选择工具。
class ProjectAudioToolView final : public IUIView
{
public:
    /// @brief 创建项目音频工具并订阅资源变更。
    explicit ProjectAudioToolView(const std::string& name);

    /// @brief 取消资源变更订阅。
    ~ProjectAudioToolView() override;

    ProjectAudioToolView(ProjectAudioToolView&&)                 = delete;
    ProjectAudioToolView(const ProjectAudioToolView&)            = delete;
    ProjectAudioToolView& operator=(ProjectAudioToolView&&)      = delete;
    ProjectAudioToolView& operator=(const ProjectAudioToolView&) = delete;

    /// @brief 绘制可滚动方块画布并处理选择、吸附及叠层拖动。
    /// @warning UI 热路径：每帧仅遍历缓存方块并执行可见性剔除；项目资源遍历、
    /// 排序和标签区域重建只允许在资源或布局脏时执行。
    void update(UIManager* sourceManager) override;

    /// @brief 请求下一帧把工具窗口聚焦到前台。
    void requestFocus();

private:
    /// @brief 方块缩放时被拖动的边或角。
    enum class ResizeHandle {
        None,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    /// @brief 工具画布中的单个项目音频方块。
    struct Item {
        /// @brief 项目音频资源稳定 ID。
        std::string audioResourceId;

        /// @brief 方块中显示的音频文件名。
        std::string label;

        /// @brief Main 或 Effect 资源类型。
        AudioTrackType type{ AudioTrackType::Effect };

        /// @brief 方块在逻辑画布中的布局矩形。
        ProjectAudioToolLayout::Rect rect;

        /// @brief 扣除上层遮挡后的文本绘制区域。
        ProjectAudioToolLayout::Rect labelRect;

        /// @brief 方块叠层顺序。
        std::int32_t zOrder{ 0 };

        /// @brief 宽度是否已经由用户手动调整。
        bool widthCustomized{ false };

        /// @brief 高度是否已经由用户手动调整。
        bool heightCustomized{ false };

        /// @brief 是否属于仅供批量布局移动使用的临时选区。
        bool batchSelected{ false };
    };

    /// @brief 批量拖动中一个方块的稳定下标和起始矩形。
    struct BatchDragEntry {
        /// @brief 方块提升到顶层后的缓存下标。
        std::size_t itemIndex{ 0 };

        /// @brief 批量拖动开始时的方块矩形。
        ProjectAudioToolLayout::Rect startRect;
    };

    /// @brief 从当前项目资源和工作区布局低频重建方块缓存。
    /// @param visibleWidth 当前可见画布的逻辑宽度。
    /// @param dpiScale 当前内容缩放。
    void rebuildItems(float visibleWidth, float dpiScale);

    /// @brief 重建叠层后各方块的可见文本区域。
    /// @warning 低频布局路径：会按叠层检查相交方块，只在打开、资源刷新或拖动
    /// 结束时调用。
    void rebuildLabelRects();

    /// @brief 把当前方块位置、叠层和选择写回项目工作区。
    void persistWorkspace();

    /// @brief 选择方块并将其提升到最上层。
    /// @param itemIndex 方块缓存下标。
    /// @param mousePosition 鼠标在逻辑画布中的坐标。
    void beginItemDrag(std::size_t itemIndex, ImVec2 mousePosition);

    /// @brief 选择方块、提升叠层并同步当前画笔音频。
    /// @return 提升后的方块下标；输入无效时返回空。
    [[nodiscard]] std::optional<std::size_t> activateItem(
        std::size_t itemIndex);

    /// @brief 从指定边或角开始缩放方块。
    /// @param itemIndex 方块缓存下标。
    /// @param handle 被拖动的边或角。
    /// @param mousePosition 鼠标在逻辑画布中的坐标。
    void beginItemResize(std::size_t itemIndex, ResizeHandle handle,
                         ImVec2 mousePosition);

    /// @brief 开始移动当前批量布局选区且不修改画笔音频。
    /// @param itemIndex 鼠标按下的批量选中方块。
    /// @param mousePosition 鼠标在逻辑画布中的坐标。
    void beginBatchDrag(std::size_t itemIndex, ImVec2 mousePosition);

    /// @brief 清空仅供批量布局移动使用的临时选区。
    void clearBatchSelection();

    /// @brief 统计当前批量布局选区中的方块数量。
    [[nodiscard]] std::size_t batchSelectionCount() const;

    /// @brief 根据鼠标位置检测方块边缘或四角的缩放热区。
    [[nodiscard]] ResizeHandle hitTestResizeHandle(const Item& item,
                                                   ImVec2 mousePosition) const;

    /// @brief 构建当前移动或缩放所需的吸附目标和下层可见性约束。
    void rebuildInteractionConstraints();

    /// @brief 构建批量移动所需的固定方块吸附目标与可见性约束。
    void rebuildBatchDragConstraints();

    /// @brief 绘制一个方块和位于其可见区域内的滚动文本。
    /// @param item 方块缓存。
    /// @param canvasOrigin 逻辑画布原点对应的屏幕坐标。
    /// @param dpiScale 当前内容缩放。
    /// @param drawList 当前 ImGui 绘制列表。
    void drawItem(const Item& item, ImVec2 canvasOrigin, float dpiScale,
                  bool hovered, bool pressed, ImDrawList& drawList) const;

    /// @brief 计算可容纳全部方块的画布尺寸。
    [[nodiscard]] ImVec2 calculateContentSize(float visibleWidth,
                                              float visibleHeight) const;

    /// @brief 按叠层从低到高排序的方块缓存。
    std::vector<Item> m_items;

    /// @brief 当前拖动方块可吸附的其它方块矩形。
    std::vector<ProjectAudioToolLayout::Rect> m_dragSnapTargets;

    /// @brief 当前拖动方块必须满足的下层可见性约束。
    std::vector<ProjectAudioToolLayout::VisibilityConstraint>
        m_dragVisibilityConstraints;

    /// @brief 当前拖动方块在 m_items 中的下标。
    std::optional<std::size_t> m_draggingItem;

    /// @brief 当前是否正在整体移动批量布局选区。
    bool m_batchDragging{ false };

    /// @brief 批量移动方块及其起始矩形缓存。
    std::vector<BatchDragEntry> m_batchDragEntries;

    /// @brief 批量选区开始移动时的组合外框。
    ProjectAudioToolLayout::Rect m_batchDragInitialBounds;

    /// @brief 批量选区上一帧通过约束后的组合外框。
    ProjectAudioToolLayout::Rect m_batchDragCurrentBounds;

    /// @brief 批量选区起始形态的无重叠并集单元。
    std::vector<ProjectAudioToolLayout::Rect> m_batchDragUnionCells;

    /// @brief 鼠标按下点相对批量选区组合外框左上角的偏移。
    ImVec2 m_batchDragOffset{ 0.0F, 0.0F };

    /// @brief 当前缩放方块在 m_items 中的下标。
    std::optional<std::size_t> m_resizingItem;

    /// @brief 当前缩放所操作的边或角。
    ResizeHandle m_resizeHandle{ ResizeHandle::None };

    /// @brief 开始缩放时的方块矩形，用于固定未拖动的对边。
    ProjectAudioToolLayout::Rect m_resizeStartRect;

    /// @brief 鼠标按下点相对活动边的逻辑偏移，避免开始缩放时跳变。
    ImVec2 m_resizePointerOffset{ 0.0F, 0.0F };

    /// @brief 鼠标按下点相对方块左上角的逻辑偏移。
    ImVec2 m_dragOffset{ 0.0F, 0.0F };

    /// @brief 当前移动或缩放的水平和垂直吸附滞回状态。
    ProjectAudioToolLayout::SnapLocks m_snapLocks;

    /// @brief 是否正在从空白画布拖出批量布局选框。
    bool m_marqueeSelecting{ false };

    /// @brief 批量布局选框在逻辑画布中的起点。
    ImVec2 m_marqueeStart{ 0.0F, 0.0F };

    /// @brief 批量布局选框在逻辑画布中的当前终点。
    ImVec2 m_marqueeEnd{ 0.0F, 0.0F };

    /// @brief 追加框选开始前每个方块的批量选中状态。
    std::vector<std::uint8_t> m_marqueeBaseSelection;

    /// @brief 当前选中项目音频资源 ID。
    std::string m_selectedAudioResourceId;

    /// @brief 当前选中资源的显示文件名。
    std::string m_selectedAudioLabel;

    /// @brief 当前选中资源的 Main/Effect 类型。
    AudioTrackType m_selectedAudioTrackType{ AudioTrackType::Effect };

    /// @brief 方块缓存对应的项目根目录 UTF-8 键。
    std::string m_cachedProjectRoot;

    /// @brief 方块默认尺寸上次测量时的内容缩放。
    float m_cachedDpiScale{ 0.0F };

    /// @brief 是否在下一帧聚焦窗口。
    bool m_requestFocus{ false };

    /// @brief 跨线程项目保存或资源变更脏标志。
    /// @warning 逻辑线程事件写入、UI 每帧读取并清除；只传递缓存失效状态，
    /// 使用 acquire/release，避免回调直接访问 UI 容器。
    std::atomic<bool> m_itemsDirty{ true };

    /// @brief 项目保存事件订阅 ID。
    Event::SubscriptionID m_projectSavedSubId{ 0 };

    /// @brief 音频资源变更事件订阅 ID。
    Event::SubscriptionID m_audioMutationSubId{ 0 };
};

}  // namespace MMM::UI
