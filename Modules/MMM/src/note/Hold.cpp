#include "mmm/note/Hold.h"
#include "mmm/SafeParse.h"
#include <cmath>
#include <ranges>

namespace MMM
{
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
    std::string        token;
    std::istringstream noteiss(description.at(5));

    // 最后一组的第一个参数就是结束时间
    std::vector<std::string> last_paras;
    while ( std::getline(noteiss, token, ':') ) {
        last_paras.push_back(token);
    }

    osunote_prop["samplegroup"] = description.at(5);

    m_duration =
        static_cast<int32_t>(MMM::Internal::safeStod(last_paras.at(0))) -
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
    if ( auto it = osunote_prop.find("samplegroup");
         it != osunote_prop.end() ) {
        std::string notegroup = it->second;
        if ( auto it_pos = std::ranges::find(notegroup, ':');
             it_pos != notegroup.end() ) {
            // 只取第一个冒号之后的部分作为 hitSample
            std::string samplepart =
                notegroup.substr(std::distance(notegroup.begin(), it_pos) + 1);
            oss << samplepart;
        } else {
            // 如果没有冒号，则可能是旧格式或异常，尝试直接补齐
            oss << "0:0:0:0:";
        }
    } else {
        oss << "0:0:0:0:";
    }

    return oss.str();
}

}  // namespace MMM
