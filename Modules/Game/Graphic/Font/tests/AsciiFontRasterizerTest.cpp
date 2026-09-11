#include "font/AsciiFontRasterizer.h"

#include "config/Utf8Path.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

/// @file
/// @brief 验证字体栅格化的路径兼容、度量完整性与字形缓冲区契约。
///
/// 测试使用项目内置字体作为稳定输入，并复制到含中文的临时路径，
/// 以覆盖不能直接交给 FreeType 窄路径 API 的文件系统场景。

namespace
{

/// @brief 验证时间文本需要的 ASCII 字形拥有可上传的 RGBA 位图。
/// @param font 已完成栅格化的字体。
/// @return 数字与时间分隔符均可用时返回 true。
bool hasTimeGlyphBitmaps(const MMM::Graphic::RasterizedAsciiFont& font)
{
    // 时间提示只依赖数字、冒号和小数点；这些字形缺失会直接影响画布读数。
    constexpr std::array<char, 12> requiredGlyphs{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.'
    };
    for ( const char character : requiredGlyphs ) {
        // 度量条目必须同时声明字形可用且确实包含可上传位图。
        const auto* metrics = font.metrics.glyph(character);
        if ( !metrics || !metrics->available || !metrics->hasBitmap ) {
            return false;
        }

        // 固定 ASCII 范围以首码点为零基索引映射到栅格化结果数组。
        const auto index =
            static_cast<std::uint32_t>(static_cast<unsigned char>(character)) -
            MMM::Common::ASCII_GLYPH_FIRST;
        const auto& glyph = font.glyphs[index];
        // RGBA8 每像素占四字节，尺寸与缓冲区长度必须严格一致。
        if ( glyph.width == 0U || glyph.height == 0U ||
             glyph.pixels.size() !=
                 static_cast<std::size_t>(glyph.width) * glyph.height * 4U ) {
            return false;
        }
    }
    return true;
}

/// @brief 在测试结束时清理精确的字体路径回归目录。
struct TestDirectoryCleanup {
    /// @brief 待清理目录。
    std::filesystem::path path;

    /// @brief 删除本测试创建的目录。
    ~TestDirectoryCleanup()
    {
        // 清理失败不覆盖原始测试结论，遗留目录只位于构建输出范围内。
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

}  // namespace

/// @brief 使用默认皮肤 ASCII 字体验证全部提示字号的独立 FreeType 栅格化结果。
/// @param argc 参数数量。
/// @param argv 依次提供 ASCII 字体、CJK 字体和测试输出目录的 UTF-8 路径。
/// @return 全部字体度量、时间字形位图和小字号选择有效时返回 0。
/// @details 返回码按输入、路径准备、ASCII 度量、档位选择和 Unicode
/// 字形阶段区分，
///          便于 CTest 在无异常机制的构建中定位失败边界。
/// @retval 1 调用参数不完整。
/// @retval 2 ASCII 字体或全局度量无效。
/// @retval 3 时间显示所需字形缺少有效位图。
/// @retval 4 范围外控制字符被错误接受。
/// @retval 5 栅格档位边界选择错误。
/// @retval 6 Unicode 稀疏字体结构或数量错误。
/// @retval 7 Unicode 字形度量或 RGBA 缓冲区错误。
/// @retval 8 无法创建隔离测试目录。
/// @retval 9 无法准备 ASCII 字体副本。
/// @retval 10 无法准备 Unicode 字体副本。
/// @note 测试不写入源码资源目录，临时文件在正常或提前返回时均由 RAII 清理。
int main(int argc, char** argv)
{
    // 测试必须由 CMake 传入两个真实字体和一个隔离输出根目录。
    if ( argc != 4 || !argv[1] || !argv[2] || !argv[3] ) {
        return 1;
    }

    // 使用中文路径验证 filesystem 原生路径读取，而不只验证 ASCII 工作目录。
    const auto unicodePathRoot = MMM::Config::utf8ToPath(argv[3]) /
                                 MMM::Config::utf8ToPath("字体栅格化测试") /
                                 MMM::Config::utf8ToPath("中文用户目录");
    TestDirectoryCleanup cleanup{ unicodePathRoot.parent_path() };
    std::error_code      fileError;
    // 所有临时数据写入构建目录；源码中的测试资源保持只读。
    std::filesystem::create_directories(unicodePathRoot, fileError);
    if ( fileError ) return 8;

    const auto fontPath =
        unicodePathRoot / MMM::Config::utf8ToPath("ASCII字体.ttf");
    // 两个目标文件名也包含非 ASCII 字符，覆盖最终文件组件的路径转换。
    const auto unicodeFontPath =
        unicodePathRoot / MMM::Config::utf8ToPath("中文字体.otf");
    // 复制而非重命名测试资源，确保源字体可供其他测试重复使用。
    std::filesystem::copy_file(
        MMM::Config::utf8ToPath(argv[1]),
        fontPath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    if ( fileError ) return 9;
    // 复用 error_code 前由成功的 copy_file 清空状态，后续错误码对应 CJK 副本。
    std::filesystem::copy_file(
        MMM::Config::utf8ToPath(argv[2]),
        unicodeFontPath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    if ( fileError ) return 10;

    MMM::Common::AsciiFontAtlasMetrics atlas;
    // 每个预定义栅格档位都单独执行 FreeType 流程，覆盖实际资源加载方式。
    for ( std::size_t tierIndex = 0U;
          tierIndex < MMM::Common::ASCII_FONT_RASTER_TIER_COUNT;
          ++tierIndex ) {
        const auto rasterized = MMM::Graphic::AsciiFontRasterizer::rasterize(
            fontPath, MMM::Common::ASCII_FONT_RASTER_HEIGHTS[tierIndex]);
        // 全局垂直度量必须有效，否则渲染无法计算基线与行高。
        if ( !rasterized || !rasterized->metrics.valid ||
             rasterized->metrics.lineHeight <= 0.0f ||
             rasterized->metrics.ascender <= 0.0f ) {
            return 2;
        }
        // 对画布时间文本所需的最小字形集合进行像素级结构检查。
        if ( !hasTimeGlyphBitmaps(*rasterized) ) {
            return 3;
        }
        // 范围外控制字符不得误映射到固定 ASCII 数组。
        if ( rasterized->metrics.glyph('\x01') != nullptr ) {
            return 4;
        }
        atlas.tiers[tierIndex] = rasterized->metrics;
        // 只保存轻量度量；像素缓冲在当前循环结束后即可释放。
    }
    // 只有全部档位验证并写入后，聚合字体图集才可标为有效。
    atlas.valid = true;

    // 请求恰好等于各档位高度时应选择同索引档位，防止阈值边界偏移。
    for ( std::size_t tierIndex = 0U; tierIndex < 5U; ++tierIndex ) {
        const auto selected = MMM::Common::selectAsciiFont(
            atlas,
            static_cast<float>(
                MMM::Common::ASCII_FONT_RASTER_HEIGHTS[tierIndex]));
        if ( !selected || selected.tierIndex != tierIndex ) {
            return 5;
        }
        // selected 观察 atlas 中的现有档位，不在循环间保留其引用。
    }

    // 码点保持严格递增，覆盖日文假名与常用中文而不混入固定 ASCII 范围。
    constexpr std::array<std::uint32_t, 10> cjkCodepoints{
        0x3074U, 0x3087U, 0x3093U, 0x30AFU, 0x30C6U,
        0x30C8U, 0x30DFU, 0x521DU, 0x91CDU, 0x97F3U
    };
    const auto unicodeFont =
        MMM::Graphic::AsciiFontRasterizer::rasterizeUnicode(
            unicodeFontPath, std::span<const std::uint32_t>(cjkCodepoints));
    // 稀疏度量和位图数组必须与全部受支持的输入码点保持同样长度。
    if ( !unicodeFont || !unicodeFont->metrics.valid ||
         unicodeFont->metrics.glyphs.size() != cjkCodepoints.size() ||
         unicodeFont->glyphs.size() != cjkCodepoints.size() ) {
        return 6;
    }
    // 逐项核对查找结果和 RGBA 缓冲区，验证码点到紧凑索引的对应关系。
    for ( std::size_t index = 0U; index < cjkCodepoints.size(); ++index ) {
        // 查找 API 按码点而非数组位置调用，验证对外查询契约。
        const auto* metrics = unicodeFont->metrics.glyph(cjkCodepoints[index]);
        const auto& glyph   = unicodeFont->glyphs[index];
        // 测试字体应覆盖全部探测码点，因此任一缺失或空位图都视为回归。
        if ( !metrics || !metrics->available || !metrics->hasBitmap ||
             glyph.width == 0U || glyph.height == 0U ||
             glyph.pixels.size() !=
                 static_cast<std::size_t>(glyph.width) * glyph.height * 4U ) {
            return 7;
        }
    }
    // 清理对象在返回后移除本次创建的中文临时目录。
    return 0;
}
