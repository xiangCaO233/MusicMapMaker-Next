#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace MMM::Config
{

struct EditorSettings;
class SkinManager;

/// @brief 可供用户选择的字体名称与文件路径。
using AvailableFont = std::pair<std::string, std::filesystem::path>;

/// @brief 在字体偏好无法解析时将其重置为皮肤默认字体。
/// @param preference 字体名称、外部字体路径或 Default。
/// @param availableFonts 当前皮肤提供的可选字体。
/// @return 偏好被重置时返回 true。
/// @warning
/// 低频配置校验路径：会访问文件系统，只能在启动、皮肤切换等资源重载流程调用。
[[nodiscard]] bool resetUnavailableFontPreference(
    std::string& preference, std::span<const AvailableFont> availableFonts);

/// @brief 根据当前皮肤校验并纠正 ASCII 与 CJK 字体偏好。
/// @param settings 需要纠正的编辑器设置。
/// @param skinManager 已成功加载的当前皮肤管理器。
/// @return 任一字体偏好被重置时返回 true。
/// @warning
/// 低频配置校验路径：会访问文件系统，只能在启动、皮肤切换等资源重载流程调用。
[[nodiscard]] bool resetUnavailableFontPreferences(
    EditorSettings& settings, const SkinManager& skinManager);

}  // namespace MMM::Config
