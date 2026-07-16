#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace MMM::Font
{

/// @brief 可由 ImGui 字体图集加载的系统字体文件定位信息。
struct SystemFontFace {
    /// @brief 字体文件的绝对路径。
    std::filesystem::path m_filePath;

    /// @brief 字体在 TTC/OTC 集合中的从零开始的 face index。
    int m_faceIndex{ 0 };

    /// @brief 系统报告的字体家族名称。
    std::string m_familyName;
};

/// @brief 解析当前系统首选 UI 字体及必要的中文回退字体。
/// @return 按加载优先级排列且已经去重的字体列表；解析失败时返回空列表。
[[nodiscard]] std::vector<SystemFontFace> resolvePreferredSystemFonts();

}  // namespace MMM::Font
