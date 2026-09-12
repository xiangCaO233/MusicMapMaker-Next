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
/// @warning 仅读取构建目录中的测试输出，不访问用户调色方案目录。
nlohmann::json readExportedFile(const std::filesystem::path& path)
{
    // 使用二进制流与正式导入入口保持平台换行处理一致。
    std::ifstream input(path, std::ios::binary);
    if ( !input.is_open() ) {
        // discarded 值让调用方沿用同一结构校验路径处理读取失败。
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
    // 禁用解析异常，损坏导出文件以值状态表示测试失败。
    return nlohmann::json::parse(input, nullptr, false);
}

/// @brief 验证导出文件的格式标识、版本、方案名和颜色数据。
/// @param document 待验证 JSON。
/// @param expectedName 预期方案名。
/// @return 文件结构和内容均符合预期时返回 true。
/// @details 与正式导入器独立检查磁盘文档，避免仅验证读写实现自洽。
bool validateExportedFile(const nlohmann::json& document,
                          const std::string&    expectedName)
{
    // 先验证顶层格式名、版本和 scheme 对象，再访问内部字段。
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

    // 方案名称和两类颜色数组长度必须与公开槽位常量一致。
    const auto& scheme = document["scheme"];
    // expectedName 在覆盖测试中分别传入 Ocean 和 Sunset。
    if ( !scheme.contains("name") || !scheme["name"].is_string() ||
         scheme["name"].get<std::string>() != expectedName ||
         !scheme.contains("noteColors") || !scheme["noteColors"].is_array() ||
         scheme["noteColors"].size() !=
             MMM::Config::NOTE_COLOR_PALETTE_SLOT_COUNT ||
         !scheme.contains("beatLineColors") ||
         !scheme["beatLineColors"].is_array() ||
         scheme["beatLineColors"].size() !=
             MMM::Config::BEAT_LINE_COLOR_PALETTE_SLOT_COUNT ) {
        // 名称和数组尺寸任一错误都表明导出文件不符合分享协议。
        return false;
    }

    // 两个字段采用相同 RGBA 结构，循环避免测试规则在两处漂移。
    for ( const char* field : { "noteColors", "beatLineColors" } ) {
        // 外层已确认数组，逐个颜色验证四通道和数值类型。
        for ( const auto& color : scheme[field] ) {
            if ( !color.is_array() || color.size() != 4 ) {
                // 每个槽位必须完整保存 RGBA，不能省略默认 Alpha。
                return false;
            }
            for ( const auto& channel : color ) {
                // 正式导入还验证范围；本辅助函数专注导出 JSON 类型与结构。
                if ( !channel.is_number() ) {
                    return false;
                }
            }
        }
    }
    // 所有头字段、数组尺寸和通道类型通过后才确认导出结构有效。
    return true;
}

/// @brief 验证配色方案导出及同一路径覆盖。
/// @param outputDirectory 测试输出目录。
/// @return 两次导出内容均正确时返回 true。
/// @warning 只在调用方传入的测试输出目录创建和覆盖文件。
bool testExportAndOverwrite(const std::filesystem::path& outputDirectory)
{
    // 输出目录位于构建测试区域，允许用例重复覆盖同一文件。
    std::error_code createDirectoryError;
    std::filesystem::create_directories(outputDirectory, createDirectoryError);
    if ( createDirectoryError ) {
        // 基础目录不可用时没有可靠目标，直接报告夹具失败。
        return false;
    }

    // 固定扩展名覆盖正式分享文件路径，不依赖导出函数自动补后缀。
    const std::filesystem::path outputPath =
        outputDirectory / "test_palette.mmpalette";
    MMM::Config::ColorPaletteScheme scheme;
    // 名称用于区分首次写入和覆盖后的第二份文档。
    scheme.name = "Ocean";
    // 六个物件槽位使用不同 RGB 与 Alpha，验证完整数组无损往返。
    scheme.noteColors = {
        std::array<float, 4>{ 0.1f, 0.2f, 0.3f, 1.0f },
        std::array<float, 4>{ 0.2f, 0.3f, 0.4f, 1.0f },
        std::array<float, 4>{ 0.3f, 0.4f, 0.5f, 0.9f },
        std::array<float, 4>{ 0.4f, 0.5f, 0.6f, 0.8f },
        std::array<float, 4>{ 0.5f, 0.6f, 0.7f, 0.7f },
        std::array<float, 4>{ 0.6f, 0.7f, 0.8f, 0.6f },
    };
    // 九个分拍线槽位全部显式赋值，覆盖常用分母和默认回退色。
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

    // 输出参数初始为空，成功导入后必须与源方案逐字段相等。
    MMM::Config::ColorPaletteScheme importedScheme;
    // 同一输出对象在两轮导入间复用，可发现导入没有完整覆盖旧值的问题。
    // 首轮同时验证写出结构、正式导入和两类颜色的精确往返。
    if ( !MMM::Config::exportColorPaletteFile(outputPath, scheme) ||
         !validateExportedFile(readExportedFile(outputPath), "Ocean") ||
         !MMM::Config::importColorPaletteFile(outputPath, importedScheme) ||
         importedScheme.name != scheme.name ||
         importedScheme.noteColors != scheme.noteColors ||
         importedScheme.beatLineColors != scheme.beatLineColors ) {
        // 任一阶段失败即说明完整导出链路不满足契约。
        return false;
    }

    // 第二轮只改变名称，用同一路径验证替换而非追加旧内容。
    scheme.name = "Sunset";
    // 覆盖后重新读取磁盘与正式导入，两个观察入口都必须看到新名称。
    return MMM::Config::exportColorPaletteFile(outputPath, scheme) &&
           validateExportedFile(readExportedFile(outputPath), "Sunset") &&
           MMM::Config::importColorPaletteFile(outputPath, importedScheme) &&
           importedScheme.name == "Sunset";
}

/// @brief 验证仓库中的 IVM 示例配色可由正式导入接口读取。
/// @param palettePath IVM 示例配色文件路径。
/// @return 文件完整有效且关键物件与整拍线颜色符合设计时返回 true。
/// @details 只读示例资源，不在源码数据目录生成任何输出。
bool testIvmExamplePalette(const std::filesystem::path& palettePath)
{
    // 示例文件由仓库资源提供，验证用户实际可分享格式而非仅自生成 JSON。
    MMM::Config::ColorPaletteScheme scheme;
    if ( !MMM::Config::importColorPaletteFile(palettePath, scheme) ||
         scheme.name != "IVM" ) {
        // 导入失败或名称错误都表示示例资源与当前格式实现不同步。
        return false;
    }

    // 选择 Tap、Hold、Polyline 节点和整拍线作为设计关键色抽样。
    const auto& tapColor = scheme.noteColors[0];
    // Hold 与 Polyline 节点应共享绿色系，抽样比较两槽完全相等。
    const auto& holdColor = scheme.noteColors[2];
    const auto& nodeColor = scheme.noteColors[5];
    // 整拍线读取第一个固定槽位，与 BeatLinePalette 的公开映射一致。
    const auto& wholeBeatColor = scheme.beatLineColors[0];
    // 断言颜色关系而非全部浮点常量，使测试表达 IVM 的可见设计约束。
    return tapColor[1] > 0.75f && tapColor[2] > 0.7f && holdColor[1] > 0.8f &&
           nodeColor == holdColor && wholeBeatColor[0] == 1.0f &&
           wholeBeatColor[1] < 0.2f;
}

}  // namespace

/// @brief 覆盖完整调色方案文件的序列化、写入和覆盖流程。
/// @param argc 参数数量。
/// @param argv 参数数组；测试参数为输出目录和 IVM 示例配色路径。
/// @return 所有检查通过时返回 0。
/// @warning 输出目录必须由 CTest 指向构建树，示例路径则保持只读。
int main(int argc, char** argv)
{
    // CMake 必须传入独立输出目录和仓库中的 IVM 示例文件。
    if ( argc < 3 || argv[1] == nullptr || argv[2] == nullptr ) {
        // 缺少任一夹具路径时无法区分写入与示例读取测试。
        return 1;
    }
    // 先验证可写输出链路，再读取只读示例资源，失败阶段由返回值区分范围。
    return testExportAndOverwrite(std::filesystem::path(argv[1])) &&
                   testIvmExamplePalette(std::filesystem::path(argv[2]))
               ? 0
               : 1;
}
