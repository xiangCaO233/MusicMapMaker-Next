#pragma once

#include "canvas/TimelineAuxiliaryWindowState.h"
#include "canvas/TimelineCanvas.h"
#include "common/SelectionOperation.h"

#include <array>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace MMM::Canvas
{
/// @brief 管理时间点表格自己的窗口、搜索与选择事务。
/// @details 表格拖动锁存自己的锚点和基线，完成选择后更新画布共用选区。
/// 谱面身份变化时重置本表的旧手势，避免将拖动提交到新谱面。
struct TimelineCanvas::TimingTableController {
    /// @brief 绘制表格并消费输入，将用户命令提交到画布路由。
    /// @warning 打开时每帧调用，仅读取已发布的表格快照，不等待逻辑线程。
    void render(TimelineCanvas& canvas);

    /// @brief 仅属于 Timeline 时间点表格的窗口状态。
    TimelineAuxiliaryWindowState m_auxiliaryWindowState;

    /// @brief 下一次绘制时是否检查并恢复时间点表格窗口位置。
    bool m_shouldRecoverTableWindow{ false };
    /// @brief 下一次绘制时是否聚焦时间点表格窗口。
    bool m_shouldFocusTableWindow{ false };
    /// @brief 时间点表格上一帧是否同时聚焦且可从显示器工作区访问。
    bool m_isTableWindowFocusedAndReachable{ false };

    /// @brief 时间点批量编辑窗口绑定的谱面快照键。
    std::string m_tableBeatmapKey;
    /// @brief 下一帧表格是否需要滚动到当前判定线时间附近。
    bool m_tableScrollToCurrentTimePending{ false };
    /// @brief 表格待滚动定位的目标时间，单位秒。
    double m_tableScrollTargetTime{ 0.0 };
    /// @brief 时间点表格 Shift 连续选择使用的锚点实体。
    entt::entity m_tableSelectionAnchorEntity{ entt::null };
    /// @brief 是否正在通过时间点表格序号列拖动选择连续行。
    bool m_isTableRowDragSelecting{ false };
    /// @brief 时间点表格序号列拖选的起始实体。
    entt::entity m_tableRowDragAnchorEntity{ entt::null };
    /// @brief 时间点表格序号列拖选开始前保留的追加选择。
    std::unordered_set<entt::entity> m_tableRowDragBaseSelection;
    /// @brief 表格本次手势的集合模式；空值保持经典 Ctrl/Shift 规则。
    /// @details 起笔锁存，后续外部模式变化不能重解释当前拖选。
    std::optional<Logic::SelectionOperation> m_tableRowSelectionOperation;
    /// @brief 当前序号列拖选是否已经越过起始行。
    bool m_hasTableRowDragSelectionMoved{ false };
    /// @brief 时间点表格是否只显示 BPM 行。
    bool m_tableOnlyShowBpm{ false };
    /// @brief 时间点表格搜索启用的 BPM、SV、Jump 与 HS 属性。
    std::array<bool, 5> m_tableSearchEffectFilters{};
    /// @brief 时间点表格精确数值搜索文本。
    std::array<char, 64> m_tableSearchValueBuffer{};
    /// @brief 时间点表格搜索结果的批量替换值。
    double m_tableSearchReplacementValue{ 0.0 };

    /// @brief 最近一次新建 Timing 的目标时间（秒），用于表格高亮定位
    double m_lastCreatedTimingTime{ -1.0 };
    /// @brief 最近一次新建 Timing 的类型
    ::MMM::TimingEffect m_lastCreatedTimingEffect{ ::MMM::TimingEffect::BPM };
    /// @brief 最近一次新建 Timing 的高亮截止时间（ImGui 时间）
    double m_lastCreatedTimingHighlightUntil{ 0.0 };
};

/// @brief 在 UI 使用的独占几何上追加临时图元，并恢复借用的上一帧快照。
/// @details 快照交换之前必须执行 reset。该对象不拥有快照，记录仅在
/// 本次 UI 消费期间有效，同一缓冲区复用时不会累积临时顶点。
struct TimelineCanvas::TimelineSnapshotDecorator {
    /// @brief 恢复捕获时的颜色和容器长度。
    /// @warning 准备新帧前调用，只遍历曾被改写的颜色，不读 ECS。
    void reset();

    /// @brief 根据本帧交互对象装饰快照，为下一次恢复保存基线。
    /// @warning UI 热路径，不执行磁盘操作或等待 GPU。
    void refresh(TimelineCanvas& canvas, const ImVec2& size);

    /// @brief Timeline 顶点颜色恢复记录。
    struct TimelineVertexColorRestore {
        /// @brief 顶点索引。
        uint32_t vertexIndex{ 0 };

        /// @brief 修饰前完整颜色。
        Graphic::Vertex::Color color;
    };

    /// @brief 当前被 UI 侧交互修饰过的 Timeline 快照。
    Common::Render::RenderSnapshot* m_decoratedTimelineSnapshot{ nullptr };

    /// @brief 修饰前快照顶点数量。
    size_t m_decoratedTimelineVertexCount{ 0 };

    /// @brief 修饰前快照索引数量。
    size_t m_decoratedTimelineIndexCount{ 0 };

    /// @brief 修饰前普通绘制命令数量。
    size_t m_decoratedTimelineCmdCount{ 0 };

    /// @brief 修饰前发光绘制命令数量。
    size_t m_decoratedTimelineGlowCmdCount{ 0 };

    /// @brief 修饰前顶点颜色恢复列表。
    std::vector<TimelineVertexColorRestore> m_timelineColorRestore;

    /// @brief 每帧清空的发光标记索引，避免同一 marker 追加多条命令。
    std::unordered_set<std::uint64_t> m_glowMarkers;
    /// @brief 每帧清空的半透明标记索引，每个 marker 仅保存一次原始颜色。
    std::unordered_set<std::uint64_t> m_dimMarkers;
};
}  // namespace MMM::Canvas
