#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::UI
{
/// @brief 公网房卡封面固定宽度。
inline constexpr std::uint32_t COLLABORATION_ROOM_COVER_WIDTH = 320U;
/// @brief 公网房卡封面固定高度。
inline constexpr std::uint32_t COLLABORATION_ROOM_COVER_HEIGHT = 180U;
/// @brief 与目录协议一致的 Base64 文本上限。
inline constexpr std::size_t COLLABORATION_ROOM_COVER_BASE64_MAX_BYTES =
    96U * 1024U;

/// @brief 房卡封面生成失败类型。
enum class CollaborationRoomCoverImageError {
    None,
    FileUnavailable,
    DecodeFailed,
    EncodeFailed,
    PayloadTooLarge,
};

/// @brief 开房时发布到目录服务的封面缩略图。
struct EncodedCollaborationRoomCoverImage {
    /// @brief 不含 data URI 前缀的 Base64 JPEG 数据。
    std::string base64;
    /// @brief 生成结果错误；成功时为 None。
    CollaborationRoomCoverImageError error =
        CollaborationRoomCoverImageError::None;
};

/// @brief 已解码、可直接上传至 Vulkan 纹理的 RGBA8 房卡封面。
struct DecodedCollaborationRoomCoverImage {
    /// @brief 固定尺寸 RGBA8 像素。
    std::vector<unsigned char> pixels;
    /// @brief 像素宽度。
    std::uint32_t width = 0U;
    /// @brief 像素高度。
    std::uint32_t height = 0U;

    /// @brief 判断是否包含一张完整有效的固定尺寸封面。
    explicit operator bool() const;
};

/// @brief 从本地图片生成居中裁剪的 16:9 JPEG 房卡缩略图。
/// @param sourcePath 用户选择或当前谱面引用的图片文件。
/// @return 成功时包含 Base64 数据，否则包含稳定错误类型。
/// @warning
/// 用户低频选择路径：会同步读取并解码一个图片文件，禁止在每帧无条件调用。
[[nodiscard]] EncodedCollaborationRoomCoverImage
encodeCollaborationRoomCoverImage(const std::filesystem::path& sourcePath);

/// @brief 解码目录服务按需返回的 Base64 JPEG 房卡缩略图。
/// @param base64 不含 data URI 前缀的 Base64 数据。
/// @return 仅接受固定 320x180 图片；无效或过大数据返回空结果。
/// @warning GPU 资源准备低频路径：会分配并解码一张固定尺寸图片，禁止在 UI
/// 绘制热路径调用。
[[nodiscard]] DecodedCollaborationRoomCoverImage
decodeCollaborationRoomCoverImage(std::string_view base64);
}  // namespace MMM::UI
