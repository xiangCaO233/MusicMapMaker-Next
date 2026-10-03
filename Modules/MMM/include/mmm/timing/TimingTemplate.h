#pragma once

#include "mmm/timing/Timing.h"
#include <cstddef>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <vector>

namespace MMM
{
/// @brief 模板条目保存相对基准的坐标，不绑定来源谱面的实体身份。
/// @details 偏移允许为负，以支持整排线前的高速 SV。
/// 普通点没有终点；插值段保留原函数，并额外保存当前模板轴上的终点。
struct TimingTemplatePoint {
    /// @brief 秒或拍的有符号偏移，由所属模板的坐标轴决定。
    double m_offset{ 0.0 };
    /// @brief 原始参数、元数据和可选段落；时间戳不参与模板定位。
    Timing m_timing;
    /// @brief 插值段尾相对基准的偏移，普通点忽略此字段。
    double m_endOffset{ 0.0 };
};

/// @brief 可跨谱面重复使用的时间点组。
/// @details 基准可以是组内任意点，其他点的偏移始终相对于它。
/// 存在 BPM 时只能按时间定位，避免模板自身改变换算所依赖的拍轴。
struct TimingTemplate {
    /// @brief 个人模板库中可编辑的显示名称。
    std::string m_name{ "新时间点模板" };
    /// @brief Time 按秒定位，Beat 按目标谱面的连续拍数定位。
    TimingVariable m_variable{ TimingVariable::Time };
    /// @brief 基准条目索引，不要求该点处于最早时间。
    std::size_t m_anchor{ 0 };
    /// @brief 保持用户选择顺序的值副本，最多保存 4096 项。
    std::vector<TimingTemplatePoint> m_points;
};

/// @brief 从选区构造模板，来源时间戳使用 Timing 的毫秒单位。
/// @param tempo 目标或来源谱面的完整 BPM 定义，支持连续 BPM 段。
/// @note 仅在创建或修改模板的低频路径排序红线，不用于每帧更新。
std::expected<TimingTemplate, std::string> makeTimingTemplate(
    std::span<const Timing> selected, std::size_t anchor,
    TimingVariable variable, std::span<const Timing> tempo,
    double fallbackBpm = 120.0);

/// @brief 将组内某点改为基准，平移所有偏移并保持组内距离。
/// @return 索引无效时返回 false，原模板保持不变。
bool reanchorTimingTemplate(TimingTemplate& value, std::size_t anchor);

/// @brief 将模板放置到指定秒数，返回毫秒时间戳的独立 Timing。
/// @details 负落点、损坏参数、内部交叠和非法段落使整个组失败。
/// 目标已有物件的冲突由逻辑提交入口进一步校验。
/// @note 低频操作；插值拍轴只在此处重新绑定，不在预览帧中构造。
std::expected<std::vector<Timing>, std::string> placeTimingTemplate(
    const TimingTemplate& value, double anchorSeconds,
    std::span<const Timing> tempo, double fallbackBpm = 120.0);

/// @brief 将稳定定义写为 JSON，不保存实体和运行时拍轴缓存。
void to_json(nlohmann::json& output, const TimingTemplate& value);
/// @brief 检查不可信模板数据，未知轴或版本不会退化成另一种定位方式。
std::expected<TimingTemplate, std::string> readTimingTemplate(
    const nlohmann::json& input);
/// @brief 读取单个模板或带 version/templates 的模板库文件内容。
/// @details 所有定义有效且名称不重复才返回完整候选组，最多 256 个。
/// @note 不写入个人库；文件导入需要界面显式确认后另行提交。
std::expected<std::vector<TimingTemplate>, std::string>
readTimingTemplateBundle(const nlohmann::json& input);
}  // namespace MMM
