#pragma once

#include "config/EditorSettings.h"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace MMM::Config
{

/// @brief 调色方案文件格式标识。
inline constexpr const char* COLOR_PALETTE_FILE_FORMAT =
    "MusicMapMaker.ColorPalette";

/// @brief 当前调色方案文件格式版本。
inline constexpr std::uint32_t COLOR_PALETTE_FILE_VERSION = 1;

/// @brief 调色方案文件扩展名。
inline constexpr const char* COLOR_PALETTE_FILE_EXTENSION = ".mmpalette";

/// @brief 可独立分享的调色方案文件结构。
struct ColorPaletteFile {
    /// @brief 文件格式版本。
    std::uint32_t version{ COLOR_PALETTE_FILE_VERSION };

    /// @brief 文件内保存的单个完整调色方案。
    ColorPaletteScheme scheme;
};

/// @brief 将调色方案文件序列化为 JSON。
/// @param j 输出 JSON。
/// @param file 待序列化文件。
inline void to_json(nlohmann::json& j, const ColorPaletteFile& file)
{
    j = nlohmann::json{ { "format", COLOR_PALETTE_FILE_FORMAT },
                        { "version", file.version },
                        { "scheme", file.scheme } };
}

/// @brief 从 JSON 读取调色方案文件。
/// @param j 输入 JSON。
/// @param file 输出文件结构。
inline void from_json(const nlohmann::json& j, ColorPaletteFile& file)
{
    file.version = j.value("version", COLOR_PALETTE_FILE_VERSION);
    file.scheme  = j.value("scheme", ColorPaletteScheme());
}

/// @brief 将调色方案导出为可独立分享的 JSON 文件。
/// @param path 目标文件路径。
/// @param scheme 待导出的完整调色方案。
/// @return 文件完整写入目标路径时返回 true。
[[nodiscard]] bool exportColorPaletteFile(const std::filesystem::path& path,
                                          const ColorPaletteScheme&    scheme);

/// @brief 从独立 JSON 文件导入完整调色方案。
/// @param path 来源文件路径。
/// @param scheme 校验成功后写入的完整调色方案。
/// @return 文件格式、版本和全部颜色均有效时返回 true。
[[nodiscard]] bool importColorPaletteFile(const std::filesystem::path& path,
                                          ColorPaletteScheme&          scheme);

}  // namespace MMM::Config
