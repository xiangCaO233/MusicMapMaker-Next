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
    // Hold 参数允许以冒号结束，末尾空字段必须保留为“无自定义音效”。
    while ( start <= value.size() ) {
        const std::size_t end = value.find(':', start);
        fields.emplace_back(value.substr(start,
                                         end == std::string_view::npos
                                             ? value.size() - start
                                             : end - start));
        if ( end == std::string_view::npos ) break;
        // 仅移动视图起点，避免在逐段扫描时构造临时源字符串。
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
    // 第一段是结束时间，之后固定五段 HitSample；缺失部分补为空值。
    fields.resize(6);
    // 文件名以通用采样绑定为准，防止资源重命名后回写旧来源值。
    fields[5] = sampleFile;

    std::ostringstream stream;
    // 从索引一开始跳过结束时间，只组装可复用的 HitSample 部分。
    for ( std::size_t index = 1; index < fields.size(); ++index ) {
        // 首个 HitSample 字段前不写分隔符，后续字段以冒号连接。
        if ( index > 1 ) stream << ':';
        stream << fields[index];
    }
    return stream.str();
}
}  // namespace

/// @brief 从 osu!mania HitObject 字段加载长条物件。
/// @details
/// 逗号字段负责起点、轨道和音效位，最后一段再以冒号拆出终点及 HitSample。
/// SafeParse 负责缺失字段的默认化，本函数同步刷新通用采样绑定。
void Hold::from_osu_description(const std::vector<std::string>& description,
                                int32_t                         orbit_count)
{
    using enum NoteMetadataType;
    auto& osunote_prop = m_metadata.note_properties[OSU];

    // 导入对象可能被复用，先恢复派生类应有的固定类型。
    m_type = NoteType::HOLD;
    for ( int i = 0; i < description.size(); ++i ) {
        // 仅解析通用模型需要的逗号字段，其余来源字段保持在原描述中。
        switch ( i ) {
        case 0: {
            // 与普通 Note 使用相同的 0 至 512 横坐标离散规则。
            m_track = std::floor(
                MMM::Internal::safeStod(MMM::Internal::safeAt(description, 0)) *
                orbit_count / 512);
            break;
        }
        case 2: {
            // osu! 时间统一以毫秒为单位，无需在领域层换算。
            m_timestamp =
                MMM::Internal::safeStod(MMM::Internal::safeAt(description, 2));
            break;
        }
        case 4: {
            // 位标志属于 osu! 私有元数据，不与自定义文件绑定混用。
            osunote_prop["sample"] = std::to_string(
                MMM::Internal::safeStoi(MMM::Internal::safeAt(description, 4)));
            break;
        }
        default: break;
        }
    }

    // osu! 将结束时间与 HitSample 粘在同一逗号字段，需要二次按冒号解析。
    const std::string sampleGroup = MMM::Internal::safeAt(description, 5);
    const auto        last_paras  = splitOsuHoldParameters(sampleGroup);

    // 保存完整字段，以便导出时保留音效组、参数和音量等来源信息。
    osunote_prop["samplegroup"] = sampleGroup;
    const auto sampleFile       = MMM::Internal::safeAt(last_paras, 5);
    if ( sampleFile.empty() ) {
        // 空文件名必须清除复用对象原有的通用采样绑定。
        clearSampleBinding();
    } else {
        setSampleBinding(AudioSampleBinding{ sampleFile, 1.0F });
    }

    // 领域模型保存持续时间，使用解析出的绝对终点减去起点。
    m_duration = static_cast<int32_t>(MMM::Internal::safeStod(
                     MMM::Internal::safeAt(last_paras, 0))) -
                 m_timestamp;
}

/// @brief 转换为 osu!mania Hold HitObject 描述。
/// @details 由起点和持续时间重建绝对终点，并用通用采样绑定刷新文件名段。
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
    // osu!mania HitObject 使用整数横坐标与毫秒时间输出。
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

    // osu! 要求先写绝对终点，再紧接冒号分隔的 HitSample 参数。
    int end_time = m_timestamp + m_duration;
    // 与当前整数输出精度一致，终点按格式可表示的整毫秒写出。
    oss << end_time << ":";

    // 缺少来源字段时使用完整六段默认值，确保输出仍可被 osu! 解析。
    const auto             sampleGroup = osunote_prop.contains("samplegroup")
                                             ? osunote_prop.at("samplegroup")
                                             : std::string("0:0:0:0:0:");
    const auto&            binding     = getSampleBinding();
    const std::string_view sampleFile =
        binding ? std::string_view(binding->m_audioResourceId)
                : std::string_view{};
    // 无绑定时仍保留空文件名字段，避免缩短标准 HitSample 结构。
    oss << composeOsuHoldHitSample(sampleGroup, sampleFile);

    // 返回值不包含换行，由文件级写出器统一控制行边界。
    return oss.str();
}

}  // namespace MMM
