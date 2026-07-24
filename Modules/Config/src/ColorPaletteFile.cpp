#include "config/ColorPaletteFile.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <system_error>
#include <utility>

namespace
{

/// @brief 从 JSON 数组读取固定数量的 RGBA 颜色。
/// @tparam ColorCount 颜色数量。
/// @param value 输入 JSON 数组。
/// @param colors 输出颜色数组。
/// @return 数组结构完整且所有通道位于 0 到 1 时返回 true。
template<std::size_t ColorCount>
bool readColorArray(const nlohmann::json&                         value,
                    std::array<std::array<float, 4>, ColorCount>& colors)
{
    if ( !value.is_array() || value.size() != ColorCount ) {
        return false;
    }
    for ( std::size_t colorIndex = 0; colorIndex < ColorCount; ++colorIndex ) {
        const auto& color = value[colorIndex];
        if ( !color.is_array() || color.size() != 4 ) {
            return false;
        }
        for ( std::size_t channelIndex = 0; channelIndex < 4; ++channelIndex ) {
            if ( !color[channelIndex].is_number() ) {
                return false;
            }
            const double channel = color[channelIndex].get<double>();
            if ( !std::isfinite(channel) || channel < 0.0 || channel > 1.0 ) {
                return false;
            }
            colors[colorIndex][channelIndex] = static_cast<float>(channel);
        }
    }
    return true;
}

}  // namespace

namespace MMM::Config
{

bool exportColorPaletteFile(const std::filesystem::path& path,
                            const ColorPaletteScheme&    scheme)
{
    if ( path.empty() ) {
        XERROR("Cannot export color palette to an empty path");
        return false;
    }

    const std::filesystem::path parent = path.parent_path();
    if ( !parent.empty() ) {
        std::error_code createDirectoryError;
        std::filesystem::create_directories(parent, createDirectoryError);
        if ( createDirectoryError ) {
            XERROR(
                "Failed to create color palette export directory: {}. "
                "Error: {}",
                pathToUtf8(parent),
                createDirectoryError.message());
            return false;
        }
    }

    ColorPaletteFile paletteFile;
    paletteFile.scheme            = scheme;
    const nlohmann::json document = paletteFile;

    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp";
    bool writeSucceeded = false;
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if ( !output.is_open() ) {
            XERROR("Failed to open color palette export file: {}",
                   pathToUtf8(temporaryPath));
            return false;
        }

        output << std::setw(4) << document << '\n';
        writeSucceeded = output.good();
    }

    if ( !writeSucceeded ) {
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        XERROR("Failed to write color palette export file: {}",
               pathToUtf8(temporaryPath));
        return false;
    }

    std::error_code replaceError;
    std::filesystem::rename(temporaryPath, path, replaceError);
    if ( replaceError ) {
        std::error_code copyError;
        std::filesystem::copy_file(
            temporaryPath,
            path,
            std::filesystem::copy_options::overwrite_existing,
            copyError);
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        if ( copyError ) {
            XERROR(
                "Failed to replace color palette export file: {}. "
                "Error: {}",
                pathToUtf8(path),
                copyError.message());
            return false;
        }
    }

    XINFO("Color palette exported: {}", pathToUtf8(path));
    return true;
}

bool importColorPaletteFile(const std::filesystem::path& path,
                            ColorPaletteScheme&          scheme)
{
    if ( path.empty() ) {
        XERROR("Cannot import color palette from an empty path");
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if ( !input.is_open() ) {
        XERROR("Failed to open color palette import file: {}",
               pathToUtf8(path));
        return false;
    }

    const nlohmann::json document =
        nlohmann::json::parse(input, nullptr, false);
    if ( document.is_discarded() || !document.is_object() ||
         !document.contains("format") || !document["format"].is_string() ||
         document["format"].get<std::string>() != COLOR_PALETTE_FILE_FORMAT ||
         !document.contains("version") ||
         !document["version"].is_number_unsigned() ||
         document["version"].get<std::uint32_t>() !=
             COLOR_PALETTE_FILE_VERSION ||
         !document.contains("scheme") || !document["scheme"].is_object() ) {
        XERROR("Invalid color palette file header: {}", pathToUtf8(path));
        return false;
    }

    const auto& storedScheme = document["scheme"];
    if ( !storedScheme.contains("name") || !storedScheme["name"].is_string() ||
         !storedScheme.contains("noteColors") ||
         !storedScheme.contains("beatLineColors") ) {
        XERROR("Incomplete color palette scheme: {}", pathToUtf8(path));
        return false;
    }

    ColorPaletteScheme importedScheme;
    importedScheme.name = storedScheme["name"].get<std::string>();
    if ( !readColorArray(storedScheme["noteColors"],
                         importedScheme.noteColors) ||
         !readColorArray(storedScheme["beatLineColors"],
                         importedScheme.beatLineColors) ) {
        XERROR("Invalid color palette colors: {}", pathToUtf8(path));
        return false;
    }

    scheme = std::move(importedScheme);
    XINFO("Color palette imported: {}", pathToUtf8(path));
    return true;
}

}  // namespace MMM::Config
