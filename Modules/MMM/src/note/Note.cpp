#include "mmm/note/Note.h"
#include "mmm/SafeParse.h"
#include <algorithm>
#include <cmath>

namespace MMM
{
Note::Note() {}

Note::~Note() {}

/// @brief 从osu描述加载
void Note::from_osu_description(const std::vector<std::string>& description,
                                int32_t                         orbit_count)
{
    using enum NoteMetadataType;
    auto& osunote_prop = m_metadata.note_properties[OSU];
    m_type             = NoteType::NOTE;

    // 位置
    m_track = uint32_t(std::floor(
        MMM::Internal::safeStod(MMM::Internal::safeAt(description, 0)) *
        double(orbit_count) / 512.));

    // 时间戳
    m_timestamp =
        MMM::Internal::safeStod(MMM::Internal::safeAt(description, 2));

    // 音效
    osunote_prop["sample"] = std::to_string(
        MMM::Internal::safeStoi(MMM::Internal::safeAt(description, 4)));

    // 音效组
    osunote_prop["samplegroup"] = MMM::Internal::safeAt(description, 5);
}

/// @brief 转换为osu描述
std::string Note::to_osu_description(int32_t orbit_count)
{
    using enum NoteMetadataType;
    auto& osunote_prop = m_metadata.note_properties[OSU];
    /*
     * 格式:
     * x,y,开始时间,物件类型,长键音效,结束时间:音效组:附加音效组:音效参数:音量[:自定义音效文件]
     * 对于单键:
     *   - 结束时间 = 开始时间
     *   - 音效组参数格式为:
     * normalSet:additionalSet:sampleSetParameter:volume:[sampleFile]
     */

    std::ostringstream oss;

    // x 坐标 (根据轨道数计算)
    // 原公式: orbit = floor(x * orbit_count / 512)
    // 反推: x = orbit * 512 / orbit_count
    auto x = static_cast<int>((double(m_track) + 0.5) * 512 / orbit_count);
    oss << x << ",";

    // y 坐标 (固定192)
    oss << "192,";

    // 开始时间
    oss << m_timestamp << ",";

    // 物件类型 (NOTE=1)
    oss << "1,";

    // 长键音效 (NoteSample枚举值)
    if ( auto it = osunote_prop.find("sample"); it != osunote_prop.end() ) {
        oss << it->second << ",";
    } else {
        oss << "0" << ",";
    }

    // 音效组参数
    if ( auto it = osunote_prop.find("samplegroup");
         it != osunote_prop.end() ) {
        std::string notegroup = it->second;
        oss << notegroup;
    } else {
        oss << "0:0:0:0:";
    }

    return oss.str();
}
}  // namespace MMM
