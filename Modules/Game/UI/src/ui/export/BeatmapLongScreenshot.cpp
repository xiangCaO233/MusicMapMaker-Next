/// @file BeatmapLongScreenshot.cpp
/// @brief 静态输出普通编辑画布的多列谱面图。
/// @details UI 线程固定皮肤及会话配置，调用正常物件和分拍线生成过程。
/// 同一张 RGBA 图集服务每个分列，编码线程不依赖可变会话或渲染资源。
/// 每个列保留批次、裁剪范围和时间标记，底板及贴图颜色以同一顺序合成。
/// 压缩器逐行消费图像，整个高度不参与像素缓冲分配。
/// 在暂存目录完整写出并关流后才提交；系统文档的发布是独立一步。
#include "ui/export/BeatmapLongScreenshot.h"

#include "common/AsciiFontData.h"
#include "common/UnicodeFontData.h"
#include "common/Utf8Codepoints.h"
#include "common/platform/DocumentStorage.h"
#include "common/render/CanvasRenderTypes.h"
#include "common/render/TextureID.h"
#include "config/EditorConfig.h"
#include "config/skin/SkinConfig.h"
#include "font/AsciiFontRasterizer.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "mmm/timing/BpmNormalization.h"
#include "mmm/timing/TimingEffect.h"

#include <entt/entt.hpp>
#include <fmt/format.h>
#include <glm/vec4.hpp>
#include <miniz.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace MMM::UI
{
namespace
{

/// @brief 固定时间比例，图片可直接对照编辑器从下往上读谱。
// 每列采用同一纵向比例。比例大于常见密集分拍所需的最小间距，
// 避免默认皮肤下把相邻短音符糊成同一色块。
constexpr int PIXELS_PER_SECOND = 480;
/// @brief 轨宽留出绘制长条、端点和箭头的横向间距。
constexpr int LANE_WIDTH = 66;
/// @brief 左右边距也为顶部水印提供对齐位置。
constexpr int MARGIN = 24;
/// @brief 每种水印独占一行，关闭后回收其高度。
constexpr int WATERMARK_LINE = 30;
/// @brief 宽窄谱面统一用小号署名，避免水印遮住谱面。
constexpr int FONT_HEIGHT = 20;
/// @brief 对齐 Batcher 的图集 UV 与半像素采样约定。
constexpr int ATLAS_SIZE = 2048;
/// @brief 一列时间标签预留的宽度，窄键数在列中居中。
constexpr int MIN_CARD_WIDTH = 286;
/// @brief 列内时间栏高于轨道上边界。
constexpr int LABEL_HEIGHT = 40;
/// @brief 保持通用查看器可处理的像素预算。
constexpr double MAX_PIXELS = 160000000.0;
using Color                 = std::array<unsigned char, 3>;
using Common::Render::CanvasVertex;
using Common::Render::TextureID;
/// @brief 用于无轨道部分、页间距和署名空间的底色。
constexpr Color BACKGROUND{ 16, 18, 22 };
/// @brief 轨道贴图透明处的纯色背景，保留轨道和分拍线对比。
constexpr Color TRACK_BACKGROUND{ 25, 29, 36 };
/// @brief 时间栏与水印的字体色，保留原始字形的覆盖率。
constexpr Color ATTRIBUTION{ 207, 215, 229 };

/// @brief 需要 alpha 合成的字形；位图由调用方的字体栅格结果持有。
/// @details 字形观察现有栅格数据，扫描过程中不能重建字库。
/// 字形度量转换成整图像素后所有标签共用同一套混合操作。
struct Glyph {
    int                                  m_x, m_y;
    const Graphic::RasterizedAsciiGlyph* m_bitmap;
};

/// @brief PNG 块中的整数始终按网络字节序编码。
/// @param value 宽、高、块长度或 CRC 的整数值。
/// @return 与宿主字节序无关的四个字节。
std::array<unsigned char, 4> bigEndian(std::uint32_t value)
{
    return { static_cast<unsigned char>(value >> 24U),
             static_cast<unsigned char>(value >> 16U),
             static_cast<unsigned char>(value >> 8U),
             static_cast<unsigned char>(value) };
}

/// @brief 流式压缩 PNG 扫描行，内存不随图片高度增长。
/// @details 所有 IDAT 共享一个 DEFLATE 流。
/// 长度只描述当前块，不需要先编码完整图像来计算总长。
/// 失败时外层清理暂存文件，与平台文档写回的失败区分。
class PngWriter
{
public:
    /// @brief 建立单一 zlib 流；所有 IDAT 块共享该流的压缩词典。
    /// @param path 本次独占的暂存文件。
    /// @note 打开失败不初始化流，begin 保证不会写出不完整的签名。
    explicit PngWriter(const std::filesystem::path& path)
        : m_file(path, std::ios::binary)
        , m_initialized(m_file && mz_deflateInit(&m_stream, 1) == MZ_OK)
    {
    }

    /// @brief 无论中途在哪一步失败，都回收压缩器内存。
    /// @note 析构负责压缩状态清理；输入错误由成员函数的返回值传递。
    ~PngWriter()
    {
        if ( m_initialized ) mz_deflateEnd(&m_stream);
    }

    /// @brief 写入 PNG 签名和 RGB8 的非交错图像头。
    /// @param width 已验证的 RGB 列数。
    /// @param height 输出像素行数，包含时间栏和水印。
    /// @return 成功建立压缩器并写好 IHDR 时返回 true。
    bool begin(int width, int height)
    {
        if ( !m_initialized ) return false;
        constexpr std::array<unsigned char, 8> signature{ 137, 80, 78, 71,
                                                          13,  10, 26, 10 };
        write(signature);
        const auto                          w = bigEndian(width);
        const auto                          h = bigEndian(height);
        const std::array<unsigned char, 13> header{
            w[0], w[1], w[2], w[3], h[0], h[1], h[2], h[3], 8, 2, 0, 0, 0
        };
        return chunk("IHDR", header);
    }

    /// @brief 压缩一行已包含 PNG 过滤类型字节的数据。
    /// @param bytes 包含过滤类型字节的完整扫描行。
    /// @pre 调用者在完成压缩前持有输入缓冲。
    /// @return 当前输入已消费、可用压缩块已写出时返回 true。
    bool row(std::span<const unsigned char> bytes)
    {
        m_stream.next_in  = bytes.data();
        m_stream.avail_in = static_cast<unsigned int>(bytes.size());
        return compress(MZ_NO_FLUSH);
    }

    /// @brief 刷新最后一个压缩块，写入尾块并检查关闭时的写入错误。
    /// @return 结束块和关流全部成功时返回 true。
    /// @note 磁盘写满可能需要到关流才报告，提交操作不得提前。
    bool finish()
    {
        if ( !compress(MZ_FINISH) || !chunk("IEND", {}) ) return false;
        // 缓冲刷新可能在收尾时才报告磁盘写满，关闭后才能提交正式文件。
        m_file.close();
        return static_cast<bool>(m_file);
    }

private:
    /// @brief 将二进制数据原样写入流；空块允许没有数据。
    /// @param bytes 已由调用方按目标字节序序列化。
    /// @note 不为零字节 span 解引用地址，IEND 的 CRC 仍保留。
    void write(std::span<const unsigned char> bytes)
    {
        if ( !bytes.empty() ) {
            m_file.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
    }

    /// @brief 写入长度、类型、负载及覆盖类型和负载的 CRC。
    /// @param kind 固定四个 ASCII 字节的 PNG 块类型。
    /// @param bytes CRC 覆盖的块正文，可为空。
    /// @return 长度、类型、正文和校验均已写入时返回 true。
    bool chunk(std::string_view kind, std::span<const unsigned char> bytes)
    {
        // 块长度只计算正文，不能把类型或校验和加进去。
        // PNG 校验顺序固定为块类型在前、正文在后。
        write(bigEndian(static_cast<std::uint32_t>(bytes.size())));
        const auto* type = reinterpret_cast<const unsigned char*>(kind.data());
        write({ type, kind.size() });
        write(bytes);
        auto crc = mz_crc32(0, type, kind.size());
        if ( !bytes.empty() ) crc = mz_crc32(crc, bytes.data(), bytes.size());
        write(bigEndian(static_cast<std::uint32_t>(crc)));
        return static_cast<bool>(m_file);
    }

    /// @brief 每次只保留固定大小的压缩输出，块边界无需对应像素行。
    /// @param mode 普通行允许留在压缩器内部，最后一行需要刷新。
    /// @return 成功输出压缩块并消费当前输入时返回 true。
    /// @note 先写本次生成数据，再判断结束标志，避免漏写最终 IDAT。
    bool compress(int mode)
    {
        do {
            m_stream.next_out  = m_output.data();
            m_stream.avail_out = static_cast<unsigned int>(m_output.size());
            const auto result  = mz_deflate(&m_stream, mode);
            if ( result != MZ_OK && result != MZ_STREAM_END ) return false;
            const auto count = m_output.size() - m_stream.avail_out;
            if ( count && !chunk("IDAT", { m_output.data(), count }) )
                return false;
            if ( result == MZ_STREAM_END ) return true;
            // 普通行消费完输入即可返回，结尾必须等到完整的 zlib 尾部。
        } while ( m_stream.avail_in != 0 || mode == MZ_FINISH );
        return true;
    }

    /// @brief 只写临时文件，校验后才对外提交。
    std::ofstream m_file;
    /// @brief 整幅图共用一个连续的 DEFLATE 流。
    mz_stream m_stream{};
    std::array<unsigned char, 65536>
        /// @brief 每次最多生成一个固定大小的块。
        m_output;
    /// @brief 仅成功初始化时回收流状态。
    bool m_initialized{};
};

/// @brief 按字形度量在水印行排版，保留 Unicode 姓名及常见混合字体。
/// @details 位图、度量和 UTF-8 解码都复用画布的字体实现；只加载本次用到的字符。
/// @param chart 提供独立水印状态和两种字体路径。
/// @param width 出图宽度，用于收束较长作者名。
/// @param ascii 西文字体位图和度量。
/// @param unicode 稀疏 Unicode 位图，只有作者名实际使用的码点。
/// @param glyphs 追加字形的绝对像素定位和借用引用。
/// @return 字体均成功准备时返回 true。
/// @note 关闭作者水印时不在 PNG 元数据或图像中写入作者名。
/// 西文字库还用于每列的时间栏，所以两个水印均关闭时也会加载。
bool layoutWatermarks(const BeatmapScreenshotImage& chart, int width,
                      Graphic::RasterizedAsciiFont&   ascii,
                      Graphic::RasterizedUnicodeFont& unicode,
                      std::vector<Glyph>&             glyphs)
{
    // 时间标签和英文署名共用基础字库。
    // 即使关闭所有水印，这里仍须准备绘制每列时间区间所需的字形。
    auto loaded =
        Graphic::AsciiFontRasterizer::rasterize(chart.m_asciiFont, FONT_HEIGHT);
    if ( !loaded ) return false;
    ascii = std::move(*loaded);

    std::vector<std::uint32_t> codepoints;
    Common::appendNonAsciiCodepoints(codepoints, chart.m_author);
    // 只在署名出现非 ASCII 内容时读取第二套字体，
    // 否则不用解码体积较大的 CJK 文件。
    if ( !codepoints.empty() ) {
        // 字库接口依赖已经唯一且升序排列的码点列表。
        std::sort(codepoints.begin(), codepoints.end());
        codepoints.erase(std::unique(codepoints.begin(), codepoints.end()),
                         codepoints.end());
        auto cjk = Graphic::AsciiFontRasterizer::rasterizeUnicode(
            chart.m_cjkFont, codepoints, FONT_HEIGHT);
        if ( !cjk ) return false;
        unicode = std::move(*cjk);
    }

    // 两个水印各自独占一行；仅为已经启用且存在文本的署名增加高度。
    // 关闭其中任意一项时，另一项自动移到首行。
    int baseline = 24;
    for ( auto text :
          { chart.m_showMmm ? std::string_view("MMM") : std::string_view{},
            std::string_view(chart.m_author) } ) {
        if ( text.empty() ) continue;
        int pen = MARGIN;
        for ( std::size_t offset = 0; offset < text.size(); ) {
            const auto cp = Common::decodeNextUtf8Codepoint(text, offset);
            // 控制字符或当前 Unicode 字库未覆盖的字形统一采用后备符号。
            // 查找缺失不会改变后续字形解码偏移。
            const auto asciiKey =
                cp >= 32 && cp <= 126 ? static_cast<char>(cp) : '?';
            const auto* metrics = ascii.metrics.glyph(asciiKey);
            const auto* bitmap =
                &ascii.glyphs[asciiKey - Common::ASCII_GLYPH_FIRST];
            if ( const auto* cjk = unicode.metrics.glyph(cp);
                 cjk && cjk->available ) {
                const auto& entries = unicode.metrics.glyphs;
                const auto  it =
                    std::lower_bound(entries.begin(),
                                     entries.end(),
                                     cp,
                                     [](const auto& item, std::uint32_t value) {
                                         return item.codepoint < value;
                                     });
                metrics = cjk;
                bitmap  = &unicode.glyphs[it - entries.begin()];
            }
            // 笔位置先加字形原始 bearing，不能用 advance 代替真实字形宽度。
            // 决定截断后不再写该行其余字形，避免长名扩宽整图。
            const int x = pen + std::lround(metrics->bearingX * FONT_HEIGHT);
            if ( x + static_cast<int>(bitmap->width) > width - MARGIN ) break;
            if ( metrics->hasBitmap )
                glyphs.push_back(
                    { x,
                      baseline - static_cast<int>(std::lround(
                                     metrics->bearingY * FONT_HEIGHT)),
                      bitmap });
            pen +=
                static_cast<int>(std::lround(metrics->advanceX * FONT_HEIGHT));
        }
        baseline += WATERMARK_LINE;
    }
    return true;
}

/// @brief 将一个水印扫描行合成到底色，只读取当前行所需的字形像素。
/// @param y 图像从顶部开始的扫描行号。
/// @param row 已铺好底色的像素缓冲区。
/// @param glyphs 已解析的字形位置及对应 RGBA 位图。
/// @note 这里只混合本行命中的字形，字体像素不另作屏幕纹理。
void drawWatermarks(int y, std::span<Color> row,
                    const std::vector<Glyph>& glyphs)
{
    for ( const auto& [x, top, bitmap] : glyphs ) {
        const int dy = y - top;
        // 字形的 origin 由 bearing 计算，可能落在并不处于本行的位置。
        // 这个检查也排除了书写宽度溢出后留在页头之外的不可见字形。
        if ( dy < 0 || dy >= static_cast<int>(bitmap->height) ) continue;
        for ( unsigned int dx = 0; dx < bitmap->width; ++dx ) {
            const int column = x + static_cast<int>(dx);
            if ( column < 0 || column >= static_cast<int>(row.size()) )
                continue;
            // FreeType 位图为 RGBA8，使用 alpha 作为笔画覆盖率。
            // 现有字体为 RGBA8，笔画覆盖来自 alpha 一路。
            // 字体原位保存在所属栅格结果中，混合时不拷贝位图。
            const auto alpha =
                bitmap->pixels[(dy * bitmap->width + dx) * 4 + 3];
            for ( std::size_t channel = 0; channel < ATTRIBUTION.size();
                  ++channel ) {
                row[column][channel] = static_cast<unsigned char>(
                    (ATTRIBUTION[channel] * alpha +
                     row[column][channel] * (255 - alpha)) /
                    255);
            }
        }
    }
}

/// @brief 单列卡片给时间标签留宽；宽谱面继续保留所有正式轨道。
/// @param trackWidth 正式轨道像素宽度，不含页边距。
/// @return 时间栏与谱面区都能容纳的宽度。
/// @note 窄键数无需拉宽音符，轨道在统一列间距中居中。
int cardWidth(int trackWidth)
{
    return std::max(MIN_CARD_WIDTH, trackWidth + MARGIN);
}

/// @brief 生成分秒标签并保留四位小数；负时间和超过 60 分钟的值也可读。
/// @param seconds 相对于歌曲原点的秒数。
/// @return 可带负号的累计分钟和四位小数秒。
/// @note 先舍入整数刻度再拆分分秒，进位不会得到 60 秒。
std::string timeLabel(double seconds)
{
    // 按一万分之一秒先取整再拆出各位，
    // 带进位的曲末不会出现分秒边界错误。
    const auto ticks = std::llround(std::abs(seconds) * 10000);
    return fmt::format("{}{:02}:{:02}.{:04}",
                       seconds < 0 ? "-" : "",
                       ticks / 600000,
                       ticks / 10000 % 60,
                       ticks % 10000);
}

/// @brief 固定当前皮肤的 RGBA 像素，批次和像素使用同一编号及 UV。
/// @details 图集内预留间隔，插值采样不会把两个不相干的物件纹理混到一起。
/// @param atlas 组装后的 RGBA8 皮肤像素。
/// @param scene 使用其语义纹理到归一化 UV 的映射。
/// @return 音符所需的正式皮肤资源已准备完成时返回 true。
/// @details 文件尺寸限制先于读取，像素尺寸限制先于解码。
/// 图集与正常 Batcher 尺寸相同，UV 宽高比保留原皮肤的像素比例。
/// 条目分配采用分行安放并留下采样间隙，避免双线性滤波混入邻图。
/// 连接体和点贴图共用一张图集，避免不同列使用不同代际的皮肤。
bool loadAtlas(std::vector<unsigned char>&     atlas,
               Common::Render::RenderSnapshot& scene)
{
    // 资产表须与 Basic2DCanvas 的同名纹理身份保持一致。
    // 两端箭头分别装载，避免在导出时自行镜像一张不对称贴图。
    // 普通皮肤可缺 HoldHead，由已共用的 Hold 渲染器决定回退。
    constexpr std::array assets{
        std::pair{ TextureID::Note, "note.note" },
        std::pair{ TextureID::HoldHead, "note.holdhead" },
        std::pair{ TextureID::Node, "note.node" },
        std::pair{ TextureID::HoldEnd, "note.holdend" },
        std::pair{ TextureID::HoldBodyVertical, "note.holdbodyvertical" },
        std::pair{ TextureID::HoldBodyHorizontal, "note.holdbodyhorizontal" },
        std::pair{ TextureID::FlickArrowLeft, "note.arrowleft" },
        std::pair{ TextureID::FlickArrowRight, "note.arrowright" },
        std::pair{ TextureID::Track, "panel.track.background" },
    };
    atlas.resize(ATLAS_SIZE * ATLAS_SIZE * 4);
    // 无材质批次采样纯白，由顶点颜色决定最终显示色。
    atlas[0] = atlas[1] = atlas[2] = atlas[3] = 255;
    scene.uvMap.emplace(
        static_cast<unsigned>(TextureID::None),
        glm::vec4{ 0, 0, 1.0f / ATLAS_SIZE, 1.0f / ATLAS_SIZE });
    const auto& paths = Config::SkinManager::instance().getData().assetPaths;
    int         x = 2, y = 0, shelfHeight = 0;
    for ( const auto& [id, key] : assets ) {
        const auto found = paths.find(key);
        if ( found == paths.end() || found->second.empty() ) {
            // Hold 头部有既有的普通物件回退，轨道也可使用画布底色。
            if ( id == TextureID::HoldHead || id == TextureID::Track ) continue;
            return false;
        }
        std::ifstream stream(found->second, std::ios::binary | std::ios::ate);
        const auto    fileSize = stream.tellg();
        // 大小上限先于读取，避免超大自定义素材超出导出器的内存预算。
        if ( fileSize <= 0 || fileSize > 64 * 1024 * 1024 ) return false;
        const auto                 size = static_cast<int>(fileSize);
        std::vector<unsigned char> bytes(size);
        stream.seekg(0);
        if ( !stream.read(reinterpret_cast<char*>(bytes.data()), size) )
            return false;
        int width = 0, height = 0, channels = 0;
        // 先验证尺寸再分配解码缓冲，图集不受超大自定义图片拖累。
        if ( !stbi_info_from_memory(
                 bytes.data(), size, &width, &height, &channels) ||
             width <= 0 || height <= 0 || width > ATLAS_SIZE ||
             height > ATLAS_SIZE )
            return false;
        // 同一行使用已经放入的最高纹理来推进，不能按当前纹理高度推进。
        // 新行宽度仍从零开始，选取固定尺寸矩形后才生成其归一化 UV。
        if ( x + width > ATLAS_SIZE ) {
            x = 0;
            y += shelfHeight + 2;
            shelfHeight = 0;
        }
        if ( y + height > ATLAS_SIZE ) return false;
        const std::unique_ptr<unsigned char, decltype(&stbi_image_free)> data(
            stbi_load_from_memory(
                bytes.data(), size, &width, &height, &channels, STBI_rgb_alpha),
            &stbi_image_free);
        if ( !data ) return false;
        // RGBA 是行优先布局，目的跨度取图集宽而非纹理宽。
        // 只复制有效纹理像素，图集填充区保持透明。
        for ( int row = 0; row < height; ++row ) {
            const auto dst = ((y + row) * ATLAS_SIZE + x) * 4;
            std::copy_n(
                data.get() + row * width * 4, width * 4, atlas.begin() + dst);
        }
        scene.uvMap.emplace(
            static_cast<unsigned>(id),
            glm::vec4{ static_cast<float>(x) / ATLAS_SIZE,
                       static_cast<float>(y) / ATLAS_SIZE,
                       static_cast<float>(width) / ATLAS_SIZE,
                       static_cast<float>(height) / ATLAS_SIZE });
        x += width + 2;
        shelfHeight = std::max(shelfHeight, height);
    }
    return true;
}

/// @brief 一个批次四边形的反变换和范围，保留绘制顺序与纹理方向。
/// @note 形状及裁剪来自逻辑层，没有硬编码音符的形状或尺寸。
/// 坐标保存为列局部像素，移到成品行是在编码阶段完成。
struct Quad {
    /// @brief 四边形在裁剪后的包围区间，右边界和下边界不包含。
    int m_left, m_right, m_top, m_bottom;
    /// @brief 本列中四边形的基点、UV 和非预乘颜色。
    /// @note 原点选择第一个索引的角，不需要假定几何已经正向排列。
    CanvasVertex m_origin;
    /// @brief 从像素偏移反解两个边方向的比例。
    /// @note 逆矩阵在预处理阶段一次计算；活动扫描行只做乘加。
    float m_ax, m_ay, m_bx, m_by;
    /// @brief 纹理坐标沿两个边方向的变化量。
    /// @note Batcher 的四边形 UV 沿相邻边分量独立，另一分量沿边保持不变。
    float m_u, m_v;
};

/// @brief 按 Vulkan 纹理像素中心的约定进行双线性采样。
/// @note 工作线程只读图集；图集两侧超出部分钳制到边缘。
/// @param atlas 已准备且不会变化的 RGBA8 图集。
/// @param u 水平方向归一化坐标。
/// @param v 垂直方向归一化坐标。
/// @return 双线性插值后的四通道颜色，范围为 0 至 255。
/// @note Batcher 已应用相同的半像素约定，本函数无需查找原贴图边框。
std::array<float, 4> sampleAtlas(std::span<const unsigned char> atlas, float u,
                                 float v)
{
    // 采样与图形后端一样以像素中心为整数位置。
    // 最后一行或末列在图集边界时，两个插值端点会钳在相同位置。
    const float cx = std::clamp(u * ATLAS_SIZE - 0.5f, 0.0f, ATLAS_SIZE - 1.0f);
    const float cy = std::clamp(v * ATLAS_SIZE - 0.5f, 0.0f, ATLAS_SIZE - 1.0f);
    const auto  x = static_cast<int>(cx), y = static_cast<int>(cy);
    const auto  right = std::min(x + 1, ATLAS_SIZE - 1);
    const auto  below = std::min(y + 1, ATLAS_SIZE - 1);
    const auto  a     = (y * ATLAS_SIZE + x) * 4;
    const auto  b     = (y * ATLAS_SIZE + right) * 4;
    const auto  c     = (below * ATLAS_SIZE + x) * 4;
    const auto  d     = (below * ATLAS_SIZE + right) * 4;
    // 即使颜色完全透明，双线性插值仍需要全部四个通道。
    // 后续绘制阶段统一执行纹理色、顶点乘色和不透明度混合。
    std::array<float, 4> pixel;
    for ( int k = 0; k < 4; ++k )
        pixel[k] = std::lerp(std::lerp(atlas[a + k], atlas[b + k], cx - x),
                             std::lerp(atlas[c + k], atlas[d + k], cx - x),
                             cy - y);
    return pixel;
}

/// @brief 仅扫描当前列实际覆盖本行的四边形，从底板到物件保持画布顺序。
/// @details 普通绘制指令把一张贴图的四边形拆成索引三角形。
/// 列只绘制底板、分拍、普通点贴图及其横纵连接体。
/// 对四边形反解相邻边的比例即可按皮肤相同顺序采样整块图元。
/// 按进入扫描行的顺序整理起点索引，活动集合仍使用原始绘制顺序。
/// 底板与分拍线保持在音符下方，链接体不会意外覆盖后画的节点。
/// 编码行只借用瞬时 RGB 缓冲，栅格器不保存整个列的像素。
class ColumnRasterizer
{
public:
    /// @brief 从主画布的四边形索引协议取数据；保留原始批次裁剪。
    /// @param column 已生成绘制批次的一列。
    /// @pre 批次索引应由同一四边形 Batcher 生成，图集对应同一列的 UV。
    /// @note 裁剪在建立包围框时执行，后续不逐像素读取批次命令。
    explicit ColumnRasterizer(const BeatmapScreenshotColumn& column)
    {
        const auto& scene = column.m_scene;
        for ( const auto& cmd : scene.cmds ) {
            // 逻辑批次可含刚好越过时间选择边界的贴图边缘，
            // 屏幕剪裁按命令生效，不能用图像尺寸取代每条命令自己的裁剪。
            const auto& clip = cmd.scissor;
            for ( auto offset = cmd.indexOffset;
                  offset + 5 < cmd.indexOffset + cmd.indexCount;
                  offset += 6 ) {
                const auto& a   = scene.vertices[scene.indices[offset]];
                const auto& b   = scene.vertices[scene.indices[offset + 1]];
                const auto& c   = scene.vertices[scene.indices[offset + 2]];
                const auto& d   = scene.vertices[scene.indices[offset + 4]];
                const float abx = b.pos.x - a.pos.x, aby = b.pos.y - a.pos.y;
                const float adx = d.pos.x - a.pos.x, ady = d.pos.y - a.pos.y;
                const auto  determinant = abx * ady - aby * adx;
                if ( !std::isfinite(determinant) ||
                     std::abs(determinant) < 1e-5f )
                    continue;
                // 先与绘制指令裁剪框求交，再按光栅器像素中心取样。
                // 左上和右下都按像素中心计算半开区间，
                // 例如几何右边界落在中心时不重复写相邻批次边界。
                const auto bound = [](float edge, int first, int last) {
                    return static_cast<int>(
                        std::clamp(std::ceil(edge - 0.5f),
                                   static_cast<float>(first),
                                   static_cast<float>(last)));
                };
                const int right  = clip.x + static_cast<int>(clip.width);
                const int bottom = clip.y + static_cast<int>(clip.height);
                Quad      quad{
                    bound(std::min({ a.pos.x, b.pos.x, c.pos.x, d.pos.x }),
                          clip.x,
                          right),
                    bound(std::max({ a.pos.x, b.pos.x, c.pos.x, d.pos.x }),
                          clip.x,
                          right),
                    bound(std::min({ a.pos.y, b.pos.y, c.pos.y, d.pos.y }),
                          clip.y,
                          bottom),
                    bound(std::max({ a.pos.y, b.pos.y, c.pos.y, d.pos.y }),
                          clip.y,
                          bottom),
                    a,
                    ady / determinant,
                    -adx / determinant,
                    -aby / determinant,
                    abx / determinant,
                    b.uv.u - a.uv.u,
                    d.uv.v - a.uv.v
                };
                if ( quad.m_top >= quad.m_bottom ||
                     quad.m_left >= quad.m_right )
                    continue;
                m_order.push_back(m_quads.size());
                m_quads.push_back(quad);
            }
        }
        // 索引排序只决定何时加入，绘制仍按照原始批次的顺序。
        std::stable_sort(
            m_order.begin(), m_order.end(), [this](auto a, auto b) {
                return m_quads[a].m_top < m_quads[b].m_top;
            });
    }

    /// @brief 合成正式画布的一行，颜色乘色及 Alpha 与普通画布一致。
    /// @param y 本列局部像素行号，必须连续递增。
    /// @param pixels 此列占有的行内范围，不含左右页间距。
    /// @param atlas 只读皮肤图集，在本次导出所有列之间共用。
    /// @details 先重建活动索引再按顺序绘制，结束的长条立即退出集合。
    /// 像素中心反求纹理内的位置，透明像素按照顶点色叠在既有像素上。
    /// @note 上层可在同一行依次提交多列，列之间没有共享扫描状态。
    void draw(int y, std::span<Color> pixels,
              std::span<const unsigned char> atlas)
    {
        std::fill(pixels.begin(), pixels.end(), TRACK_BACKGROUND);
        std::erase_if(m_active,
                      [&](auto index) { return m_quads[index].m_bottom <= y; });
        while ( m_next < m_order.size() &&
                m_quads[m_order[m_next]].m_top <= y ) {
            const auto index = m_order[m_next++];
            m_active.insert(
                std::lower_bound(m_active.begin(), m_active.end(), index),
                index);
        }
        for ( auto index : m_active ) {
            const auto& q = m_quads[index];
            const auto& p = q.m_origin;
            // 列内同一行所有像素共享 Y 偏移，
            // 使用每个像素的 X 偏移反求两个纹理轴。
            const float dy = y + 0.5f - p.pos.y;
            for ( int x = q.m_left;
                  x < std::min(q.m_right, static_cast<int>(pixels.size()));
                  ++x ) {
                const float dx = x + 0.5f - p.pos.x;
                const float s  = q.m_ax * dx + q.m_ay * dy;
                const float t  = q.m_bx * dx + q.m_by * dy;
                // 包围矩形可能含有斜接段之外的像素；用局部坐标确认实际覆盖。
                // 这份裁剪与时间窗边界独立，避免移除斜边内部仍应绘制的区域。
                if ( s < 0 || s > 1 || t < 0 || t > 1 ) continue;
                const auto tex =
                    sampleAtlas(atlas, p.uv.u + q.m_u * s, p.uv.v + q.m_v * t);
                const auto alpha =
                    std::clamp(p.color.a, 0.0f, 1.0f) * tex[3] / 255.0f;
                // 颜色来自普通画布每部件规则，含用户覆盖选项。
                // 这里不能把纹理本身的 alpha 忽略或硬编码成统一不透明。
                const std::array tint{ p.color.r, p.color.g, p.color.b };
                for ( int k = 0; k < 3; ++k )
                    pixels[x][k] = static_cast<unsigned char>(std::lround(
                        std::lerp(pixels[x][k],
                                  tex[k] * std::clamp(tint[k], 0.0f, 1.0f),
                                  alpha)));
            }
        }
    }

private:
    /// @brief 顺序与原始绘制批次相同。
    /// @note 图元数组完成后不再增长，其下标同时代表画布的绘制顺序。
    std::vector<Quad> m_quads;
    /// @brief 入队索引按首行排序；活动索引按批次次序排序。
    /// @details 起始索引只在构造时排序，活动索引随扫描结束逐步收敛。
    /// 工作集减少到当前行实际可见的图元，避免每行遍历整谱。
    std::vector<std::size_t> m_order, m_active;
    /// @brief 首个尚未进入扫描集合的图元。
    /// @note 构造时为零，扫码结束时等于已记录的起始索引总数。
    std::size_t m_next{};
};

/// @brief 在已栅格化的西文字体中布局每列的时间标签。
/// @param font 已栅格化的西文字形数据。
/// @param text 当前列的 ASCII 起止时间。
/// @param pen 初始的图像水平排版位置。
/// @param baseline 各列统一使用的时间栏基线。
/// @param glyphs 期望追加到整图扫描布局的字形。
/// @note 起止时间放在轨道上方，避免任何可选水印遮盖物件。
void labelGlyphs(const Graphic::RasterizedAsciiFont& font,
                 std::string_view text, int pen, int baseline,
                 std::vector<Glyph>& glyphs)
{
    for ( char ch : text ) {
        // 列标签格式全部是西文数字和分隔符，
        // 不重复加载 Unicode 字库也能保留完整毫秒刻度。
        const auto* metrics = font.metrics.glyph(ch);
        if ( !metrics ) continue;
        const auto& bitmap = font.glyphs[ch - Common::ASCII_GLYPH_FIRST];
        if ( metrics->hasBitmap ) {
            glyphs.push_back({ pen + static_cast<int>(std::lround(
                                         metrics->bearingX * FONT_HEIGHT)),
                               baseline - static_cast<int>(std::lround(
                                              metrics->bearingY * FONT_HEIGHT)),
                               &bitmap });
        }
        pen += static_cast<int>(std::lround(metrics->advanceX * FONT_HEIGHT));
    }
}

/// @brief 逐行编码，末列不足设定时长时底部留空，不伸缩该列的物件或时间刻度。
/// @param image 准备好的画布列表、皮肤和字体路径。
/// @param temp 暂存目录中的 PNG。
/// @param stop 由工作线程拥有者传递的取消状态。
/// @return 关闭 PNG 后才返回 Saved；此时尚未写回系统文档。
/// @details 首列决定输出高度，末列使用实际秒数和相同像素比例。
/// 没有物件的余下图像区域继续使用整图底色。
/// 每个绘制器只按该列的局部行号推进。
/// 整行合成后过滤再压缩，确保列顺序、边距和水印不会乱序。
ScreenshotResult encode(const BeatmapScreenshotImage& image,
                        const std::filesystem::path& temp, std::stop_token stop)
{
    // 宽度包含所有列及相邻列间的空白；短列不补轨道底板。
    // 高度使用首列，因为只有最右末列可能不足设定时长。
    const int spacing = cardWidth(image.m_columnWidth);
    // 以总列数计算宽度；只消耗本行的这段空间。
    // 不需要按最终成品的尺寸创建一个 GPU 纹理。
    const int width =
        static_cast<int>(image.m_columns.size()) * spacing + MARGIN;
    // 单独的时间栏始终存在，两个可选署名不挤占轨道高度。
    // 无署名的图依然保留可读的起止时间。
    const int top = LABEL_HEIGHT + WATERMARK_LINE * (image.m_showMmm +
                                                     !image.m_author.empty());
    const int height = top + image.m_columns.front().m_height + MARGIN;
    Graphic::RasterizedAsciiFont   ascii;
    Graphic::RasterizedUnicodeFont unicode;
    std::vector<Glyph>             glyphs;
    if ( !layoutWatermarks(image, width, ascii, unicode, glyphs) )
        return ScreenshotResult::FontError;

    // 每列批次独立排序并保留自己的活动集合。
    // 这样相邻列即使同时含一个长条，也不会消费彼此的裁剪。
    std::vector<ColumnRasterizer> rasterizers;
    for ( std::size_t col = 0; col < image.m_columns.size(); ++col ) {
        rasterizers.emplace_back(image.m_columns[col]);
        labelGlyphs(ascii,
                    image.m_columns[col].m_label,
                    MARGIN + static_cast<int>(col) * spacing,
                    top - 13,
                    glyphs);
    }

    PngWriter png(temp);
    if ( !png.begin(width, height) ) return ScreenshotResult::WriteError;
    std::vector<Color>         row(width);
    std::vector<unsigned char> bytes(1 + 3 * row.size());
    // 行缓冲和当前活跃图元独立于 PNG 高度，避免成图后才整体压缩。
    for ( int y = 0; y < height; ++y ) {
        // 取消在写成品前检查。
        // 终止过程中临时图由守卫清理，用户路径保留原文件。
        if ( stop.stop_requested() ) return ScreenshotResult::Cancelled;
        std::fill(row.begin(), row.end(), BACKGROUND);
        // 轨道从时间栏之后开始，防止长图裁切时标签落在节点上。
        // 逐列判断短列是否还有实际轨道，再尝试合成其活动图元。
        if ( y < top ) {
            drawWatermarks(y, row, glyphs);
        } else {
            const int dy = y - top;
            for ( std::size_t col = 0; col < image.m_columns.size(); ++col ) {
                if ( dy >= image.m_columns[col].m_height ) continue;
                // 每一列使用同样的宽度和时间比例，只有有效裁切高度不同。
                const auto left = MARGIN + col * spacing +
                                  (spacing - MARGIN - image.m_columnWidth) / 2;
                rasterizers[col].draw(
                    dy,
                    std::span(row).subspan(left, image.m_columnWidth),
                    image.m_atlas);
            }
        }

        // Sub 过滤有利于压缩横向重复像素，保持 PNG 无损。
        // Sub 滤波按同一通道的相邻像素做差。
        // 每行首像素总以前值为零；不需要保留上一整行数据。
        bytes[0] = 1;
        Color previous{};
        for ( std::size_t x = 0; x < row.size(); ++x ) {
            for ( std::size_t k = 0; k < previous.size(); ++k )
                bytes[x * 3 + k + 1] =
                    static_cast<unsigned char>(row[x][k] - previous[k]);
            previous = row[x];
        }
        if ( !png.row(bytes) ) return ScreenshotResult::WriteError;
    }
    // 扫描行全写完并不代表 PNG 完整，仍需提交压缩尾数据和 IEND。
    // 关闭文件后才让调用方拿到可读的最终结果。
    return png.finish() ? ScreenshotResult::Saved
                        : ScreenshotResult::WriteError;
}

/// @brief 捕获的根物件可占据几个相邻列，折线范围取所有内部子段。
/// @param note 保留实际类型、时间和全部子段的根物件。
/// @return 它可能与哪些列时间相交的秒域包络。
/// @note 折线按整件参与裁剪，几何仍由普通画布从子段生成。
std::pair<double, double> timeRange(const Logic::NoteComponent& note)
{
    // 保存根的时间边界，再将各子段扩入区间。
    // 这样含看不见的根但仍有可见子段的折线也能出现在中间列。
    double first = note.m_timestamp;
    double last  = first + std::max(0.0, note.m_duration);
    for ( const auto& sub : note.m_subNotes ) {
        first = std::min(first, sub.timestamp);
        last  = std::max(last, sub.timestamp + std::max(0.0, sub.duration));
    }
    return { first, last };
}

/// @brief 暂存目录仅包含本次编码的文件，作用域退出自动回收。
/// @details 目录创建与权限确认先于该守卫构造。
/// 原子提交后目录中不再有 image 文件，清理仍可按同一路径执行。
struct StagingDirectory {
    /// @brief 只有独占创建成功后才构造清理守卫。
    std::filesystem::path m_path;

    /// @brief 移除 writer 使用的临时文件与其容器，失败不触碰最终目标。
    /// @note 清理不递归，不能移除同目录已有目标或其他临时文件。
    ~StagingDirectory()
    {
        std::error_code error;
        std::filesystem::remove(m_path / "chart.png", error);
        std::filesystem::remove(m_path, error);
    }
};
}  // namespace

/// @brief 校验输出范围，限制横向分列后的整体图片尺寸。
/// @param lanes 可见正式轨道数量。
/// @param begin 起始时间，以秒计。
/// @param end 结束时间，必须晚于起点。
/// @param columnSeconds 单列时长上限，参与宽度和高度的预算。
/// @return 像素总数在上限内且输入为可用时间时返回 true。
/// @note 为两个水印预留预算，单独开关不改变有效时段。
bool validScreenshotRange(int lanes, double begin, double end,
                          int columnSeconds)
{
    if ( lanes <= 0 || lanes > 64 || !std::isfinite(begin) ||
         !std::isfinite(end) || std::abs(begin) > 1e8 || std::abs(end) > 1e8 ||
         end <= begin || columnSeconds <= 0 )
        return false;
    // 预算和生成使用同一列时长，较短的列会增加总宽度。
    const auto width = std::ceil((end - begin) / columnSeconds) *
                           cardWidth(lanes * LANE_WIDTH) +
                       MARGIN;
    const auto height =
        std::ceil(std::min(end - begin, static_cast<double>(columnSeconds)) *
                  PIXELS_PER_SECOND) +
        LABEL_HEIGHT + 2 * WATERMARK_LINE + MARGIN + LANE_WIDTH;
    // 水印和时间栏逐图绘制，不能按每列分别计算其垂直开销。
    return width * height <= MAX_PIXELS;
}

/// @brief 使用真实皮肤、普通画布渲染器及线性时间生成独立的导出批次。
/// @param chart 从当前编辑会话取得、包含未保存修改的快照。
/// @param config 提供普通画布的物件尺寸、乘色、对齐和分拍规则。
/// @return 可以脱离会话独立渲染的全部分列，失败时返回空。
/// @details 私有注册表用线性时间，保留 BPM 曲线而不应用滚速效果。
/// 输出按时间向右排列，每列内仍从下向上读谱。
/// 缩短的最后一列不改变时间比例，选区边界运用普通贴图裁剪。
/// @note 会获取当前皮肤文件，只能由用户已经确认的导出入口调用。
std::optional<BeatmapScreenshotImage> prepareBeatmapScreenshot(
    const BeatmapScreenshot& chart, const Config::EditorConfig& config)
{
    // 先限制尺寸再加载皮肤，非法范围不会读取任意图像数据。
    // 选择太长的区间可以缩短，自定义样式和源谱面无需调整。
    if ( !validScreenshotRange(
             chart.m_lanes, chart.m_begin, chart.m_end, chart.m_columnSeconds) )
        return std::nullopt;
    BeatmapScreenshotImage image;
    image.m_columnWidth = chart.m_lanes * LANE_WIDTH;
    image.m_showMmm     = chart.m_showMmm;
    image.m_author      = chart.m_author;
    image.m_asciiFont   = chart.m_asciiFont;
    image.m_cjkFont     = chart.m_cjkFont;

    Common::Render::RenderSnapshot atlasScene;
    if ( !loadAtlas(image.m_atlas, atlasScene) ) return std::nullopt;
    // 使用一份会话样式的值副本。
    // 分拍显示、皮肤颜色、部件尺寸和对齐仍由原配置决定。
    // 线性映射只在这份私有导出参数里启用，每列时间刻度相同。
    auto linear = config;
    // 图片是连续可阅读的时间尺，不受跳变、反向滚动或模拟判定影响。
    linear.visual.enableLinearScrollMapping = true;
    linear.visual.timelineZoom              = 1.0f;
    linear.visual.simulateAutoplay          = false;

    entt::registry                               timeline;
    std::vector<const Logic::TimelineComponent*> tempos;
    // 先写完实体，再保存观察指针，避免扩容造成引用失效。
    for ( const auto& marker : chart.m_tempos ) {
        if ( std::isfinite(marker.m_timestamp) && marker.m_value > 0.0 &&
             std::isfinite(marker.m_value) )
            timeline.emplace<Logic::TimelineComponent>(timeline.create(),
                                                       marker);
    }
    if ( timeline.view<const Logic::TimelineComponent>().empty() ) {
        timeline.emplace<Logic::TimelineComponent>(
            timeline.create(),
            Logic::TimelineComponent{
                0.0,
                TimingEffect::BPM,
                chart.m_defaultBpm > 0 ? normalizeBpmValue(chart.m_defaultBpm)
                                       : DEFAULT_NORMALIZED_BPM,
                {},
                {} });
    }
    for ( const auto& [entity, timing] :
          timeline.view<const Logic::TimelineComponent>().each() )
        tempos.push_back(&timing);
    // 分拍绘制按相邻 BPM 段寻找有效范围，
    // 所以在生成第一列以前先排序一次，后续各列借用同一顺序。
    std::sort(tempos.begin(), tempos.end(), [](const auto* a, const auto* b) {
        return a->m_timestamp < b->m_timestamp;
    });
    Logic::System::ScrollCache cache;
    // 给普通画布的滚动缓存保留 BPM 用于定位真实分拍。
    // 静态导出不保留播放插值的锚点，不会因当前歌曲播放位置而移动拍线。
    cache.rebuild(timeline, linear, nullptr);
    entt::registry notes;
    // 会话渲染器按上下文查找时间映射；这里只有导出私有缓存。
    // 注册表析构先于 cache，内部观察指针不会悬空。
    notes.ctx().emplace<const Logic::System::ScrollCache*>(&cache);
    std::vector<std::pair<entt::entity, std::pair<double, double>>> objects;
    // 整谱捕获可能包含任一时间段的物件，只有和选区相交的才加入绘制表。
    // 同一注册表可服务多个列，不重新构造所有未相交物件。
    for ( const auto& note : chart.m_notes ) {
        const auto [first, last] = timeRange(note);
        if ( !std::isfinite(first) || !std::isfinite(last) ||
             first > chart.m_end || last < chart.m_begin )
            continue;
        const auto entity = notes.create();
        notes.emplace<Logic::NoteComponent>(entity, note);
        // 普通画布要求每个渲染实体有变换组件；导出坐标仍由相同时间映射求值。
        notes.emplace<Logic::TransformComponent>(entity);
        objects.emplace_back(entity, std::pair{ first, last });
    }
    std::vector<entt::entity> visible;
    const double              columnSeconds = chart.m_columnSeconds;
    const auto                count         = static_cast<int>(
        std::ceil((chart.m_end - chart.m_begin) / columnSeconds));
    // 列时间范围根据起始时间逐列推导，而不是累计上一次舍入后的像素。
    // 导致像素长度不足一行的选区也保留一行可见轨道。
    for ( int index = 0; index < count; ++index ) {
        const auto first = chart.m_begin + index * columnSeconds;
        const auto last  = std::min(chart.m_end, first + columnSeconds);
        visible.clear();
        for ( const auto& [entity, range] : objects ) {
            const auto [start, end] = range;
            // 交界瞬时物件归入后一列；跨界的实体在两列分别由同一裁剪逻辑处理。
            if ( end >= first &&
                 (start < last || (index + 1 == count && start == last)) )
                visible.push_back(entity);
        }
        BeatmapScreenshotColumn column;
        column.m_label  = timeLabel(first) + " - " + timeLabel(last);
        column.m_height = std::max(1,
                                   static_cast<int>(std::ceil(
                                       (last - first) * PIXELS_PER_SECOND))) +
                          LANE_WIDTH;
        // 几何和像素分离：各列只持有自己的绘制范围与命令，
        // 贴图数据在整张图片只保留一份。
        column.m_scene.uvMap = atlasScene.uvMap;
        Logic::System::NoteRenderSystem::generateLongScreenshotColumn(
            notes,
            visible,
            cache,
            tempos,
            column.m_scene,
            linear,
            first,
            image.m_columnWidth,
            column.m_height,
            chart.m_lanes,
            PIXELS_PER_SECOND,
            chart.m_drawBeatLines);
        image.m_columns.push_back(std::move(column));
    }
    // 返回的几何和 RGBA 不引用局部 registry、cache 或 SkinManager。
    // 后台线程在这些对象销毁后仍可以独立编码每一列。
    // 视野和播放锚点也不需要在完成后恢复。
    return image;
}

/// @brief 完整编码后提交结果；通过 SAF 保存时还要确认文档写回成功。
/// @param image 已固定几何和贴图的不可变输入。
/// @param path 用户在系统或内置选择器确认的实际输出路径。
/// @param stop 可以在文件提交前取消。
/// @return 目标已完整写入，且系统文档写回成功时返回 Saved。
/// @details 暂存目录与输出同卷，只有编码成功才提交文件重命名。
/// Android 发布失败仍保留完整的私有副本，并通过统一反馈报告错误。
ScreenshotResult writeBeatmapScreenshot(const BeatmapScreenshotImage& image,
                                        const std::filesystem::path&  path,
                                        std::stop_token               stop)
{
    if ( image.m_columns.empty() ) return ScreenshotResult::InvalidRange;
    if ( stop.stop_requested() ) return ScreenshotResult::Cancelled;
    if ( path.empty() ) return ScreenshotResult::WriteError;

    // 暂存和成品在同一文件系统，重命名之前已有目标保留原内容。
    // 独占目录创建失败时不能复用别的任务的临时文件。
    // 任务拥有目录后才拥有清理其内容的权限。
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto stagingPath =
        path.parent_path() / (".mmm-png-" + std::to_string(stamp));
    std::error_code error;
    if ( !std::filesystem::create_directory(stagingPath, error) )
        return ScreenshotResult::WriteError;
    StagingDirectory staging{ stagingPath };
    const auto       temp   = stagingPath / "chart.png";
    const auto       result = encode(image, temp, stop);
    if ( result != ScreenshotResult::Saved ) return result;
    if ( stop.stop_requested() ) return ScreenshotResult::Cancelled;
    std::filesystem::rename(temp, path, error);
    if ( error ) return ScreenshotResult::WriteError;
    std::string publishError;
    // 普通路径已经在上一步完成。
    // 系统选中的文档还需把本地完整文件写回其提供器，不能提前提示成功。
    if ( !Common::publishDocument(path, publishError) ) {
        XERROR("Beatmap PNG publication failed: {}", publishError);
        return ScreenshotResult::PublishError;
    }
    return ScreenshotResult::Saved;
}
}  // namespace MMM::UI
