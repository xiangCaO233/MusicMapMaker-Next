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
constexpr std::string_view BASE64_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief 将字节编码为无换行 Base64 文本。
std::string encodeBase64(const std::vector<unsigned char>& bytes)
{
    if ( bytes.empty() ) return {};
    std::string output;
    output.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for ( std::size_t index = 0U; index < bytes.size(); index += 3U ) {
        const std::uint32_t first = bytes[index];
        const std::uint32_t second =
            index + 1U < bytes.size() ? bytes[index + 1U] : 0U;
        const std::uint32_t third =
            index + 2U < bytes.size() ? bytes[index + 2U] : 0U;
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
    return -1;
}

/// @brief 严格解码一段无空白 Base64 文本。
std::vector<unsigned char> decodeBase64(std::string_view input)
{
    if ( input.empty() || input.size() % 4U != 0U ||
         input.size() > COLLABORATION_ROOM_COVER_BASE64_MAX_BYTES ) {
        return {};
    }

    std::vector<unsigned char> output;
    output.reserve((input.size() / 4U) * 3U);
    for ( std::size_t index = 0U; index < input.size(); index += 4U ) {
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
            return {};
        }

        const std::uint32_t block =
            (static_cast<std::uint32_t>(first) << 18U) |
            (static_cast<std::uint32_t>(second) << 12U) |
            (static_cast<std::uint32_t>(third) << 6U) |
            static_cast<std::uint32_t>(fourth);
        output.push_back(static_cast<unsigned char>((block >> 16U) & 0xffU));
        if ( input[index + 2U] != '=' ) {
            output.push_back(static_cast<unsigned char>((block >> 8U) & 0xffU));
        }
        if ( input[index + 3U] != '=' ) {
            output.push_back(static_cast<unsigned char>(block & 0xffU));
        }
    }
    return output;
}

/// @brief 对源图做居中裁剪与双线性缩放，输出固定尺寸 RGB8。
std::vector<unsigned char> resizeCover(const unsigned char* source,
                                       int sourceWidth, int sourceHeight)
{
    constexpr float TARGET_ASPECT =
        static_cast<float>(COLLABORATION_ROOM_COVER_WIDTH) /
        static_cast<float>(COLLABORATION_ROOM_COVER_HEIGHT);
    const float sourceAspect =
        static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
    float cropWidth  = static_cast<float>(sourceWidth);
    float cropHeight = static_cast<float>(sourceHeight);
    if ( sourceAspect > TARGET_ASPECT ) {
        cropWidth = cropHeight * TARGET_ASPECT;
    } else {
        cropHeight = cropWidth / TARGET_ASPECT;
    }
    const float cropLeft = (static_cast<float>(sourceWidth) - cropWidth) * 0.5F;
    const float cropTop =
        (static_cast<float>(sourceHeight) - cropHeight) * 0.5F;

    std::vector<unsigned char> output(
        static_cast<std::size_t>(COLLABORATION_ROOM_COVER_WIDTH) *
        COLLABORATION_ROOM_COVER_HEIGHT * 3U);
    for ( std::uint32_t y = 0U; y < COLLABORATION_ROOM_COVER_HEIGHT; ++y ) {
        const float sourceY =
            cropTop +
            (static_cast<float>(y) + 0.5F) * cropHeight /
                static_cast<float>(COLLABORATION_ROOM_COVER_HEIGHT) -
            0.5F;
        const int y0 = std::clamp(
            static_cast<int>(std::floor(sourceY)), 0, sourceHeight - 1);
        const int   y1 = std::min(y0 + 1, sourceHeight - 1);
        const float fy =
            std::clamp(sourceY - static_cast<float>(y0), 0.0F, 1.0F);
        for ( std::uint32_t x = 0U; x < COLLABORATION_ROOM_COVER_WIDTH; ++x ) {
            const float sourceX =
                cropLeft +
                (static_cast<float>(x) + 0.5F) * cropWidth /
                    static_cast<float>(COLLABORATION_ROOM_COVER_WIDTH) -
                0.5F;
            const int x0 = std::clamp(
                static_cast<int>(std::floor(sourceX)), 0, sourceWidth - 1);
            const int   x1 = std::min(x0 + 1, sourceWidth - 1);
            const float fx =
                std::clamp(sourceX - static_cast<float>(x0), 0.0F, 1.0F);
            const std::size_t destination =
                (static_cast<std::size_t>(y) * COLLABORATION_ROOM_COVER_WIDTH +
                 x) *
                3U;
            for ( std::size_t channel = 0U; channel < 3U; ++channel ) {
                const auto sample = [&](int sampleX, int sampleY) {
                    return static_cast<float>(
                        source[(static_cast<std::size_t>(sampleY) *
                                    static_cast<std::size_t>(sourceWidth) +
                                static_cast<std::size_t>(sampleX)) *
                                   4U +
                               channel]);
                };
                const float top =
                    sample(x0, y0) * (1.0F - fx) + sample(x1, y0) * fx;
                const float bottom =
                    sample(x0, y1) * (1.0F - fx) + sample(x1, y1) * fx;
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
void appendEncodedImage(void* context, void* data, int size)
{
    if ( !context || !data || size <= 0 ) return;
    auto&       output = *static_cast<std::vector<unsigned char>*>(context);
    const auto* begin  = static_cast<const unsigned char*>(data);
    output.insert(output.end(), begin, begin + size);
}
}  // namespace

DecodedCollaborationRoomCoverImage::operator bool() const
{
    return width == COLLABORATION_ROOM_COVER_WIDTH &&
           height == COLLABORATION_ROOM_COVER_HEIGHT &&
           pixels.size() == static_cast<std::size_t>(width) * height * 4U;
}

EncodedCollaborationRoomCoverImage encodeCollaborationRoomCoverImage(
    const std::filesystem::path& sourcePath)
{
    std::error_code pathError;
    if ( sourcePath.empty() ||
         !std::filesystem::is_regular_file(sourcePath, pathError) ||
         pathError ) {
        return { {}, CollaborationRoomCoverImageError::FileUnavailable };
    }

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
        if ( source ) stbi_image_free(source);
        return { {}, CollaborationRoomCoverImageError::DecodeFailed };
    }

    std::vector<unsigned char> resized =
        resizeCover(source, sourceWidth, sourceHeight);
    stbi_image_free(source);

    std::vector<unsigned char> jpeg;
    jpeg.reserve(48U * 1024U);
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

    std::string base64 = encodeBase64(jpeg);
    if ( base64.empty() ||
         base64.size() > COLLABORATION_ROOM_COVER_BASE64_MAX_BYTES ) {
        return { {}, CollaborationRoomCoverImageError::PayloadTooLarge };
    }
    return { std::move(base64), CollaborationRoomCoverImageError::None };
}

DecodedCollaborationRoomCoverImage decodeCollaborationRoomCoverImage(
    std::string_view base64)
{
    const std::vector<unsigned char> encoded = decodeBase64(base64);
    if ( encoded.empty() ||
         encoded.size() >
             static_cast<std::size_t>(std::numeric_limits<int>::max()) ) {
        return {};
    }

    int width    = 0;
    int height   = 0;
    int channels = 0;
    if ( stbi_info_from_memory(encoded.data(),
                               static_cast<int>(encoded.size()),
                               &width,
                               &height,
                               &channels) == 0 ||
         width != static_cast<int>(COLLABORATION_ROOM_COVER_WIDTH) ||
         height != static_cast<int>(COLLABORATION_ROOM_COVER_HEIGHT) ) {
        return {};
    }

    unsigned char* pixels =
        stbi_load_from_memory(encoded.data(),
                              static_cast<int>(encoded.size()),
                              &width,
                              &height,
                              &channels,
                              STBI_rgb_alpha);
    if ( !pixels ) return {};

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
