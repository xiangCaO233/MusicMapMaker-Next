#pragma once

#include "event/core/EventBus.h"
#include "mmm/project/AudioResource.h"
#include "ui/IUIView.h"
#include "ui/imgui/manager/ProjectAudioToolLayout.h"

#include <array>
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

        /// @brief 该方块独立试听实例的 AudioManager 池标识。
        std::string previewPoolKey;

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

    /// @brief 一个已评分并可直接定位的实时搜索结果。
    struct SearchResult {
        /// @brief 项目音频资源稳定 ID。
        std::string audioResourceId;

        /// @brief 结果列表显示文本。
        std::string displayLabel;

        /// @brief 文件名或资源 ID 的最高相似度分数。
        int score{ 0 };
    };

    /// @brief 从当前项目资源和工作区布局低频重建方块缓存。
    /// @param visibleWidth 当前可见画布的逻辑宽度。
    /// @param dpiScale 当前内容缩放。
    void rebuildItems(float visibleWidth, float dpiScale);

    /// @brief 重建叠层后各方块的可见文本区域。
    /// @warning 低频布局路径：会按叠层检查相交方块，只在打开、资源刷新或拖动
    /// 结束时调用。
    void rebuildLabelRects();

    /// @brief 从已缓存的可见性约束建立固定标签区域基线。
    /// @warning 低频交互起点：只读取交互约束，只在开始拖动或缩放时调用。
    void prepareInteractionLabelRects();

    /// @brief 按当前移动对象位置增量刷新所有标签与控件区域。
    /// @warning 拖动热路径：只遍历缓存方块和活动对象，不分配、不排序。
    void refreshInteractionLabelRects();

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

    /// @brief 按项目选项试听一次新选中的 Effect 音频资源。
    /// @param item 本次完成选择的资源方块。
    /// @warning 低频用户操作路径：首次试听可能同步加载音频，只允许在明确的
    /// 选择动作完成后调用。
    void previewEffectSelection(const Item& item) const;

    /// @brief 清除当前项目音频选择，并同步为空资源画笔。
    void clearActiveItem();

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

    /// @brief 按当前输入词增量重建相似音频结果缓存。
    /// @warning 只在输入词、项目资源或 DPI 布局变化后调用；允许遍历和排序资源。
    void rebuildSearchResults();

    /// @brief 请求下一次画布更新时定位并选中指定搜索结果。
    void requestSearchResultFocus(const std::string& audioResourceId);

    /// @brief 打开指定音频方块的重命名弹窗并预填当前文件名。
    /// @param itemIndex 右键命中的方块下标。
    void requestItemRename(std::size_t itemIndex);

    /// @brief 绘制项目音频文件名与逻辑轨道 ID 同步重命名弹窗。
    /// @param dpiScale 当前内容缩放。
    void renderRenamePopup(float dpiScale);

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

    /// @brief 排除当前移动对象后，各方块可用于增量裁切的标签区域基线。
    std::vector<ProjectAudioToolLayout::Rect> m_interactionBaseLabelRects;

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

    /// @brief 单方块拖动手势开始时的鼠标逻辑坐标。
    ImVec2 m_itemDragStartMouse{ 0.0F, 0.0F };

    /// @brief 当前单方块拖动手势是否已经越过拖动阈值。
    bool m_itemDragMoved{ false };

    /// @brief 当前手势是否从已选方块开始，用于短按释放时反选。
    bool m_itemDragStartedSelected{ false };

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

    /// @brief 项目音频实时搜索输入。
    std::array<char, 256> m_searchBuffer{};

    /// @brief 按相似度从高到低排列的搜索结果缓存。
    std::vector<SearchResult> m_searchResults;

    /// @brief 当前键盘或鼠标预选的搜索结果下标。
    std::size_t m_searchHighlightedIndex{ 0 };

    /// @brief 搜索输入或资源列表变化后需要低频重新评分。
    bool m_searchResultsDirty{ true };

    /// @brief 等待画布创建后执行定位的音频资源 ID。
    std::string m_searchFocusRequestId;

    /// @brief 重命名弹窗当前目标资源 ID。
    std::string m_renameAudioResourceId;

    /// @brief 重命名弹窗文件名输入。
    std::array<char, 512> m_renameBuffer{};

    /// @brief 下一帧需要打开重命名弹窗。
    bool m_shouldOpenRenamePopup{ false };

    /// @brief 弹窗打开后需要把键盘焦点放入文件名输入框。
    bool m_shouldFocusRenameInput{ false };

    /// @brief 当前选中项目音频资源 ID。
    std::string m_selectedAudioResourceId;

    /// @brief 当前选中资源的显示文件名。
    std::string m_selectedAudioLabel;

    /// @brief 当前选中资源的 Main/Effect 类型。
    AudioTrackType m_selectedAudioTrackType{ AudioTrackType::Effect };

    /// @brief 当前选中资源创建新物件时写入的物件音量倍率。
    float m_brushAudioVolume{ 1.0F };

    /// @brief 当前保持音量编辑弹窗的资源 ID。
    std::string m_openVolumeEditorResourceId;

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
