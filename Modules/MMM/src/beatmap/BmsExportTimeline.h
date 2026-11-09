#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace MMM::Bms
{
/// @brief 为小节分数寻找满足误差预算的有界有理数。
/// @param fraction 小节内的非负位置，不包含右端点。
/// @param tolerance 允许的绝对小节分数误差，必须为有限正数。
/// @return 槽位与总槽数；无法在读取器支持的行长内表示时为空。
/// @note 连分数避免为每个密集事件扫描数万个候选分母。
/// @note 分母最多一百万，匹配 BMS 读取器两百万字符的行长边界。
inline std::optional<std::array<int, 2>> approximateExportFraction(
    double fraction, double tolerance)
{
    // 右端点必须由小节调用层进位，不能在这里生成槽位等于总槽数的行。
    // 非有限输入先拒绝，后面的浮点到整数转换才有明确范围。
    if ( !std::isfinite(fraction) || fraction < 0 || fraction >= 1 ||
         !std::isfinite(tolerance) || tolerance <= 0 )
        return std::nullopt;
    constexpr std::int64_t LIMIT = 1'000'000;
    // 保存相邻两个收敛项；分母递增时，区间内的分子也受同一上限约束。
    // 初始两项是连分数递推的种子，不表示实际输出中的有效事件。
    std::int64_t oldP = 0, oldQ = 1, p = 1, q = 0;
    double       remainder = fraction;
    for ( int iteration = 0; iteration < 64; ++iteration ) {
        // 有界次数覆盖 double 的有效精度；不能因舍入尾数进入无穷循环。
        // 原值 fraction 始终保留，只对余数递推，误差检查不使用累积近似。
        const double coefficient = std::floor(remainder);
        // 先约束系数再乘加，极接近整数的输入不能使整数收敛项溢出。
        // 达到分母边界时同时检查最后一个可用半收敛项。
        const auto bound   = q == 0 ? LIMIT : (LIMIT - oldQ) / q;
        const bool limited = coefficient > static_cast<double>(bound);
        const auto step =
            limited ? bound : static_cast<std::int64_t>(coefficient);
        const auto nextP = step * p + oldP;
        const auto nextQ = step * q + oldQ;
        // 零分母和右端点都不能进入 BMS 数据行；允许零槽表示小节开头。
        if ( nextQ > 0 && nextP >= 0 && nextP < nextQ &&
             std::abs(fraction - static_cast<double>(nextP) / nextQ) <=
                 tolerance )
            return std::array<int, 2>{ static_cast<int>(nextP),
                                       static_cast<int>(nextQ) };
        if ( limited ) break;
        oldP = p;
        oldQ = q;
        p    = nextP;
        q    = nextQ;
        // 精确有理数已到终点；继续取倒数只会产生无穷值。
        const double tail = remainder - coefficient;
        if ( tail == 0 ) break;
        remainder = 1 / tail;
        if ( !std::isfinite(remainder) ) break;
    }
    return std::nullopt;
}

/// @brief 根据实际写出拍位校正后续变速，避免局部编码误差累计成时轴漂移。
/// @note 所有状态只属于一次离线导出，不修改来源模型或运行中的播放时钟。
class ExportTimeline final
{
public:
    /// @brief 初始化零点时钟与小节分数误差预算。
    /// @param bpm 首条红线之前使用的初始正速度。
    /// @param tolerance 单次编码的小节分数误差，不是累计毫秒偏移。
    ExportTimeline(double bpm, double tolerance)
        : m_bpm(bpm), m_tolerance(tolerance)
    {
    }

    /// @brief 按来源时间顺序登记一个音乐时轴事件并返回实际编码拍位。
    /// @param timestamp 来源毫秒位置，同刻事件按原有顺序提交。
    /// @param bpm 红线的新速度；STOP 传空值且不推进音乐等待时间。
    /// @return 三位小节号及行长边界内的实际拍位，失败时为空。
    std::optional<double> append(double timestamp, std::optional<double> bpm)
    {
        if ( !std::isfinite(timestamp) || timestamp < m_sourceTime ||
             !std::isfinite(m_bpm) || m_bpm <= 0 ||
             (bpm && (!std::isfinite(*bpm) || *bpm <= 0)) )
            return std::nullopt;
        double beat = m_beat;
        if ( m_anchors.empty() || timestamp != m_sourceTime ) {
            // 从上一段实际解码时刻反算下一事件，抵消此前的表示误差。
            // 不能把原始理想拍位分别取整后继续累积其时长。
            const double wanted =
                m_beat + (timestamp - m_actualTime) * m_bpm / 60000;
            if ( !std::isfinite(wanted) || wanted < m_beat || wanted >= 4000 )
                return std::nullopt;
            int    measure  = static_cast<int>(wanted / 4);
            double fraction = wanted / 4 - measure;
            // 小节右边界规范到下一小节；同刻 BPM 与 STOP 必须共用此位置。
            if ( 1 - fraction <= m_tolerance ) {
                ++measure;
                fraction = 0;
            }
            const auto position =
                approximateExportFraction(fraction, m_tolerance);
            if ( measure >= 1000 || !position ) return std::nullopt;
            // 使用最终有理数构造实际拍位，后续校正不能仍引用理想 wanted。
            beat = measure * 4 + 4. * (*position)[0] / (*position)[1];
            if ( beat < m_beat ) return std::nullopt;
            // 当前区间结束前必须使用旧 BPM，红线的新值只控制后续区间。
            // STOP 不在这里增加等待时间，与 BMS 读取器的音乐坐标保持一致。
            m_actualTime += (beat - m_beat) * 60000 / m_bpm;
        }
        m_beat       = beat;
        m_sourceTime = timestamp;
        if ( bpm ) m_bpm = *bpm;
        // 同刻多条指令只改变后续区间的速度，不能为同一时刻制造新位移。
        if ( !m_anchors.empty() && m_anchors.back().m_sourceTime == timestamp )
            m_anchors.back() = { timestamp, beat, m_bpm };
        else
            m_anchors.push_back({ timestamp, beat, m_bpm });
        return beat;
    }

    /// @brief 从来源毫秒位置映射到已校正时轴，供 Note、Hold 与 BGM 共用。
    /// @param timestamp 来源物件或自动采样的毫秒位置。
    /// @param initialBpm 首个锚点之前使用的速度，与构造时的初始速度一致。
    /// @return 基于来源时刻所属区间计算的实际绝对拍位。
    /// @note 红线前使用初始速度；精确同刻事件复用该红线的实际拍位。
    double beatAt(double timestamp, double initialBpm) const
    {
        // upper_bound 选择来源时刻之后的首个锚点，前一项覆盖当前区间。
        // 精确同刻会落到红线之后，使判定与 BGM 都使用同一实际起点。
        const auto next =
            std::upper_bound(m_anchors.begin(),
                             m_anchors.end(),
                             timestamp,
                             [](double value, const Anchor& anchor) {
                                 return value < anchor.m_sourceTime;
                             });
        if ( next == m_anchors.begin() ) return timestamp * initialBpm / 60000;
        const auto& anchor = *(next - 1);
        return anchor.m_beat +
               (timestamp - anchor.m_sourceTime) * anchor.m_bpm / 60000;
    }

private:
    /// @brief 来源时刻及其实际拍位、后续速度的数值快照。
    struct Anchor {
        /// @brief 原谱毫秒位置，不累计编码误差。
        double m_sourceTime;
        /// @brief BMS 文本实际表示的绝对拍位。
        double m_beat;
        /// @brief 此事件之后区间使用的 BPM。
        double m_bpm;
    };
    /// @brief 按来源时刻排序的区间起点，只在离线导出中增长。
    std::vector<Anchor> m_anchors;
    /// @brief 当前区间的正 BPM。
    double m_bpm;
    /// @brief 单事件允许的小节分数误差。
    double m_tolerance;
    /// @brief 前一事件的来源时刻，用于保持同刻指令关系。
    double m_sourceTime{ 0 };
    /// @brief 根据实际拍位计算的前一事件解码时刻。
    double m_actualTime{ 0 };
    /// @brief 前一事件的实际编码拍位，禁止向前回绕。
    double m_beat{ 0 };
};
}  // namespace MMM::Bms
