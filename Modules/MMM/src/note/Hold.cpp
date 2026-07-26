#include "mmm/note/Hold.h"
#include "mmm/SafeParse.h"
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace MMM
{
namespace
{
/// @brief 按冒号拆分 osu! Hold 参数，并保留结尾空字段。
/// @param value 待拆分的 Hold 参数字符串。
/// @return 包含结束时间和 HitSample 参数的字段列表。
std::vector<std::string> splitOsuHoldParameters(std::string_view value)
{
    std::vector<std::string> fields;
    std::size_t              start = 0;
    while ( start <= value.size() ) {
        const std::size_t end = value.find(':', start);
        fields.emplace_back(value.substr(start,
                                         end == std::string_view::npos
                                             ? value.size() - start
                                             : end - start));
        if ( end == std::string_view::npos ) break;
        start = end + 1;
    }
    return fields;
}

/// @brief 将 osu! Hold 的 HitSample 参数与自定义音效文件名重新组合。
/// @param original 原始 Hold 参数字符串。
/// @param sampleFile 通用物件字段保存的自定义音效文件名。
/// @return 不含结束时间、固定包含五段参数的 HitSample 字符串。
std::string composeOsuHoldHitSample(std::string_view original,
                                    std::string_view sampleFile)
{
    auto fields = splitOsuHoldParameters(original);
    fields.resize(6);
    fields[5] = sampleFile;

    std::ostringstream stream;
    for ( std::size_t index = 1; index < fields.size(); ++index ) {
        if ( index > 1 ) stream << ':';
        stream << fields[index];
    }
    return stream.str();
}
}  // namespace

/// @brief 从osu描述加载
void Hold::from_osu_description(const std::vector<std::string>& description,
                                int32_t                         orbit_count)
{
    using enum NoteMetadataType;
    auto& osunote_prop = m_metadata.note_properties[OSU];

    m_type = NoteType::HOLD;
    for ( int i = 0; i < description.size(); ++i ) {
        switch ( i ) {
        case 0: {
            // 位置
            m_track = std::floor(
                MMM::Internal::safeStod(MMM::Internal::safeAt(description, 0)) *
                orbit_count / 512);
            break;
        }
        case 2: {
            // 时间戳
            m_timestamp =
                MMM::Internal::safeStod(MMM::Internal::safeAt(description, 2));
            break;
        }
        case 4: {
            // 音效
            osunote_prop["sample"] = std::to_string(
                MMM::Internal::safeStoi(MMM::Internal::safeAt(description, 4)));
            break;
        }
        default: break;
        }
    }

    // 长条结束时间
    // 结束时间和音效组参数粘一起了
    const std::string sampleGroup = MMM::Internal::safeAt(description, 5);
    const auto        last_paras  = splitOsuHoldParameters(sampleGroup);

    osunote_prop["samplegroup"] = sampleGroup;
    m_boundSound                = MMM::Internal::safeAt(last_paras, 5);

    m_duration = static_cast<int32_t>(MMM::Internal::safeStod(
                     MMM::Internal::safeAt(last_paras, 0))) -
                 m_timestamp;
}

/// @brief 转换为osu描述
std::string Hold::to_osu_description(int32_t orbit_count)
{
    using enum NoteMetadataType;
    auto& osunote_prop = m_metadata.note_properties[OSU];
    /*
     * 长键格式:
     * x,y,开始时间,物件类型,长键音效,结束时间:音效组:附加音效组:音效参数:音量:[自定义音效文件]
     * 对于长键:
     *   - 物件类型 = 128 (Hold note)
     *   - 结束时间 = 开始时间 + hold_time
     */

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0);

    // x 坐标 (根据轨道数计算)
    // 原公式: orbit = floor(x * orbit_count / 512)
    // 反推: x = orbit * 512 / orbit_count
    int x = static_cast<int>((double(m_track) + 0.5) * 512 / orbit_count);
    oss << x << ",";

    // y 坐标 (固定192)
    oss << "192,";

    // 开始时间
    oss << m_timestamp << ",";

    // 物件类型 (HOLD=128)
    oss << "128,";

    // 长键音效 (NoteSample枚举值)
    if ( auto it = osunote_prop.find("sample"); it != osunote_prop.end() ) {
        oss << it->second << ",";
    } else {
        oss << "0" << ",";
    }

    // 结束时间和音效组参数
    int end_time = m_timestamp + m_duration;
    oss << end_time << ":";

    // 音效组参数
    const auto sampleGroup = osunote_prop.contains("samplegroup")
                                 ? osunote_prop.at("samplegroup")
                                 : std::string("0:0:0:0:0:");
    oss << composeOsuHoldHitSample(sampleGroup, m_boundSound);

    return oss.str();
}

}  // namespace MMM
