#pragma once

#include "config/EditorSettings.h"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace MMM::Config
{

/// @brief 物件配色方案文件的格式标识。
inline constexpr const char* NOTE_COLOR_PALETTE_FILE_FORMAT =
    "MusicMapMaker.NoteColorPalette";

/// @brief 当前物件配色方案文件格式版本。
inline constexpr std::uint32_t NOTE_COLOR_PALETTE_FILE_VERSION = 1;

/// @brief 物件配色方案文件扩展名。
inline constexpr const char* NOTE_COLOR_PALETTE_FILE_EXTENSION = ".mmpalette";

/// @brief 可独立分享的物件配色方案文件结构。
struct NoteColorPaletteFile {
    /// @brief 文件格式版本。
    std::uint32_t version{ NOTE_COLOR_PALETTE_FILE_VERSION };

    /// @brief 文件内保存的单个物件配色方案。
    NoteColorPaletteScheme scheme;
};

/// @brief 将物件配色方案文件序列化为 JSON。
/// @param j 输出 JSON。
/// @param file 待序列化文件。
inline void to_json(nlohmann::json& j, const NoteColorPaletteFile& file)
{
    j = nlohmann::json{ { "format", NOTE_COLOR_PALETTE_FILE_FORMAT },
                        { "version", file.version },
                        { "scheme", file.scheme } };
}

/// @brief 从 JSON 读取物件配色方案文件。
/// @param j 输入 JSON。
/// @param file 输出文件结构。
inline void from_json(const nlohmann::json& j, NoteColorPaletteFile& file)
{
    file.version = j.value("version", NOTE_COLOR_PALETTE_FILE_VERSION);
    file.scheme  = j.value("scheme", NoteColorPaletteScheme());
}

/// @brief 将物件配色方案导出为可独立分享的 JSON 文件。
/// @param path 目标文件路径。
/// @param scheme 待导出的物件配色方案。
/// @return 文件完整写入目标路径时返回 true。
[[nodiscard]] bool exportNoteColorPaletteFile(
    const std::filesystem::path& path, const NoteColorPaletteScheme& scheme);

}  // namespace MMM::Config
