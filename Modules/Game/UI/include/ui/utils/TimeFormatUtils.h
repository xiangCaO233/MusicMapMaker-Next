#pragma once

#include "ui/utils/CanvasTimeFormatContext.h"

#include <cstdint>
#include <string>

namespace MMM::Common::Render
{
/// @brief 格式化接口仅借用快照，不要求调用方包含渲染数据定义。
struct RenderSnapshot;
}  // namespace MMM::Common::Render
namespace MMM::Config
{
/// @brief 偏好枚举仅作为参数传递，取值定义由配置模块维护。
enum class TimeFormatPreference;
}  // namespace MMM::Config

namespace MMM::UI::Utils
{

/// @brief 画布时间对应的离散拍号与分拍位置。
struct CanvasBeatPosition {
    /// @brief 一基拍号；首个 BPM 节点之前保留零或负数偏移。
    int64_t beatNumber{ 0 };
    /// @brief 已约分的分拍分子；正常整拍使用一。
    int numerator{ 0 };
    /// @brief 已约分的分拍分母。
    int denominator{ 1 };
    /// @brief 是否存在可用于换算的 BPM 时间线。
    bool valid{ false };
};

namespace TimeFormatDetail
{

/// @brief 格式化时:分:秒.毫秒。
/// @param timeSeconds 可为负值的时间，单位秒。
/// @return 固定宽度 `HH:MM:SS.mmm` 文本，负值带前导负号。
/// @note 先对绝对毫秒总数舍入，再拆分字段，保证进位一致。
std::string formatClock(double timeSeconds);

/// @brief 格式化纯毫秒。
/// @param timeSeconds 时间或时长，单位秒。
/// @return 四舍五入到整数毫秒并附加单位的文本。
std::string formatMilliseconds(double timeSeconds);

/// @brief 从快照滚动分段中提取完整 BPM 时间线。
/// @param snapshot 当前渲染快照，可为空。
/// @return 按时间升序排列且时间点去重的 BPM 节点值数组。
/// @warning 非每帧批量调用路径：函数会分配、排序并去重节点数组。
std::vector<CanvasTimeFormatBpmPoint> collectBpmPoints(
    const Common::Render::RenderSnapshot* snapshot);

/// @brief 使用已经归一化的 BPM 节点计算拍号与分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param points 已按时间排序且 BPM 有效的计时节点。
/// @param beatDivisor 当前分拍数，非正值回退为四。
/// @return 拍号与约分后的分拍；缺少适用节点时返回无效结果。
CanvasBeatPosition calculateBeatPositionWithPoints(
    double timeSeconds, const std::vector<CanvasTimeFormatBpmPoint>& points,
    int beatDivisor);

/// @brief 使用已经归一化的 BPM 节点格式化拍号 + 分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param points 已按时间排序且 BPM 有效的计时节点。
/// @param beatDivisor 当前分拍数，非正值回退为四。
/// @return 拍号加约分分拍；缺少适用节点时返回秒格式。
std::string formatBeatTimeWithPoints(
    double timeSeconds, const std::vector<CanvasTimeFormatBpmPoint>& points,
    int beatDivisor);

/// @brief 使用渲染快照格式化拍号 + 分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param snapshot 当前渲染快照，可为空。
/// @return 基于快照 BPM 时间线和当前分拍数的文本。
/// @warning 会提取、排序并去重 BPM 节点，不应在同帧对大量标签重复调用。
std::string formatBeatTime(double                                timeSeconds,
                           const Common::Render::RenderSnapshot* snapshot);

/// @brief 使用独立时间上下文格式化拍号 + 分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param context 已缓存的 BPM 节点和分拍数。
/// @return 不依赖渲染快照的拍号分拍文本。
std::string formatBeatTime(double                         timeSeconds,
                           const CanvasTimeFormatContext& context);

/// @brief 按指定偏好格式化时间。
/// @param timeSeconds 待格式化时间或时长，单位秒。
/// @param preference 时钟、毫秒、拍号或秒格式偏好。
/// @param snapshot 拍号格式需要的渲染快照，其他格式忽略。
/// @return 与偏好对应的显示文本。
std::string formatTimeWithPreference(
    double timeSeconds, Config::TimeFormatPreference preference,
    const Common::Render::RenderSnapshot* snapshot);

}  // namespace TimeFormatDetail

/// @brief 从批注表等独立画布上下文计算拍号与分拍位置。
/// @param timeSeconds 待换算时间，单位秒。
/// @param context 已缓存且按时间排序的 BPM 节点与当前分拍数。
/// @return 可直接分列显示的拍号和约分分拍；无 BPM 时结果无效。
/// @warning 可在可见行绘制路径调用，不分配内存且不排序 BPM 节点。
CanvasBeatPosition calculateCanvasBeatPosition(
    double timeSeconds, const CanvasTimeFormatContext& context);

/// @brief 按编辑器偏好格式化画布时间戳。
/// @param timeSeconds 时间戳，单位秒
/// @param snapshot 当前渲染快照，用于拍号格式查询 BPM 时间线
/// @return 已格式化的时间文本
std::string formatCanvasTime(
    double                                timeSeconds,
    const Common::Render::RenderSnapshot* snapshot = nullptr);

/// @brief 按编辑器偏好和独立数据上下文格式化画布时间戳。
/// @param timeSeconds 时间戳，单位秒。
/// @param context 不依赖渲染快照的 BPM 与分拍上下文。
/// @return 已格式化的时间文本。
std::string formatCanvasTime(double                         timeSeconds,
                             const CanvasTimeFormatContext& context);

/// @brief 按编辑器偏好格式化两个画布时间戳。
/// @param firstSeconds 第一个时间戳，单位秒
/// @param secondSeconds 第二个时间戳，单位秒
/// @param snapshot 当前渲染快照，用于拍号格式查询 BPM 时间线
/// @return 已格式化的时间范围文本
std::string formatCanvasTimePair(
    double firstSeconds, double secondSeconds,
    const Common::Render::RenderSnapshot* snapshot = nullptr);

/// @brief 格式化画布时间长度。
/// @param durationSeconds 时间长度，单位秒
/// @return 已格式化的时间长度文本
std::string formatCanvasDuration(double durationSeconds);

}  // namespace MMM::UI::Utils
