#pragma once

#include "event/core/EventBus.h"
#include "mmm/project/AudioResource.h"
#include "ui/IUIView.h"
#include "ui/imgui/manager/ProjectAudioToolLayout.h"

#include <atomic>
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
    };

    /// @brief 从当前项目资源和工作区布局低频重建方块缓存。
    /// @param visibleWidth 当前可见画布的逻辑宽度。
    void rebuildItems(float visibleWidth);

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

    /// @brief 构建当前拖动所需的吸附目标和下层可见性约束。
    void rebuildDragConstraints();

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

    /// @brief 鼠标按下点相对方块左上角的逻辑偏移。
    ImVec2 m_dragOffset{ 0.0F, 0.0F };

    /// @brief 当前拖动的水平和垂直吸附滞回状态。
    ProjectAudioToolLayout::SnapLocks m_snapLocks;

    /// @brief 当前选中项目音频资源 ID。
    std::string m_selectedAudioResourceId;

    /// @brief 当前选中资源的显示文件名。
    std::string m_selectedAudioLabel;

    /// @brief 当前选中资源的 Main/Effect 类型。
    AudioTrackType m_selectedAudioTrackType{ AudioTrackType::Effect };

    /// @brief 方块缓存对应的项目根目录 UTF-8 键。
    std::string m_cachedProjectRoot;

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
