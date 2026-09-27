#include "font/AsciiFontRasterizer.h"

#include "log/colorful-log.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

/// @file
/// @brief 实现字体文件读取、FreeType 生命周期管理和 CPU 侧 RGBA 字形栅格化。
///
/// 本实现刻意不接触 Vulkan：输出保持为普通字节数组和归一化度量，
/// 由上层图集在合适的图形线程中决定上传时机。字体文件先完整读入内存，
/// 可避免 FreeType 的窄路径接口在 Windows 上丢失 Unicode 路径信息。

namespace MMM::Graphic
{

namespace
{

/// @brief 释放 FreeType 库对象。
struct FreeTypeLibraryReleaser {
    /// @brief 释放 FreeType 库对象。
    void operator()(std::remove_pointer_t<FT_Library>* library) const
    {
        // 空指针表示初始化未完成，符合 unique_ptr 删除器的幂等清理约定。
        if ( library ) FT_Done_FreeType(library);
    }
};

/// @brief 释放 FreeType 字体对象。
struct FreeTypeFaceReleaser {
    /// @brief 释放 FreeType 字体对象。
    void operator()(std::remove_pointer_t<FT_Face>* face) const
    {
        // Face 必须在其所属 Library
        // 之前释放；调用方通过局部变量顺序维持该约束。
        if ( face ) FT_Done_Face(face);
    }
};

/// @brief 独占持有单次栅格化调用使用的 FreeType Library。
using UniqueFreeTypeLibrary =
    std::unique_ptr<std::remove_pointer_t<FT_Library>, FreeTypeLibraryReleaser>;
/// @brief 独占持有一个 FreeType Face，并在作用域结束时自动释放。
using UniqueFreeTypeFace =
    std::unique_ptr<std::remove_pointer_t<FT_Face>, FreeTypeFaceReleaser>;

/// @brief 将文件系统路径转换为 FreeType 可读取的 UTF-8 字符串。
/// @param path 字体路径。
/// @return UTF-8 路径。
/// @note 该字符串仅用于日志；实际字体加载使用文件字节，避免窄路径兼容问题。
std::string fontPathUtf8(const std::filesystem::path& path)
{
    // u8string 明确取得 UTF-8 字节，再按相同长度复制为日志库接受的 char
    // 字符串。
    const auto value = path.u8string();
    return { reinterpret_cast<const char*>(value.data()), value.size() };
}

/// @brief 保持字体文件字节在 FreeType Face 生命周期内有效。
struct LoadedFreeTypeFace {
    /// @brief 通过原生文件系统路径读取的完整字体文件。
    /// @note Face 通过内存视图引用该容器，容器地址在对象生命周期内不得变化。
    std::vector<FT_Byte> bytes;
    /// @brief 引用 bytes 的 FreeType Face；成员逆序析构确保先释放 Face。
    UniqueFreeTypeFace face;
};

/// @brief 通过 std::filesystem 原生路径读取字体并创建内存 Face。
/// @param library 已初始化的 FreeType 库。
/// @param fontPath 字体文件路径。
/// @return 文件完整可读且 FreeType 可以解析时返回持有对象。
/// @details 文件大小必须同时能由 `FT_Long`
/// 和流接口完整表示；任何短读都视为失败，
///          不把部分字体数据交给 FreeType。返回对象把文件字节放在 Face
///          之前声明， 以便析构时先销毁借用字节的 Face，再回收底层存储。
/// @warning 会读取完整字体文件并分配等量内存，只能用于低频资源准备路径。
std::optional<LoadedFreeTypeFace> loadFreeTypeFace(
    FT_Library library, const std::filesystem::path& fontPath)
{
    // 带 error_code 的查询不会抛出文件系统异常，失败统一映射为空结果。
    std::error_code fileSizeError;
    const auto fileSize = std::filesystem::file_size(fontPath, fileSizeError);
    // FreeType 的内存 Face 长度使用 FT_Long，流读取长度还受 streamsize 限制。
    if ( fileSizeError || fileSize == 0U ||
         fileSize >
             static_cast<std::uintmax_t>(std::numeric_limits<FT_Long>::max()) ||
         fileSize > static_cast<std::uintmax_t>(
                        std::numeric_limits<std::streamsize>::max()) ) {
        return std::nullopt;
    }

    std::vector<FT_Byte> bytes(static_cast<std::size_t>(fileSize));
    std::ifstream        input(fontPath, std::ios::binary);
    if ( !input ) return std::nullopt;
    // 一次性读取能保证 Face 后续观察到的是完整且连续的字体文件镜像。
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    // 同时检查流状态和实际字节数，拒绝读取过程中被截断或替换的文件。
    if ( !input ||
         input.gcount() != static_cast<std::streamsize>(bytes.size()) ) {
        return std::nullopt;
    }

    FT_Face rawFace = nullptr;
    // Face 直接借用 bytes；只有构造成功后才将二者一起移动进持有对象。
    if ( FT_New_Memory_Face(library,
                            bytes.data(),
                            static_cast<FT_Long>(bytes.size()),
                            0,
                            &rawFace) != 0 ||
         !rawFace ) {
        return std::nullopt;
    }
    // 聚合成员的声明顺序保证 face 析构早于 bytes。
    return LoadedFreeTypeFace{ std::move(bytes), UniqueFreeTypeFace(rawFace) };
}

/// @brief 从 FreeType 灰度或单色位图取得覆盖率。
/// @param bitmap 字形位图。
/// @param x 像素横坐标。
/// @param y 像素纵坐标。
/// @return 0 至 255 的覆盖率。
/// @pre `x`、`y` 位于 FreeType 返回的位图范围内。
/// @details FreeType 允许正负 pitch；负值表示首地址对应逻辑末行。
///          灰度位图直接读取覆盖率，单色位图按最高位优先展开。
unsigned char bitmapCoverage(const FT_Bitmap& bitmap, unsigned int x,
                             unsigned int y)
{
    const int pitch = bitmap.pitch;
    // 将两种行存储方向统一映射到从上到下的逻辑 y 坐标。
    const auto* row =
        pitch >= 0
            ? bitmap.buffer + static_cast<int>(y) * pitch
            : bitmap.buffer + static_cast<int>(bitmap.rows - 1U - y) * -pitch;
    if ( bitmap.pixel_mode == FT_PIXEL_MODE_GRAY ) {
        // FT_LOAD_TARGET_NORMAL 的常规结果已使用 8 位灰度覆盖率。
        return row[x];
    }
    if ( bitmap.pixel_mode == FT_PIXEL_MODE_MONO ) {
        // 单色位图每字节覆盖八个像素，最高位对应组内最左像素。
        return (row[x / 8U] & (0x80U >> (x % 8U))) != 0U ? 255U : 0U;
    }
    // 未支持的像素格式按透明处理，调用方会优先把字形标为无位图。
    return 0U;
}

/// @brief 将当前 FreeType 字形槽转换为归一化度量与 RGBA 位图。
/// @param slot 已加载的 FreeType 字形槽。
/// @param inverseHeight 标准栅格高度倒数。
/// @param metrics 接收归一化字形度量。
/// @param glyph 接收 RGBA 字形位图。
/// @details 排版度量除以请求像素高度后与实际图集分辨率解耦；像素缓冲则保留
///          原始尺寸，供上传阶段按选择的栅格档位创建图集。无轮廓的空格仍是
///          可用字形，但 `hasBitmap` 为 false，调用方应只使用其 advance。
/// @pre `metrics` 与 `glyph` 是当前字形专用的可写槽位。
void copyLoadedGlyph(FT_GlyphSlot slot, float inverseHeight,
                     Common::AsciiGlyphMetrics& metrics,
                     RasterizedAsciiGlyph&      glyph)
{
    const FT_Bitmap& bitmap = slot->bitmap;
    // 成功加载的字形即具有排版意义，位图存在性另行记录以兼容空格。
    metrics.available = true;
    metrics.hasBitmap = bitmap.width > 0U && bitmap.rows > 0U;
    metrics.width     = static_cast<float>(bitmap.width) * inverseHeight;
    metrics.height    = static_cast<float>(bitmap.rows) * inverseHeight;
    // bearing 描述位图相对基线原点的偏移，不能由位图尺寸自行推导。
    metrics.bearingX = static_cast<float>(slot->bitmap_left) * inverseHeight;
    metrics.bearingY = static_cast<float>(slot->bitmap_top) * inverseHeight;
    metrics.advanceX =
        static_cast<float>(slot->advance.x) / 64.0F * inverseHeight;

    // 没有像素的字形无需分配缓冲区，但前面写入的 advance 仍然有效。
    if ( !metrics.hasBitmap ) return;
    // 仅支持能够无损转换为单通道覆盖率的 FreeType 位图格式。
    if ( bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
         bitmap.pixel_mode != FT_PIXEL_MODE_MONO ) {
        // 保留 advance 等排版信息，但阻止上层把未知格式当作 RGBA 数据上传。
        metrics.hasBitmap = false;
        return;
    }

    glyph.width  = bitmap.width;
    glyph.height = bitmap.rows;
    // 目标图集统一使用白色 RGB 与覆盖率 Alpha，着色留给渲染管线完成。
    glyph.pixels.resize(static_cast<std::size_t>(glyph.width) *
                        static_cast<std::size_t>(glyph.height) * 4U);
    // 按逻辑行列展开 FreeType 位图，避免把 pitch 或单色位打包传播到上层。
    for ( std::uint32_t y = 0; y < glyph.height; ++y ) {
        for ( std::uint32_t x = 0; x < glyph.width; ++x ) {
            const auto alpha = bitmapCoverage(bitmap, x, y);
            const auto offset =
                (static_cast<std::size_t>(y) * glyph.width + x) * 4U;
            // 预乘和颜色混合由 Vulkan 管线控制，CPU 数据保持非预乘白色。
            glyph.pixels[offset + 0U] = 255U;
            glyph.pixels[offset + 1U] = 255U;
            glyph.pixels[offset + 2U] = 255U;
            glyph.pixels[offset + 3U] = alpha;
        }
    }
}

}  // namespace

/// @brief 栅格化固定可打印 ASCII 范围并生成归一化排版度量。
/// @param fontPath 字体文件路径，允许包含平台原生 Unicode 字符。
/// @param pixelHeight FreeType 请求的像素高度，必须大于零。
/// @return 初始化、文件读取或像素尺寸设置失败时返回空；成功时返回固定槽位字体。
/// @details 缺失的单个 ASCII 字形不会使整个字体失败，其对应槽位保持不可用，
///          由消费者选择回退显示。函数只执行 CPU
///          与文件操作，应在资源加载路径调用。
/// @warning 会同步读取文件并逐字符调用 FreeType，不得从渲染热路径调用。
std::optional<RasterizedAsciiFont> AsciiFontRasterizer::rasterize(
    const std::filesystem::path& fontPath, std::uint32_t pixelHeight)
{
    // 零高度无法形成可归一化的字体度量，也会导致后续除零。
    if ( pixelHeight == 0U ) {
        XERROR("ASCII font raster height must be greater than zero");
        return std::nullopt;
    }

    FT_Library rawLibrary = nullptr;
    // 每次调用使用独立 Library，使栅格化过程不依赖共享 FreeType 状态。
    if ( FT_Init_FreeType(&rawLibrary) != 0 || !rawLibrary ) {
        XERROR("Failed to initialize FreeType for ASCII font atlas");
        return std::nullopt;
    }
    // RAII 覆盖本函数之后所有提前返回路径。
    UniqueFreeTypeLibrary library(rawLibrary);

    const std::string pathUtf8 = fontPathUtf8(fontPath);
    // 通过原生 filesystem 读取字节，再由内存 Face 解析 Unicode 路径字体。
    auto loadedFace = loadFreeTypeFace(library.get(), fontPath);
    if ( !loadedFace ) {
        XERROR("Failed to load ASCII font: {}", pathUtf8);
        return std::nullopt;
    }
    auto& face = loadedFace->face;

    // 宽度参数为零表示由 FreeType 按字体纵向高度保持原始宽高关系。
    if ( FT_Set_Pixel_Sizes(face.get(), 0U, pixelHeight) != 0 ) {
        XERROR("Failed to set ASCII font raster size: {}", pathUtf8);
        return std::nullopt;
    }

    RasterizedAsciiFont result;
    const float         inverseHeight = 1.0f / static_cast<float>(pixelHeight);
    // 全局度量与单字形度量采用相同归一化尺度，渲染时可统一乘字号。
    result.metrics.valid    = true;
    result.metrics.ascender = static_cast<float>(face->size->metrics.ascender) /
                              64.0f * inverseHeight;
    // FreeType 的全局度量使用 26.6 定点数，先除以 64 再按像素高度归一化。
    result.metrics.lineHeight =
        std::max(1.0f, static_cast<float>(face->size->metrics.height) / 64.0f) *
        inverseHeight;

    // 固定范围与结果数组索引一一对应，避免在绘制热路径执行哈希查询。
    for ( std::uint32_t code = Common::ASCII_GLYPH_FIRST;
          code <= Common::ASCII_GLYPH_LAST;
          ++code ) {
        const std::size_t index = code - Common::ASCII_GLYPH_FIRST;
        // 单个字形缺失时保留默认空槽，字体其余字形仍可继续使用。
        if ( FT_Load_Char(face.get(),
                          code,
                          FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0 ) {
            continue;
        }

        // FreeType glyph slot 会被下一次加载覆盖，因此必须在循环内立即复制。
        // 数组槽位由码点直接定位，缺失字形不会改变其他字符的索引。
        copyLoadedGlyph(face->glyph,
                        inverseHeight,
                        result.metrics.glyphs[index],
                        result.glyphs[index]);
    }

    XDEBUG("Rasterized ASCII font atlas source: {} ({} px)",
           pathUtf8,
           pixelHeight);
    // 即使个别字符缺失，成功初始化的字体仍返回并由上层检查所需字形。
    return result;
}

/// @brief 按需栅格化非 ASCII Unicode 码点并生成稀疏字体资源。
/// @param fontPath 字体文件路径。
/// @param codepoints 严格递增且已去重的候选码点集合。
/// @param pixelHeight FreeType 请求的像素高度，必须大于零。
/// @return 字体初始化失败时返回空；成功时仅包含字体实际支持的有效码点。
/// @details 输入过滤会排除非法 Unicode、ASCII 范围和非递增项。输出 metrics 与
/// glyphs
///          始终同步追加，二者索引保持一一对应，供后续增量图集上传使用。
/// @warning 会同步读取字体并按候选码点栅格化，只适合字体缓存重建路径。
std::optional<RasterizedUnicodeFont> AsciiFontRasterizer::rasterizeUnicode(
    const std::filesystem::path&   fontPath,
    std::span<const std::uint32_t> codepoints, std::uint32_t pixelHeight)
{
    // 与固定 ASCII 路径保持相同高度契约，避免归一化度量出现无穷值。
    if ( pixelHeight == 0U ) {
        XERROR("Unicode font raster height must be greater than zero");
        return std::nullopt;
    }

    FT_Library rawLibrary = nullptr;
    // 独立 Library 避免调用间共享可变 Face 和 glyph slot。
    if ( FT_Init_FreeType(&rawLibrary) != 0 || !rawLibrary ) {
        XERROR("Failed to initialize FreeType for Unicode font atlas");
        return std::nullopt;
    }
    UniqueFreeTypeLibrary library(rawLibrary);

    const std::string pathUtf8 = fontPathUtf8(fontPath);
    // 内存 Face 让字体文件字节和解析对象共享明确的局部生命周期。
    auto loadedFace = loadFreeTypeFace(library.get(), fontPath);
    if ( !loadedFace ) {
        XERROR("Failed to load Unicode font: {}", pathUtf8);
        return std::nullopt;
    }
    auto& face = loadedFace->face;

    // Unicode 与 ASCII 使用一致的栅格模式，确保混排时度量尺度相同。
    if ( FT_Set_Pixel_Sizes(face.get(), 0U, pixelHeight) != 0 ) {
        XERROR("Failed to set Unicode font raster size: {}", pathUtf8);
        return std::nullopt;
    }

    RasterizedUnicodeFont result;
    const float inverseHeight = 1.0F / static_cast<float>(pixelHeight);
    result.metrics.valid      = true;
    result.metrics.ascender = static_cast<float>(face->size->metrics.ascender) /
                              64.0F * inverseHeight;
    // 至少保持一个像素的 FreeType 行高，再转换为相对请求高度的比例。
    result.metrics.lineHeight =
        std::max(1.0F, static_cast<float>(face->size->metrics.height) / 64.0F) *
        inverseHeight;
    // 预留候选数量上限，跳过缺失码点不会破坏最终紧凑布局。
    result.metrics.glyphs.reserve(codepoints.size());
    result.glyphs.reserve(codepoints.size());

    std::uint32_t previousCodepoint = 0U;
    bool          hasPrevious       = false;
    for ( const std::uint32_t codepoint : codepoints ) {
        // 递增约束同时完成去重；ASCII 已由固定字体图集负责，不在此重复存储。
        if ( !Common::isValidUnicodeCodepoint(codepoint) ||
             codepoint <= Common::ASCII_GLYPH_LAST ||
             (hasPrevious && codepoint <= previousCodepoint) ) {
            continue;
        }
        // 只有被接受的码点才更新前项，保证后续比较基于最近有效输入。
        previousCodepoint = codepoint;
        hasPrevious       = true;

        // 字体缺少字符映射或无法渲染时跳过，允许上层继续尝试下一回退字体。
        if ( FT_Get_Char_Index(face.get(), codepoint) == 0U ||
             FT_Load_Char(face.get(),
                          codepoint,
                          FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0 ) {
            continue;
        }

        // 度量先记录原始码点，使紧凑数组仍支持按码点二分或顺序查找。
        Common::UnicodeGlyphMetrics metrics;
        metrics.codepoint = codepoint;
        RasterizedAsciiGlyph glyph;
        // 两个输出容器只在同一字形完成复制后同步追加，维持索引对应关系。
        copyLoadedGlyph(face->glyph, inverseHeight, metrics.metrics, glyph);
        // 两次 push 使用同一个成功分支，任何被跳过项都不会留下半条记录。
        result.metrics.glyphs.push_back(metrics);
        result.glyphs.push_back(std::move(glyph));
    }

    XDEBUG("Rasterized Unicode font atlas source: {} ({} px, {} glyphs)",
           pathUtf8,
           pixelHeight,
           result.metrics.glyphs.size());
    // 空字形列表仍是有效结果，表示该字体不覆盖本次请求的非 ASCII 码点。
    return result;
}

}  // namespace MMM::Graphic
