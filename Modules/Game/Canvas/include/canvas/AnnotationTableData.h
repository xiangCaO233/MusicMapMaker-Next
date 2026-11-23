#pragma once

#include "logic/AnnotationTableSnapshot.h"
#include "ui/utils/CanvasTimeFormatContext.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MMM::Canvas
{

using Logic::AnnotationExportObjectType;
using Logic::AnnotationTableRow;

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
    /// @brief 请求下一次发布并消费活动谱面最近的完整批注快照。
    /// @return 当前可用状态以及批注行是否发生变化。
    /// @warning UI 低频路径：共享句柄保证跨逻辑线程的行数据寿命。
    /// 只在 Timing 变化时复制轻量锚点，不等待会话更新。
    AnnotationTableDataRefreshResult refresh();

    /// @brief 清空谱面绑定和所有独立数据缓存。
    void reset();

    /// @brief 获取当前批注表行。
    /// @return 按实际时间排序的批注行只读引用。
    [[nodiscard]] const std::vector<AnnotationTableRow>& rows() const;

    /// @brief 获取独立时间格式上下文。
    /// @return 当前谱面的 BPM 和分拍数据只读引用。
    [[nodiscard]] const UI::Utils::CanvasTimeFormatContext&
    timeFormatContext() const
    {
        return m_timeFormatContext;
    }

    /// @brief 行或拍位显示上下文改变时递增，用于平台派生文本的脏标记。
    /// @warning 每帧只读取 UI 自持标量，不读取会话或遍历批注。
    std::uint64_t displayRevision() const { return m_displayRevision; }
    /// @brief 稳定实例令牌用于关闭跨谱面的未提交编辑。
    /// @warning 每帧只读取 UI 缓存标量，无需读取逻辑状态。
    std::uintptr_t instanceId() const { return m_beatmapInstanceId; }
    /// @brief 低频刷新时发布的编辑许可；实际命令仍由会话执行时复核。
    /// @warning 每帧只读取已发布许可，不查询设备或等待逻辑锁。
    bool writable() const { return m_writable; }

private:
    /// @brief 实例重置与显示数据发布共同推进，不与后台缓存版本混用。
    std::uint64_t m_displayRevision{};
    bool          m_writable{};  ///< 播放或离线只读时保留阅读，禁止发起修改。
    /// @brief 当前绑定的谱面实例令牌。
    std::uintptr_t m_beatmapInstanceId{ 0U };

    /// @brief 最近一次选择的相机，用于区分等待中和同谱面刷新。
    std::string m_cameraId;

    /// @brief UI 整体保活不可变快照，绘制与导出不借用会话。
    std::shared_ptr<const Logic::AnnotationTableSnapshot> m_snapshot;

    /// @brief 与 Timeline 渲染快照无关的时间格式上下文。
    UI::Utils::CanvasTimeFormatContext m_timeFormatContext;
};

}  // namespace MMM::Canvas
