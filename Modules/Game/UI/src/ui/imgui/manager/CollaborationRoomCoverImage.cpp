#include "ui/imgui/manager/CollaborationRoomCoverImage.h"

#include "config/Utf8Path.h"

#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief Base64 标准字符表。
///
/// 协作协议使用 RFC 4648 标准字母表和 `=` 填充，不接受 URL-safe 变体。
constexpr std::string_view BASE64_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief 将字节编码为无换行 Base64 文本。
/// @param bytes 待编码 JPEG 字节。
/// @return 标准 Base64 文本；空输入返回空字符串。
///
/// 每三个输入字节组合为 24 位块并输出四个六位字符，尾块缺失字节使用 `=`。
/// 输出不插入换行，便于作为协作目录 JSON 字段传输。
std::string encodeBase64(const std::vector<unsigned char>& bytes)
{
    // 空字节序列没有合法封面含义，也避免把空载荷编码为成功结果。
    if ( bytes.empty() ) return {};
    std::string output;
    // Base64 长度固定为四乘以向上取整的三字节块数。
    output.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for ( std::size_t index = 0U; index < bytes.size(); index += 3U ) {
        // 尾块缺失的输入字节以零填充，只用于位组合，不写入解码结果。
        const std::uint32_t first = bytes[index];
        const std::uint32_t second =
            index + 1U < bytes.size() ? bytes[index + 1U] : 0U;
        const std::uint32_t third =
            index + 2U < bytes.size() ? bytes[index + 2U] : 0U;
        // 按网络阅读顺序把三个八位字节合成一个 24 位整数。
        const std::uint32_t block = (first << 16U) | (second << 8U) | third;
        output.push_back(BASE64_ALPHABET[(block >> 18U) & 0x3fU]);
        output.push_back(BASE64_ALPHABET[(block >> 12U) & 0x3fU]);
        output.push_back(index + 1U < bytes.size()
                             ? BASE64_ALPHABET[(block >> 6U) & 0x3fU]
                             : '=');
        output.push_back(
            index + 2U < bytes.size() ? BASE64_ALPHABET[block & 0x3fU] : '=');
    }
    return output;
}

/// @brief 返回 Base64 字符对应的 6 位值。
/// @param character 待解释的单个 ASCII 字符。
/// @return 合法字母表索引；填充及非法字符返回 -1。
int decodeBase64Character(char character)
{
    if ( character >= 'A' && character <= 'Z' ) return character - 'A';
    if ( character >= 'a' && character <= 'z' ) {
        return character - 'a' + 26;
    }
    if ( character >= '0' && character <= '9' ) {
        return character - '0' + 52;
    }
    if ( character == '+' ) return 62;
    if ( character == '/' ) return 63;
    // `=` 只由块级解码器按位置处理，不能当作普通六位字符。
    return -1;
}

/// @brief 严格解码一段无空白 Base64 文本。
/// @param input 无换行标准 Base64 数据。
/// @return 解码字节；格式非法、为空或超过协议上限时返回空向量。
///
/// 严格模式拒绝中间填充、单独第三位填充、空白和非标准字母。输入长度在分配
/// 前检查，避免远端目录数据诱导超出封面协议预算的内存增长。
std::vector<unsigned char> decodeBase64(std::string_view input)
{
    // 标准 Base64 长度必须为四的倍数，并受封面协议文本上限约束。
    if ( input.empty() || input.size() % 4U != 0U ||
         input.size() > COLLABORATION_ROOM_COVER_BASE64_MAX_BYTES ) {
        return {};
    }

    std::vector<unsigned char> output;
    // 预留最大解码长度；尾部填充只会使实际结果更短一到两字节。
    output.reserve((input.size() / 4U) * 3U);
    for ( std::size_t index = 0U; index < input.size(); index += 4U ) {
        // 只有最后一个四字符块允许出现 `=` 填充。
        const bool lastBlock = index + 4U == input.size();
        const int  first     = decodeBase64Character(input[index]);
        const int  second    = decodeBase64Character(input[index + 1U]);
        const int  third     = input[index + 2U] == '='
                                   ? 0
                                   : decodeBase64Character(input[index + 2U]);
        const int  fourth    = input[index + 3U] == '='
                                   ? 0
                                   : decodeBase64Character(input[index + 3U]);
        if ( first < 0 || second < 0 || third < 0 || fourth < 0 ||
             (!lastBlock &&
              (input[index + 2U] == '=' || input[index + 3U] == '=')) ||
             (input[index + 2U] == '=' && input[index + 3U] != '=') ) {
            // 任一字符或填充位置异常都使整个载荷无效，不返回部分图像字节。
            return {};
        }

        // 将四个六位值重新组合为至多三个八位输出字节。
        const std::uint32_t block =
            (static_cast<std::uint32_t>(first) << 18U) |
            (static_cast<std::uint32_t>(second) << 12U) |
            (static_cast<std::uint32_t>(third) << 6U) |
            static_cast<std::uint32_t>(fourth);
        output.push_back(static_cast<unsigned char>((block >> 16U) & 0xffU));
        // 第三字符填充代表尾块只有一个原始字节。
        if ( input[index + 2U] != '=' ) {
            output.push_back(static_cast<unsigned char>((block >> 8U) & 0xffU));
        }
        if ( input[index + 3U] != '=' ) {
            // 第四字符非填充时尾块包含完整第三字节。
            output.push_back(static_cast<unsigned char>(block & 0xffU));
        }
    }
    return output;
}

/// @brief 对源图做居中裁剪与双线性缩放，输出固定尺寸 RGB8。
/// @param source stb 解码得到的 RGBA8 像素。
/// @param sourceWidth 源图宽度。
/// @param sourceHeight 源图高度。
/// @return 固定协议尺寸、三通道 RGB8 的像素数组。
///
/// 先按目标宽高比计算最大居中裁剪框，再以像素中心映射执行双线性采样。Alpha
/// 不写入 JPEG，三个颜色通道分别插值。调用方已验证正尺寸和有效源指针。
///
/// 缩放不变量：
/// - 宽图只裁左右，高图只裁上下；
/// - 裁剪区域始终位于源图中心；
/// - 目标像素中心映射到源裁剪框连续坐标；
/// - 相邻采样坐标始终钳制在源图边界；
/// - fx 与 fy 分别作为水平和垂直插值权重；
/// - 三个 RGB 通道独立执行同一双线性公式；
/// - 结果四舍五入并限制到八位颜色范围。
/// @warning 用户选择封面时执行像素级循环；不得从 UI 每帧调用。
std::vector<unsigned char> resizeCover(const unsigned char* source,
                                       int sourceWidth, int sourceHeight)
{
    // 算法只读取 source，在完成输出前调用方必须保持 stb 缓冲有效。
    // 目标比例来自协议固定尺寸，客户端与目录服务必须保持一致。
    constexpr float TARGET_ASPECT =
        static_cast<float>(COLLABORATION_ROOM_COVER_WIDTH) /
        static_cast<float>(COLLABORATION_ROOM_COVER_HEIGHT);
    const float sourceAspect =
        static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
    float cropWidth  = static_cast<float>(sourceWidth);
    float cropHeight = static_cast<float>(sourceHeight);
    if ( sourceAspect > TARGET_ASPECT ) {
        // 源图更宽时裁掉左右边缘，完整保留垂直范围。
        cropWidth = cropHeight * TARGET_ASPECT;
    } else {
        // 源图更高时裁掉上下边缘，完整保留水平范围。
        cropHeight = cropWidth / TARGET_ASPECT;
    }
    // 两轴偏移均取差值一半，保证裁剪区域围绕源图中心。
    const float cropLeft = (static_cast<float>(sourceWidth) - cropWidth) * 0.5F;
    const float cropTop =
        (static_cast<float>(sourceHeight) - cropHeight) * 0.5F;

    // 输出只包含 RGB 三通道，大小由协议常量完整确定。
    std::vector<unsigned char> output(
        static_cast<std::size_t>(COLLABORATION_ROOM_COVER_WIDTH) *
        COLLABORATION_ROOM_COVER_HEIGHT * 3U);
    for ( std::uint32_t y = 0U; y < COLLABORATION_ROOM_COVER_HEIGHT; ++y ) {
        // 目标像素中心反向映射到裁剪框，并减半像素对齐源像素中心。
        const float sourceY =
            cropTop +
            (static_cast<float>(y) + 0.5F) * cropHeight /
                static_cast<float>(COLLABORATION_ROOM_COVER_HEIGHT) -
            0.5F;
        const int y0 = std::clamp(
            static_cast<int>(std::floor(sourceY)), 0, sourceHeight - 1);
        const int y1 = std::min(y0 + 1, sourceHeight - 1);
        // fy 是上下两个采样行之间的插值权重。
        const float fy =
            std::clamp(sourceY - static_cast<float>(y0), 0.0F, 1.0F);
        for ( std::uint32_t x = 0U; x < COLLABORATION_ROOM_COVER_WIDTH; ++x ) {
            // 水平方向使用与纵向相同的像素中心映射规则。
            const float sourceX =
                cropLeft +
                (static_cast<float>(x) + 0.5F) * cropWidth /
                    static_cast<float>(COLLABORATION_ROOM_COVER_WIDTH) -
                0.5F;
            const int x0 = std::clamp(
                static_cast<int>(std::floor(sourceX)), 0, sourceWidth - 1);
            const int x1 = std::min(x0 + 1, sourceWidth - 1);
            // fx 被钳制以处理裁剪框恰好落在源图最外像素的边界。
            const float fx =
                std::clamp(sourceX - static_cast<float>(x0), 0.0F, 1.0F);
            const std::size_t destination =
                (static_cast<std::size_t>(y) * COLLABORATION_ROOM_COVER_WIDTH +
                 x) *
                3U;
            for ( std::size_t channel = 0U; channel < 3U; ++channel ) {
                // sample 按 RGBA 源步长读取当前 RGB 通道，忽略 alpha。
                const auto sample = [&](int sampleX, int sampleY) {
                    return static_cast<float>(
                        source[(static_cast<std::size_t>(sampleY) *
                                    static_cast<std::size_t>(sourceWidth) +
                                static_cast<std::size_t>(sampleX)) *
                                   4U +
                               channel]);
                };
                const float top =
                    // 先分别在上、下采样行进行水平线性插值。
                    sample(x0, y0) * (1.0F - fx) + sample(x1, y0) * fx;
                const float bottom =
                    sample(x0, y1) * (1.0F - fx) + sample(x1, y1) * fx;
                // 再做纵向插值、四舍五入并钳制到八位颜色范围。
                output[destination + channel] = static_cast<unsigned char>(
                    std::clamp(std::lround(top * (1.0F - fy) + bottom * fy),
                               0L,
                               255L));
            }
        }
    }
    return output;
}

/// @brief stb 图片写回调，把 JPEG 字节附加到向量。
/// @param context 指向目标字节向量的上下文。
/// @param data stb 本次输出的字节块。
/// @param size 字节块长度。
/// @warning 只由 stbi_write_jpg_to_func 同步调用，context 必须在编码期间有效。
void appendEncodedImage(void* context, void* data, int size)
{
    // 无效回调参数不修改输出，stb 最终会通过编码返回值报告失败。
    if ( !context || !data || size <= 0 ) return;
    auto&       output = *static_cast<std::vector<unsigned char>*>(context);
    const auto* begin  = static_cast<const unsigned char*>(data);
    // 回调可能分多块到达，按顺序追加以恢复完整 JPEG 字节流。
    output.insert(output.end(), begin, begin + size);
}
}  // namespace

/// @brief 验证解码封面是否满足固定尺寸 RGBA8 契约。
/// @return 尺寸和像素字节数完全匹配时返回 true。
DecodedCollaborationRoomCoverImage::operator bool() const
{
    return width == COLLABORATION_ROOM_COVER_WIDTH &&
           height == COLLABORATION_ROOM_COVER_HEIGHT &&
           pixels.size() == static_cast<std::size_t>(width) * height * 4U;
}

/// @brief 读取、裁剪并编码协作房间封面。
/// @param sourcePath 用户选择的本地图像文件。
/// @return Base64 JPEG 及明确错误码。
///
/// 文件先由 stb 解码为 RGBA，再居中裁剪到协议尺寸 RGB，编码为质量 72 JPEG，
/// 最后转为 Base64 并检查传输上限。每个失败阶段返回不同错误码。
///
/// 编码边界：
/// - 输入必须是当前可访问的普通文件；
/// - stb 必须返回正宽高的解码缓冲；
/// - resizeCover 始终输出固定宽高的 RGB8；
/// - JPEG 写回调必须产生非空字节；
/// - Base64 文本不得超过协作协议上限；
/// - 返回成功时 error 必须为 None 且 data 非空。
/// - 原始 stb 缓冲在缩放完成后立即释放；
/// - 中间 RGB 与 JPEG 数据都由局部 vector 管理；
/// - 任一失败返回时不泄漏 stb 分配的像素内存。
/// @warning 用户触发的低频路径：包含文件读取、像素缩放和 JPEG 编码。
EncodedCollaborationRoomCoverImage encodeCollaborationRoomCoverImage(
    const std::filesystem::path& sourcePath)
{
    // 使用 error_code 文件查询，权限或路径错误不会抛出异常。
    std::error_code pathError;
    if ( sourcePath.empty() ||
         !std::filesystem::is_regular_file(sourcePath, pathError) ||
         pathError ) {
        return { {}, CollaborationRoomCoverImageError::FileUnavailable };
    }

    // stb 文件接口接受 UTF-8 字符串，由项目路径工具统一转换。
    const std::string utf8Path     = Config::pathToUtf8(sourcePath);
    int               sourceWidth  = 0;
    int               sourceHeight = 0;
    int               channels     = 0;
    unsigned char*    source       = stbi_load(utf8Path.c_str(),
                                               &sourceWidth,
                                               &sourceHeight,
                                               &channels,
                                               STBI_rgb_alpha);
    if ( !source || sourceWidth <= 0 || sourceHeight <= 0 ) {
        // 防御 stb 在异常尺寸下仍返回缓冲区，所有权仍必须释放。
        if ( source ) stbi_image_free(source);
        return { {}, CollaborationRoomCoverImageError::DecodeFailed };
    }

    // resizeCover 完成后即可释放原始 RGBA 大图，降低后续编码峰值占用。
    std::vector<unsigned char> resized =
        resizeCover(source, sourceWidth, sourceHeight);
    stbi_image_free(source);

    // 常见封面预留 48 KiB，实际大小仍由 stb 回调按需扩展。
    std::vector<unsigned char> jpeg;
    jpeg.reserve(48U * 1024U);
    // 固定质量在视觉细节与目录传输负载之间保持跨客户端一致。
    constexpr int JPEG_QUALITY = 72;
    if ( stbi_write_jpg_to_func(
             &appendEncodedImage,
             &jpeg,
             static_cast<int>(COLLABORATION_ROOM_COVER_WIDTH),
             static_cast<int>(COLLABORATION_ROOM_COVER_HEIGHT),
             3,
             resized.data(),
             JPEG_QUALITY) == 0 ) {
        return { {}, CollaborationRoomCoverImageError::EncodeFailed };
    }

    // 最终检查基于传输文本大小，而不是压缩前像素或 JPEG 字节数。
    std::string base64 = encodeBase64(jpeg);
    if ( base64.empty() ||
         base64.size() > COLLABORATION_ROOM_COVER_BASE64_MAX_BYTES ) {
        return { {}, CollaborationRoomCoverImageError::PayloadTooLarge };
    }
    return { std::move(base64), CollaborationRoomCoverImageError::None };
}

/// @brief 将目录服务提供的 Base64 封面解码为固定尺寸 RGBA8。
/// @param base64 远端封面传输文本。
/// @return 有效像素；任一校验失败返回空对象。
///
/// Base64 严格解码后先用 stbi_info 验证尺寸，再实际分配像素缓冲，避免任意大图
/// 伪装成封面消耗内存。最终统一请求 RGBA8，供纹理上传直接使用。
///
/// 解码边界：
/// - 文本必须使用标准 Base64 且长度是四的倍数；
/// - 填充只能位于最后一块并符合一个或两个字节尾块规则；
/// - 编码字节数必须能够安全转换为 stb 的 int 参数；
/// - 图片头声明尺寸必须与协议封面尺寸完全一致；
/// - 完整解码必须成功并返回 RGBA8 缓冲；
/// - 返回对象的 bool 转换会再次校验尺寸与字节数。
DecodedCollaborationRoomCoverImage decodeCollaborationRoomCoverImage(
    std::string_view base64)
{
    // stb 长度参数为 int，转换前显式排除超出范围的输入。
    const std::vector<unsigned char> encoded = decodeBase64(base64);
    if ( encoded.empty() ||
         encoded.size() >
             static_cast<std::size_t>(std::numeric_limits<int>::max()) ) {
        return {};
    }

    int width    = 0;
    int height   = 0;
    int channels = 0;
    // 先读头部尺寸，任何非协议宽高都不进入完整图片解码。
    if ( stbi_info_from_memory(encoded.data(),
                               static_cast<int>(encoded.size()),
                               &width,
                               &height,
                               &channels) == 0 ||
         width != static_cast<int>(COLLABORATION_ROOM_COVER_WIDTH) ||
         height != static_cast<int>(COLLABORATION_ROOM_COVER_HEIGHT) ) {
        return {};
    }

    // 强制四通道输出，使结果字节数和纹理格式保持稳定。
    unsigned char* pixels =
        stbi_load_from_memory(encoded.data(),
                              static_cast<int>(encoded.size()),
                              &width,
                              &height,
                              &channels,
                              STBI_rgb_alpha);
    if ( !pixels ) return {};

    // 将 stb 缓冲复制进 RAII 向量后立即释放原始内存。
    DecodedCollaborationRoomCoverImage result;
    result.width  = static_cast<std::uint32_t>(width);
    result.height = static_cast<std::uint32_t>(height);
    result.pixels.assign(pixels,
                         pixels + static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(height) * 4U);
    stbi_image_free(pixels);
    return result;
}
}  // namespace MMM::UI
