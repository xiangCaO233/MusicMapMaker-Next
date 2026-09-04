#pragma once

#include "mmm/annotation/BeatmapAnnotation.h"
#include "ui/utils/CanvasTimeFormatContext.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MMM::Canvas
{

/// @brief 批注表中一条已经解析到实际谱面位置的独立数据行。
struct AnnotationTableRow {
    /// @brief 批注实际展示时间，单位秒。
    double timestamp{ 0.0 };

    /// @brief 批注稳定标识。
    std::string id;

    /// @brief 批注目标类型。
    ::MMM::BeatmapAnnotationTargetKind targetKind{
        ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP
    };

    /// @brief 目标物件轨道；独立时间戳或目标丢失时为 -1。
    std::int32_t track{ -1 };

    /// @brief 目标物件是否已经不存在。
    bool targetMissing{ false };

    /// @brief 批注作者。
    std::string author;

    /// @brief Markdown 正文。
    std::string content;
};

/// @brief 批注表数据刷新后的可用状态。
enum class AnnotationTableDataStatus {
    Close,
    Pending,
    Ready,
};

/// @brief 一次批注表数据刷新的结果。
struct AnnotationTableDataRefreshResult {
    /// @brief 当前数据可用状态。
    AnnotationTableDataStatus status{ AnnotationTableDataStatus::Close };

    /// @brief 批注行是否在本次刷新中发生替换。
    bool rowsChanged{ false };
};

/// @brief 独立维护活动谱面的批注表数据和时间格式上下文。
class AnnotationTableData
{
public:
    /// @brief 从活动谱面会话的版本化缓存刷新数据。
    /// @return 当前可用状态以及批注行是否发生变化。
    /// @warning UI 低频路径：调用时短暂持有 Session 锁，只在缓存版本变化时
    /// 复制批注行或 BPM 分段，禁止改为每帧无条件执行。
    AnnotationTableDataRefreshResult refresh();

    /// @brief 清空谱面绑定和所有独立数据缓存。
    void reset();

    /// @brief 获取当前批注表行。
    /// @return 按实际时间排序的批注行只读引用。
    [[nodiscard]] const std::vector<AnnotationTableRow>& rows() const
    {
        return m_rows;
    }

    /// @brief 获取独立时间格式上下文。
    /// @return 当前谱面的 BPM 和分拍数据只读引用。
    [[nodiscard]] const UI::Utils::CanvasTimeFormatContext&
    timeFormatContext() const
    {
        return m_timeFormatContext;
    }

private:
    /// @brief 当前绑定的谱面实例地址令牌。
    std::uintptr_t m_beatmapInstanceId{ 0U };

    /// @brief 当前批注行对应的逻辑缓存版本。
    std::uint64_t m_annotationRevision{ static_cast<std::uint64_t>(-1) };

    /// @brief 当前时间格式上下文对应的滚动缓存版本。
    std::uint64_t m_timingRevision{ static_cast<std::uint64_t>(-1) };

    /// @brief 当前缓存的分拍数，用于识别不涉及 Timing 的格式配置变化。
    int m_beatDivisor{ 4 };

    /// @brief 按实际时间排序的全量批注行缓存。
    std::vector<AnnotationTableRow> m_rows;

    /// @brief 与 Timeline 渲染快照无关的时间格式上下文。
    UI::Utils::CanvasTimeFormatContext m_timeFormatContext;
};

}  // namespace MMM::Canvas
