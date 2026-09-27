#include "mmm/note/Note.h"
#include "mmm/SafeParse.h"
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace MMM
{
namespace
{
/// @brief 按冒号拆分 osu! HitSample，并保留结尾空字段。
/// @param value 待拆分的 HitSample 字符串。
/// @return HitSample 字段列表。
std::vector<std::string> splitOsuHitSample(std::string_view value)
{
    std::vector<std::string> fields;
    std::size_t              start = 0;
    // `start == size` 仍需迭代一次，才能保留末尾冒号代表的空文件名。
    while ( start <= value.size() ) {
        const std::size_t end = value.find(':', start);
        fields.emplace_back(value.substr(start,
                                         end == std::string_view::npos
                                             ? value.size() - start
                                             : end - start));
        if ( end == std::string_view::npos ) break;
        // 下一段从分隔符之后开始；string_view 避免为搜索过程复制文本。
        start = end + 1;
    }
    return fields;
}

/// @brief 将 osu! HitSample 的前四个参数与自定义音效文件名重新组合。
/// @param original 原始 HitSample 字符串。
/// @param sampleFile 通用物件字段保存的自定义音效文件名。
/// @return 固定包含五段参数的 HitSample 字符串。
std::string composeOsuHitSample(std::string_view original,
                                std::string_view sampleFile)
{
    auto fields = splitOsuHitSample(original);
    // osu! HitSample 固定五段；不足补空，超出的非标准尾段不向外传播。
    fields.resize(5);
    // 通用绑定是文件名的权威来源，覆盖来源元数据中的旧值。
    fields[4] = sampleFile;

    std::ostringstream stream;
    for ( std::size_t index = 0; index < fields.size(); ++index ) {
        if ( index > 0 ) stream << ':';
        stream << fields[index];
    }
    return stream.str();
}
}  // namespace

/// @brief 构造默认普通物件。
Note::Note() {}

/// @brief 销毁多态物件基类。
Note::~Note() {}

/// @brief 从 osu!mania HitObject 字段加载普通物件。
/// @details 缺失或非法字段由 SafeParse 收敛为默认值，不从格式边界抛出异常。
void Note::from_osu_description(const std::vector<std::string>& description,
                                int32_t                         orbit_count)
{
    using enum NoteMetadataType;
    auto& osunote_prop = m_metadata.note_properties[OSU];
    m_type             = NoteType::NOTE;

    // osu! 横坐标使用 0 至 512 连续区间，向下取整映射到内部离散轨道。
    m_track = uint32_t(std::floor(
        MMM::Internal::safeStod(MMM::Internal::safeAt(description, 0)) *
        double(orbit_count) / 512.));

    // 时间戳
    m_timestamp =
        MMM::Internal::safeStod(MMM::Internal::safeAt(description, 2));

    // 位标志保留在来源元数据中，避免通用 NoteType 承担 osu! 私有语义。
    osunote_prop["sample"] = std::to_string(
        MMM::Internal::safeStoi(MMM::Internal::safeAt(description, 4)));

    // 完整 HitSample 原文用于无损回写，自定义文件名提升为通用采样绑定。
    osunote_prop["samplegroup"] = MMM::Internal::safeAt(description, 5);
    const auto hitSampleFields = splitOsuHitSample(osunote_prop["samplegroup"]);
    const auto sampleFile      = MMM::Internal::safeAt(hitSampleFields, 4);
    if ( sampleFile.empty() ) {
        // 空尾段显式清除旧绑定，支持复用对象重新导入。
        clearSampleBinding();
    } else {
        setSampleBinding(AudioSampleBinding{ sampleFile, 1.0F });
    }
}

/// @brief 转换为 osu!mania 普通 HitObject 描述。
/// @details 通用采样绑定覆盖来源元数据内的文件名，其余 HitSample 字段保留。
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
    // 整数精度与 osu! HitObject 的时间和横坐标写法保持一致。
    oss << std::fixed << std::setprecision(0);

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

    // 缺失来源元数据时生成合法的默认 HitSample，再合并当前通用绑定。
    const auto             sampleGroup = osunote_prop.contains("samplegroup")
                                             ? osunote_prop.at("samplegroup")
                                             : std::string("0:0:0:0:");
    const auto&            binding     = getSampleBinding();
    const std::string_view sampleFile =
        binding ? std::string_view(binding->m_audioResourceId)
                : std::string_view{};
    // 即使没有绑定也写出末尾空字段，保留固定 HitSample 结构。
    oss << composeOsuHitSample(sampleGroup, sampleFile);

    return oss.str();
}
}  // namespace MMM
