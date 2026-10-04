#include "config/TimingInterpolationPreferences.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace MMM::Config
{
namespace
{
/// @brief 只接受有限 JSON 数值，避免配置损坏进入浮点投影或整数转换。
/// @param json 已确认是对象的配置节点。
/// @param name 字段名称，缺失或类型不符时使用 fallback。
/// @param fallback 当前字段的领域默认值。
/// @return 有限数值或 fallback；不把字符串、布尔量强转为数值。
/// @pre json 已通过对象类型检查，fallback 为有限值。
/// @note 读取前检查类型，避免不可信用户配置触发 JSON 类型异常。
double finiteNumber(const nlohmann::json& json, const char* name,
                    double fallback)
{
    const auto item = json.find(name);
    if ( item == json.end() || !item->is_number() ) return fallback;
    const double value = item->get<double>();
    // NaN 或无穷值不能依靠 clamp 修复，先明确回到有限默认值。
    return std::isfinite(value) ? value : fallback;
}
}  // namespace

/// @brief 将最后确认的工具选项写入应用配置。
/// @param json 配置根中的工具选项节点，由本函数整体赋值。
/// @param value 已确认使用的选项，不含谱面实体、位置或缓存身份。
/// @details 自变量以布尔选择保存，曲线以稳定标识保存，不依赖 UI 标签。
/// 分拍分子与分母分别保存，不通过浮点 Hz 反推有理采样步长。
/// @note 坐标以归一比例保存，窗口大小与显示缩放不参与持久化。
void to_json(nlohmann::json& json, const TimingInterpolationPreferences& value)
{
    json = { { "curve", value.m_curve },
             { "useBeats", value.m_useBeats },
             { "samplesPerSecond", value.m_samplesPerSecond },
             { "beatNumerator", value.m_beatNumerator },
             { "beatDenominator", value.m_beatDenominator },
             { "controls", value.m_controls },
             { "expression", value.m_expression } };
}

/// @brief 逐字段恢复偏好，旧配置与损坏字段不会阻断其他有效选项。
/// @param json 任意用户配置节点，允许缺失、标量或部分对象。
/// @param value 接收规范化结果；先重置默认，避免沿用调用前的旧数据。
/// @details 密度和控制点只接受有限数值；整数步长限制在领域允许范围。
/// 未知曲线标识采用线性回退，未来版本的标识不会成为错误枚举。
/// @note 源码只检查容量，不绑定旧定义域，真实公式合法性由新段验证。
/// @details 仅此低频读取入口修正配置；编辑窗口里的非法输入保留供用户修正。
void from_json(const nlohmann::json&           json,
               TimingInterpolationPreferences& value)
{
    value = {};
    // 旧配置缺少整个对象，以及手工改成标量的配置都采用完整默认值。
    if ( !json.is_object() ) return;
    if ( const auto item = json.find("curve");
         item != json.end() && item->is_string() ) {
        const auto& name = item->get_ref<const std::string&>();
        if ( std::find(TIMING_CURVE_PREFERENCE_NAMES.begin(),
                       TIMING_CURVE_PREFERENCE_NAMES.end(),
                       name) != TIMING_CURVE_PREFERENCE_NAMES.end() )
            value.m_curve = name;
    }
    if ( const auto item = json.find("useBeats");
         item != json.end() && item->is_boolean() )
        value.m_useBeats = item->get<bool>();
    // 密度只要求有限正数，具体段落的采样总量仍由领域模型校验。
    const double density = finiteNumber(json, "samplesPerSecond", 16);
    if ( density > 0 ) value.m_samplesPerSecond = density;
    // 与领域分拍上限保持一致；先限制 double 再转 int，避免窄化溢出。
    value.m_beatNumerator = static_cast<int>(
        std::clamp(finiteNumber(json, "beatNumerator", 1), 1.0, 65536.0));
    value.m_beatDenominator = static_cast<int>(
        std::clamp(finiteNumber(json, "beatDenominator", 2), 1.0, 65536.0));
    if ( const auto item = json.find("controls");
         item != json.end() && item->is_array() && item->size() == 4 ) {
        // 个别坐标损坏时只恢复该坐标，不丢弃其他可用控制点。
        // 数组长度不符则保留整组默认，不能将索引错位当成合法控制点。
        for ( std::size_t index = 0; index < 4; ++index ) {
            if ( !(*item)[index].is_number() ) continue;
            const double coordinate = (*item)[index].get<double>();
            if ( std::isfinite(coordinate) )
                value.m_controls[index] = std::clamp(coordinate, 0.0, 1.0);
        }
        // 两控制点的顺序是时间反解不变量，配置不能令 X 轴回折。
        if ( value.m_controls[0] > value.m_controls[2] )
            std::swap(value.m_controls[0], value.m_controls[2]);
    }
    if ( const auto item = json.find("expression");
         item != json.end() && item->is_string() ) {
        // 类型确认后借用源文本，只在容量合法时复制进配置对象。
        const auto& expression = item->get_ref<const std::string&>();
        // 超长公式整体拒绝而不是截断，截断可能改变数学含义或破坏 UTF-8。
        if ( expression.size() <= 2048 ) value.m_expression = expression;
    }
    // 没有源码的自定义模式无法重建，安全回退不影响其它已恢复选项。
    if ( value.m_curve == "Custom" && value.m_expression.empty() )
        value.m_curve = "Linear";
}
}  // namespace MMM::Config
