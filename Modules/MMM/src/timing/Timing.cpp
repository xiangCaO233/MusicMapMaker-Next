#include "mmm/timing/Timing.h"
#include "mmm/SafeParse.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace MMM
{

/// @brief 从osu的字符串读取
void Timing::from_osu_description(std::vector<std::string>& description)
{
    using enum TimingMetadataType;
    auto& osutiming_prop = m_metadata.timing_properties[OSU];
    /*
     * 时间语法：时间,拍长,节拍,音效组,音效参数,音量,是否为非继承时间点（红线）,效果
     *时间（整型）：
     *时间轴区间的开始时间，以谱面音频开始为原点，单位是毫秒。
     *这个时间轴区间的结束时间即为下一个时间点的开始时间（如果这是最后一个时间点，则无结束时间）。
     *
     *拍长（精准小数）： 这个参数有两种含义：
     *对于非继承时间点（红线），这个参数即一拍的长度，单位是毫秒。
     *对于继承时间点（绿线），这个参数为负值，去掉符号后被 100
     *整除，即为这根绿线控制的滑条速度。例如，-50
     *代表这一时间段的滑条速度均为基础滑条速度的 2 倍。
     *
     *节拍（整型）： 一小节中的拍子数量。继承时间点（绿线）的这个值无效果。
     *
     *音效组（整型）： 物件使用的默认音效组（0 = 谱面默认设置（SampleSet），1 =
     *normal，2 = soft，3 = drum）。
     *
     *音效参数（整型）： 物件使用的自定义音效参数。
     *0 表示使用 osu! 默认的音效。
     *
     *音量（整型）： 击打物件的音量（0 - 100）。
     *
     *是否为非继承时间点（红线）（布尔值）： 字面意思。
     *效果（整型）： 一位影响时间点特殊效果的参数。参见：效果部分。
     */
    m_timestamp = MMM::Internal::safeStod(description.at(0));

    // 上一个基准bpm
    double last_base_bpm = 0.0;

    // 先判断是否继承时间点
    auto is_inherit_timing = MMM::Internal::safeStod(description.at(6)) == 0;

    // beat_length
    m_beat_length = std::stod(description.at(1));

    if ( is_inherit_timing ) {
        // bpm只存储倍速--并非bpm
        auto is_base_timing     = false;
        m_bpm                   = last_base_bpm;
        m_timingEffect          = TimingEffect::SCROLL;
        m_timingEffectParameter = m_beat_length;
    } else {
        // 真实bpm
        m_bpm = 1.0 / m_beat_length * 1000.0 * 60.0;

        // 限制极端数值，防止后续计算（如拍线生成）进入死循环或溢出
        if ( std::isinf(m_bpm) || std::isnan(m_bpm) || m_bpm > 1000000.0 ) {
            m_bpm = 1000000.0;
        }
        if ( m_bpm < 0.1 ) {
            m_bpm = 0.1;
        }

        last_base_bpm           = m_bpm;
        m_timingEffectParameter = m_bpm;
        m_timingEffect          = TimingEffect::BPM;
    }

    // beats
    osutiming_prop["beat"] = std::to_string(
        MMM::Internal::safeStoi(MMM::Internal::safeAt(description, 2)));
    osutiming_prop["sampleset"] = std::to_string(
        MMM::Internal::safeStoi(MMM::Internal::safeAt(description, 3)));
    osutiming_prop["sampleparameter"] = std::to_string(
        MMM::Internal::safeStoi(MMM::Internal::safeAt(description, 4)));
    osutiming_prop["volume"] = std::to_string(
        MMM::Internal::safeStoi(MMM::Internal::safeAt(description, 5)));
    osutiming_prop["effect"] = std::to_string(static_cast<int8_t>(
        MMM::Internal::safeStoi(MMM::Internal::safeAt(description, 7))));
}

/// @brief 转换为osu的字符串
std::string Timing::to_osu_description()
{
    using enum TimingMetadataType;
    auto& osutiming_prop = m_metadata.timing_properties[OSU];
    /*
     * 格式: 时间,拍长,节拍,音效组,音效参数,音量,是否为非继承时间点,效果
     * 对于非继承时间点(红线):
     *   拍长 = 60000 / BPM
     * 对于继承时间点(绿线):
     *   拍长 = -100 / 滑条速度倍数
     */
    std::ostringstream oss;
    // 时间
    oss << m_timestamp << ",";

    // 拍长
    switch ( m_timingEffect ) {
    case TimingEffect::BPM: {
        // 非继承时间点(红线): 拍长为正，表示毫秒每拍
        double ms_per_beat = 60000.0 / m_bpm;
        oss << std::fixed << std::setprecision(12) << ms_per_beat << ",";
        break;
    }
    case TimingEffect::SCROLL: {
        // 继承时间点(绿线): 拍长为负值，表示滑条速度倍数
        oss << std::fixed << std::setprecision(12) << m_beat_length << ",";
        break;
    }
    }

    // 节拍
    if ( auto it = osutiming_prop.find("beat"); it != osutiming_prop.end() ) {
        oss << it->second << ",";
    } else {
        oss << "4" << ",";
    }
    // 音效组
    if ( auto it = osutiming_prop.find("sampleset");
         it != osutiming_prop.end() ) {
        oss << it->second << ",";
    } else {
        oss << "0" << ",";
    }
    // 音效参数
    if ( auto it = osutiming_prop.find("sampleparameter");
         it != osutiming_prop.end() ) {
        oss << it->second << ",";
    } else {
        oss << "0" << ",";
    }
    // 音量
    if ( auto it = osutiming_prop.find("volume"); it != osutiming_prop.end() ) {
        oss << it->second << ",";
    } else {
        oss << "50" << ",";
    }
    // 是否为非继承时间点 (0=继承/绿线, 1=非继承/红线)
    oss << (m_timingEffect == TimingEffect::SCROLL ? "0" : "1") << ",";
    // 效果
    if ( auto it = osutiming_prop.find("effect"); it != osutiming_prop.end() ) {
        oss << it->second << ",";
    } else {
        oss << "0";
    }
    return oss.str();
}

}  // namespace MMM
