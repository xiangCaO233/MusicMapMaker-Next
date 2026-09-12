#include "config/ColorPaletteFile.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

namespace
{

/// @brief 从 JSON 数组读取固定数量的 RGBA 颜色。
/// @tparam ColorCount 颜色数量。
/// @param value 输入 JSON 数组。
/// @param colors 输出颜色数组。
/// @return 数组结构完整且所有通道位于 0 到 1 时返回 true。
/// @details 输出数组可能在失败前写入前缀，调用方必须使用局部候选对象。
template<std::size_t ColorCount>
bool readColorArray(const nlohmann::json&                         value,
                    std::array<std::array<float, 4>, ColorCount>& colors)
{
    // 外层长度属于格式契约，不能接受缺项或额外颜色后再静默裁剪。
    if ( !value.is_array() || value.size() != ColorCount ) {
        // 类型与数量一起检查，避免后续按索引访问越界。
        return false;
    }
    // 逐槽读取到调用方提供的固定数组，不在校验路径执行动态扩容。
    for ( std::size_t colorIndex = 0; colorIndex < ColorCount; ++colorIndex ) {
        const auto& color = value[colorIndex];
        // 每种颜色必须恰好提供 RGBA 四通道，RGB 简写不在文件协议内。
        if ( !color.is_array() || color.size() != 4 ) {
            // 不接受对象形式或三通道颜色，保持共享文件格式单一。
            return false;
        }
        // 通道独立验证，任一错误都会使整套方案保持未导入状态。
        for ( std::size_t channelIndex = 0; channelIndex < 4; ++channelIndex ) {
            // JSON 整数和浮点均属于 number，字符串数值不做隐式转换。
            if ( !color[channelIndex].is_number() ) {
                // 布尔值和 null 不属于颜色通道，即使 JSON 库可做某些转换。
                return false;
            }
            const double channel = color[channelIndex].get<double>();
            // 拒绝 NaN、无穷和范围外值，避免异常颜色进入渲染计算。
            if ( !std::isfinite(channel) || channel < 0.0 || channel > 1.0 ) {
                // 通道范围与渲染配置约定一致，导入层不执行自动钳制。
                return false;
            }
            // 只有当前通道完整通过校验后才缩窄为项目使用的 float。
            colors[colorIndex][channelIndex] = static_cast<float>(channel);
        }
    }
    // 所有槽位完整验证并写入后才确认数组成功。
    return true;
}

}  // namespace

namespace MMM::Config
{

/// @brief 将完整调色方案以带格式头的 JSON 文件安全写入目标路径。
/// @param path 最终 `.mmpalette` 文件路径。
/// @param scheme 需要导出的方案名称、物件色和分拍线色。
/// @return 临时写入与最终替换全部成功时返回 true。
/// @warning 低频文件系统路径：会创建目录、写文件并替换现有目标。
bool exportColorPaletteFile(const std::filesystem::path& path,
                            const ColorPaletteScheme&    scheme)
{
    // 空路径没有可恢复目标，禁止退化为当前工作目录中的临时文件。
    if ( path.empty() ) {
        XERROR("Cannot export color palette to an empty path");
        return false;
    }

    // 仅在目标含父目录时创建层级，简单文件名保持相对路径语义。
    const std::filesystem::path parent = path.parent_path();
    if ( !parent.empty() ) {
        std::error_code createDirectoryError;
        // error_code 版本把权限等错误转为返回值，不传播文件系统异常。
        std::filesystem::create_directories(parent, createDirectoryError);
        if ( createDirectoryError ) {
            // 目录不可用时尚未创建临时文件，原目标不会被修改。
            XERROR(
                "Failed to create color palette export directory: {}. "
                "Error: {}",
                pathToUtf8(parent),
                createDirectoryError.message());
            return false;
        }
        // create_directories 对已存在目录无副作用，重复导出可复用同一位置。
    }

    // 复用 ColorPaletteFile 的 JSON 适配器，格式标识和版本由一处维护。
    ColorPaletteFile paletteFile;
    // 默认版本字段随结构体初始化，导出函数不允许调用方指定旧版本。
    paletteFile.scheme            = scheme;
    const nlohmann::json document = paletteFile;

    // 临时文件与目标位于同一目录，通常允许原子 rename 完成替换。
    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp";
    // 写入结果跨文件流作用域保存，确保 close 后再决定是否替换目标。
    bool writeSucceeded = false;
    {
        // 二进制截断避免平台换行转换，并覆盖可能存在的旧临时内容。
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if ( !output.is_open() ) {
            // 正式目标尚未触碰，打开临时文件失败不会破坏旧方案。
            XERROR("Failed to open color palette export file: {}",
                   pathToUtf8(temporaryPath));
            return false;
        }

        // 四空格缩进便于人工分享和审查，末尾换行保持文本工具兼容。
        output << std::setw(4) << document << '\n';
        // 流关闭前记录完整写入状态，析构随后刷新并释放文件句柄。
        writeSucceeded = output.good();
    }

    if ( !writeSucceeded ) {
        // 部分临时文件不得留作下次导入候选，失败后尽力清理。
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        // 清理错误不取代原始写入失败，调用方仍得到统一 false。
        XERROR("Failed to write color palette export file: {}",
               pathToUtf8(temporaryPath));
        return false;
    }

    // 首选同目录重命名，成功时读者只会看到旧文件或完整新文件。
    std::error_code replaceError;
    std::filesystem::rename(temporaryPath, path, replaceError);
    if ( replaceError ) {
        // Windows 可能拒绝 rename 覆盖现有目标，退化为显式覆盖复制。
        std::error_code copyError;
        std::filesystem::copy_file(
            temporaryPath,
            path,
            std::filesystem::copy_options::overwrite_existing,
            copyError);
        // 复制后无论成功失败都移除临时文件，避免残留被误当正式方案。
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        if ( copyError ) {
            // 原目标可能仍保持旧内容，调用方根据 false 不更新当前方案。
            XERROR(
                "Failed to replace color palette export file: {}. "
                "Error: {}",
                pathToUtf8(path),
                copyError.message());
            return false;
        }
        // 覆盖复制成功后正式目标已完整，rename 的原始错误无需继续上报。
    }

    // 仅最终目标已完整可用时记录成功，日志路径统一转为 UTF-8。
    XINFO("Color palette exported: {}", pathToUtf8(path));
    return true;
}

/// @brief 从独立 JSON 文件严格校验并导入完整调色方案。
/// @param path 待读取的 `.mmpalette` 路径。
/// @param scheme 仅在全部校验通过后接收新方案。
/// @return 文件头、方案结构和所有 RGBA 通道有效时返回 true。
/// @warning 低频文件系统路径：读取并解析整个方案文件。
bool importColorPaletteFile(const std::filesystem::path& path,
                            ColorPaletteScheme&          scheme)
{
    // 空路径单独报告，避免把打开失败误解为某个真实文件不可读。
    if ( path.empty() ) {
        XERROR("Cannot import color palette from an empty path");
        return false;
    }

    // 输入流使用二进制模式，JSON 解析器统一处理所有平台换行。
    std::ifstream input(path, std::ios::binary);
    if ( !input.is_open() ) {
        // 输出参数保持调用前状态，便于 UI 继续使用当前调色方案。
        XERROR("Failed to open color palette import file: {}",
               pathToUtf8(path));
        return false;
    }

    // 禁用异常解析；语法错误通过 discarded 值进入统一文件头校验。
    const nlohmann::json document =
        nlohmann::json::parse(input, nullptr, false);
    // 顶层判断按短路顺序排列，只有存在且类型正确后才执行 get。
    if ( document.is_discarded() || !document.is_object() ||
         !document.contains("format") || !document["format"].is_string() ||
         document["format"].get<std::string>() != COLOR_PALETTE_FILE_FORMAT ||
         !document.contains("version") ||
         !document["version"].is_number_unsigned() ||
         document["version"].get<std::uint32_t>() !=
             COLOR_PALETTE_FILE_VERSION ||
         !document.contains("scheme") || !document["scheme"].is_object() ) {
        // 格式名和版本必须精确匹配，未来版本不能被旧程序猜测读取。
        XERROR("Invalid color palette file header: {}", pathToUtf8(path));
        // 旧版或未来版本文件都在这里拒绝，不进入不兼容的字段解析。
        return false;
    }

    // 文件头通过后再检查方案必需字段，错误分类更便于用户定位。
    const auto& storedScheme = document["scheme"];
    // 名称和两组数组都是当前版本的必需字段，不提供隐式默认方案。
    if ( !storedScheme.contains("name") || !storedScheme["name"].is_string() ||
         !storedScheme.contains("noteColors") ||
         !storedScheme.contains("beatLineColors") ) {
        // 颜色字段的具体数组结构交给固定尺寸模板统一验证。
        XERROR("Incomplete color palette scheme: {}", pathToUtf8(path));
        // 名称或任一颜色组缺失时不创建可见的部分方案。
        return false;
    }

    // 在局部对象中构建候选方案，失败不会部分覆盖调用方当前配置。
    ColorPaletteScheme importedScheme;
    // 名称已确认是字符串，可以无异常地按既定类型读取。
    importedScheme.name = storedScheme["name"].get<std::string>();
    // 物件色和分拍线色分别使用各自编译期数组长度实例化模板。
    if ( !readColorArray(storedScheme["noteColors"],
                         importedScheme.noteColors) ||
         !readColorArray(storedScheme["beatLineColors"],
                         importedScheme.beatLineColors) ) {
        // 两组数组必须同时有效，不能导入只含物件色或分拍线色的半成品。
        XERROR("Invalid color palette colors: {}", pathToUtf8(path));
        // importedScheme 是局部值，失败返回后自动丢弃已写入的颜色前缀。
        return false;
    }

    // 完整候选通过后一次移动提交，维持输出参数的强失败不变语义。
    scheme = std::move(importedScheme);
    // 成功日志只在输出参数已经更新后产生，状态与诊断顺序一致。
    XINFO("Color palette imported: {}", pathToUtf8(path));
    return true;
}

}  // namespace MMM::Config
