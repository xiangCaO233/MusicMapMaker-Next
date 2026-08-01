#include "config/ColorPaletteFile.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>

namespace
{

/// @brief 读取导出的 JSON 文件且不启用解析异常。
/// @param path 待读取文件。
/// @return 解析成功时返回 JSON；失败时返回 discarded 值。
nlohmann::json readExportedFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if ( !input.is_open() ) {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
    return nlohmann::json::parse(input, nullptr, false);
}

/// @brief 验证导出文件的格式标识、版本、方案名和颜色数据。
/// @param document 待验证 JSON。
/// @param expectedName 预期方案名。
/// @return 文件结构和内容均符合预期时返回 true。
bool validateExportedFile(const nlohmann::json& document,
                          const std::string&    expectedName)
{
    if ( document.is_discarded() || !document.is_object() ||
         !document.contains("format") || !document["format"].is_string() ||
         document["format"].get<std::string>() !=
             MMM::Config::COLOR_PALETTE_FILE_FORMAT ||
         !document.contains("version") ||
         !document["version"].is_number_unsigned() ||
         document["version"].get<std::uint32_t>() !=
             MMM::Config::COLOR_PALETTE_FILE_VERSION ||
         !document.contains("scheme") || !document["scheme"].is_object() ) {
        return false;
    }

    const auto& scheme = document["scheme"];
    if ( !scheme.contains("name") || !scheme["name"].is_string() ||
         scheme["name"].get<std::string>() != expectedName ||
         !scheme.contains("noteColors") || !scheme["noteColors"].is_array() ||
         scheme["noteColors"].size() !=
             MMM::Config::NOTE_COLOR_PALETTE_SLOT_COUNT ||
         !scheme.contains("beatLineColors") ||
         !scheme["beatLineColors"].is_array() ||
         scheme["beatLineColors"].size() !=
             MMM::Config::BEAT_LINE_COLOR_PALETTE_SLOT_COUNT ) {
        return false;
    }

    for ( const char* field : { "noteColors", "beatLineColors" } ) {
        for ( const auto& color : scheme[field] ) {
            if ( !color.is_array() || color.size() != 4 ) {
                return false;
            }
            for ( const auto& channel : color ) {
                if ( !channel.is_number() ) {
                    return false;
                }
            }
        }
    }
    return true;
}

/// @brief 验证配色方案导出及同一路径覆盖。
/// @param outputDirectory 测试输出目录。
/// @return 两次导出内容均正确时返回 true。
bool testExportAndOverwrite(const std::filesystem::path& outputDirectory)
{
    std::error_code createDirectoryError;
    std::filesystem::create_directories(outputDirectory, createDirectoryError);
    if ( createDirectoryError ) {
        return false;
    }

    const std::filesystem::path outputPath =
        outputDirectory / "test_palette.mmpalette";
    MMM::Config::ColorPaletteScheme scheme;
    scheme.name       = "Ocean";
    scheme.noteColors = {
        std::array<float, 4>{ 0.1f, 0.2f, 0.3f, 1.0f },
        std::array<float, 4>{ 0.2f, 0.3f, 0.4f, 1.0f },
        std::array<float, 4>{ 0.3f, 0.4f, 0.5f, 0.9f },
        std::array<float, 4>{ 0.4f, 0.5f, 0.6f, 0.8f },
        std::array<float, 4>{ 0.5f, 0.6f, 0.7f, 0.7f },
        std::array<float, 4>{ 0.6f, 0.7f, 0.8f, 0.6f },
    };
    scheme.beatLineColors = {
        std::array<float, 4>{ 0.9f, 0.1f, 0.1f, 1.0f },
        std::array<float, 4>{ 0.8f, 0.2f, 0.1f, 1.0f },
        std::array<float, 4>{ 0.7f, 0.3f, 0.1f, 1.0f },
        std::array<float, 4>{ 0.6f, 0.4f, 0.1f, 1.0f },
        std::array<float, 4>{ 0.5f, 0.5f, 0.1f, 1.0f },
        std::array<float, 4>{ 0.4f, 0.6f, 0.1f, 1.0f },
        std::array<float, 4>{ 0.3f, 0.7f, 0.1f, 1.0f },
        std::array<float, 4>{ 0.2f, 0.8f, 0.1f, 1.0f },
        std::array<float, 4>{ 0.1f, 0.9f, 0.1f, 1.0f },
    };

    MMM::Config::ColorPaletteScheme importedScheme;
    if ( !MMM::Config::exportColorPaletteFile(outputPath, scheme) ||
         !validateExportedFile(readExportedFile(outputPath), "Ocean") ||
         !MMM::Config::importColorPaletteFile(outputPath, importedScheme) ||
         importedScheme.name != scheme.name ||
         importedScheme.noteColors != scheme.noteColors ||
         importedScheme.beatLineColors != scheme.beatLineColors ) {
        return false;
    }

    scheme.name = "Sunset";
    return MMM::Config::exportColorPaletteFile(outputPath, scheme) &&
           validateExportedFile(readExportedFile(outputPath), "Sunset") &&
           MMM::Config::importColorPaletteFile(outputPath, importedScheme) &&
           importedScheme.name == "Sunset";
}

/// @brief 验证仓库中的 IVM 示例配色可由正式导入接口读取。
/// @param palettePath IVM 示例配色文件路径。
/// @return 文件完整有效且关键物件与整拍线颜色符合设计时返回 true。
bool testIvmExamplePalette(const std::filesystem::path& palettePath)
{
    MMM::Config::ColorPaletteScheme scheme;
    if ( !MMM::Config::importColorPaletteFile(palettePath, scheme) ||
         scheme.name != "IVM" ) {
        return false;
    }

    const auto& tapColor       = scheme.noteColors[0];
    const auto& holdColor      = scheme.noteColors[2];
    const auto& nodeColor      = scheme.noteColors[5];
    const auto& wholeBeatColor = scheme.beatLineColors[0];
    return tapColor[1] > 0.75f && tapColor[2] > 0.7f && holdColor[1] > 0.8f &&
           nodeColor == holdColor && wholeBeatColor[0] == 1.0f &&
           wholeBeatColor[1] < 0.2f;
}

}  // namespace

/// @brief 覆盖完整调色方案文件的序列化、写入和覆盖流程。
/// @param argc 参数数量。
/// @param argv 参数数组；测试参数为输出目录和 IVM 示例配色路径。
/// @return 所有检查通过时返回 0。
int main(int argc, char** argv)
{
    if ( argc < 3 || argv[1] == nullptr || argv[2] == nullptr ) {
        return 1;
    }
    return testExportAndOverwrite(std::filesystem::path(argv[1])) &&
                   testIvmExamplePalette(std::filesystem::path(argv[2]))
               ? 0
               : 1;
}
