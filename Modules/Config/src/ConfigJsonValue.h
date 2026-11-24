#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace MMM::Config
{
/// @brief 检查配置值及容器元素能否进入既有 JSON 转换器。
/// @tparam T 字段的 C++ 类型，由调用处的默认值决定。
/// @param value 从配置文件取得的候选字段或容器元素。
/// @return 类型兼容且数值可安全转换时返回 true。
/// @details 容器在整个字段通过检查后才转换，坏元素不会留下半份设置。
template<typename T> bool acceptsConfigJson(const nlohmann::json& value)
{
    // JSON 数字与布尔是不同类型，避免手工编辑的 0 改变开关语义。
    if constexpr ( std::is_same_v<T, bool> ) {
        return value.is_boolean();
    } else if constexpr ( std::is_arithmetic_v<T> ) {
        // 禁止布尔值参与数值转换，并在窄化之前检查可表示范围。
        if ( !value.is_number() ) return false;
        const auto number = value.get<long double>();
        if constexpr ( std::is_integral_v<T> ) {
            // 用上界的下一整数表示半开区间，兼容 long double 等宽于 double
            // 的平台。
            const auto limit = std::ldexp(1.0L, std::numeric_limits<T>::digits);
            return value.is_number_integer() &&
                   number >= (std::is_signed_v<T> ? -limit : 0) &&
                   number < limit;
        } else {
            // JSON 整数也可用于浮点字段，范围检查同时排除非有限值。
            return std::abs(number) <= std::numeric_limits<T>::max();
        }
        // 字符串需先于容器判断，因为它也有 value_type。
    } else if constexpr ( std::is_same_v<T, std::string> ) {
        return value.is_string();
    } else if constexpr ( std::is_enum_v<T> ) {
        // 各枚举的 ADL 读取器负责兼容旧名称和未知值。
        return value.is_string();
    } else if constexpr ( requires { typename T::mapped_type; } ) {
        // 资源名等键由用户定义，仅验证值，保留任意合法键名。
        return value.is_object() &&
               std::all_of(value.begin(), value.end(), [](const auto& item) {
                   return acceptsConfigJson<typename T::mapped_type>(item);
               });
    } else if constexpr ( requires { typename T::value_type; } ) {
        if ( !value.is_array() ) return false;
        // 固定数组缺元素时，标准库适配器会访问不存在的下标。
        if constexpr ( requires { std::tuple_size<T>::value; } ) {
            if ( value.size() < std::tuple_size_v<T> ) return false;
        }
        // 逐层检查颜色矩阵等嵌套数组，单看外层数组不足以保证转换成功。
        return std::all_of(value.begin(), value.end(), [](const auto& item) {
            return acceptsConfigJson<typename T::value_type>(item);
        });
    } else {
        // 配置对象负责验证自己的字段，各字段可以独立采用默认值。
        return value.is_object();
    }
}

/// @brief 以字段级默认值恢复缺失、类型错误或超范围的配置数据。
/// @param object 保存配置字段的 JSON 节点。
/// @param key 该字段在当前版本或历史版本中的持久化键。
/// @param fallback 调用处既有业务默认值。
/// @return 转换后的值，或调用处指定的默认值。
/// @note find 对非对象安全，旧字段迁移继续由调用方决定优先级。
/// @note 仅用于低频持久化读取，避免把容器预检引入每帧配置访问。
template<typename T>
T readConfigValue(const nlohmann::json& object, std::string_view key,
                  T fallback)
{
    // find 只借用现有节点，缺失字段不会物化为空值，也不会写回原 JSON。
    const auto value = object.find(key);
    if ( value == object.end() || !acceptsConfigJson<T>(*value) )
        return fallback;
    // 类型预检后仍走同一个 ADL 入口，范围约束与别名迁移继续由原读取器处理。
    return value->template get<T>();
}
}  // namespace MMM::Config
