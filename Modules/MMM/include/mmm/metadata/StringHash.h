#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace MMM
{
/// @brief 支持异构字符串键查找的哈希器。
/// @details 允许 `const char*`、string_view 和 string 在不构造临时 string
/// 时查表。
/// @note 独立于元数据 DTO，使采样属性表无需包含谱面或物件定义。
/// 不保存状态；属性表复制时继续使用同一值语义哈希器。
struct StringHash {
    /// @brief 启用 unordered_map 的透明查找重载。
    using is_transparent = void;
    /// @note 字符指针必须指向以零结尾的有效字符串。
    /// @brief 计算 C 字符串键的哈希值。
    [[nodiscard]] size_t operator()(const char* txt) const
    {
        return std::hash<std::string_view>{}(txt);
    }
    /// @brief 计算字符串视图键的哈希值。
    /// @note 直接借用字符区间，不构造临时拥有型字符串。
    [[nodiscard]] size_t operator()(std::string_view txt) const
    {
        return std::hash<std::string_view>{}(txt);
    }
    /// @brief 计算拥有型字符串键的哈希值。
    [[nodiscard]] size_t operator()(const std::string& txt) const
    {
        return std::hash<std::string>{}(txt);
    }
};

}  // namespace MMM
