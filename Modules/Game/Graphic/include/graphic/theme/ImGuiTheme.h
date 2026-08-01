#pragma once

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace MMM::Graphic
{

/// @brief ImGui 主题实例来源。
enum class ImGuiThemeOrigin : std::uint8_t {
    BuiltIn,  ///< 编译进程序的内置主题。
    Plugin    ///< 从用户配置目录加载的 Lua 主题插件。
};

/// @brief 可注册、可选择并可应用的独立 ImGui 主题实例。
class ImGuiTheme final
{
public:
    /// @brief 主题样式应用回调。
    using ApplyFunction = std::function<void(ImGuiStyle&)>;

    /// @brief 构造主题实例。
    /// @param id 持久化使用的稳定主题 ID。
    /// @param displayName 设置界面显示名称。
    /// @param origin 主题来源。
    /// @param baseThemeId 插件主题继承的内置主题 ID；内置主题为空。
    /// @param sourcePath 插件入口文件路径；内置主题为空。
    /// @param applyFunction 将主题字段写入目标 ImGuiStyle 的回调。
    ImGuiTheme(std::string id, std::string displayName, ImGuiThemeOrigin origin,
               std::string baseThemeId, std::filesystem::path sourcePath,
               ApplyFunction applyFunction);

    /// @brief 获取稳定主题 ID。
    /// @return 主题 ID。
    [[nodiscard]] std::string_view id() const { return m_id; }

    /// @brief 获取设置界面显示名称。
    /// @return 主题显示名称。
    [[nodiscard]] std::string_view displayName() const { return m_displayName; }

    /// @brief 获取主题来源。
    /// @return 内置或插件来源。
    [[nodiscard]] ImGuiThemeOrigin origin() const { return m_origin; }

    /// @brief 获取插件主题继承的内置主题 ID。
    /// @return 内置主题 ID；内置主题自身返回空。
    [[nodiscard]] std::string_view baseThemeId() const { return m_baseThemeId; }

    /// @brief 获取插件入口文件路径。
    /// @return 插件路径；内置主题返回空路径。
    [[nodiscard]] const std::filesystem::path& sourcePath() const
    {
        return m_sourcePath;
    }

    /// @brief 将本主题的样式字段写入目标样式。
    /// @param style 待修改的 ImGui 样式实例。
    void apply(ImGuiStyle& style) const;

private:
    /// @brief 持久化使用的稳定主题 ID。
    std::string m_id;
    /// @brief 设置界面显示名称。
    std::string m_displayName;
    /// @brief 主题来源。
    ImGuiThemeOrigin m_origin{ ImGuiThemeOrigin::BuiltIn };
    /// @brief 插件主题继承的内置主题 ID。
    std::string m_baseThemeId;
    /// @brief 插件入口文件路径。
    std::filesystem::path m_sourcePath;
    /// @brief 样式应用回调。
    ApplyFunction m_applyFunction;
};

}  // namespace MMM::Graphic
