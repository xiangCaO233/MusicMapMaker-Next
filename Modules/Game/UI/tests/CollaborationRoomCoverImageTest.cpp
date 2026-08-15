#include "ui/imgui/manager/CollaborationRoomCoverImage.h"

#include "config/Utf8Path.h"

#include <filesystem>

namespace
{
/// @brief 验证真实图片可生成并往返解码为固定尺寸房卡封面。
bool testImageRoundTrip(const std::filesystem::path& sourcePath)
{
    const auto encoded = MMM::UI::encodeCollaborationRoomCoverImage(sourcePath);
    if ( encoded.error != MMM::UI::CollaborationRoomCoverImageError::None ||
         encoded.base64.empty() ||
         encoded.base64.size() >
             MMM::UI::COLLABORATION_ROOM_COVER_BASE64_MAX_BYTES ) {
        return false;
    }
    const auto decoded =
        MMM::UI::decodeCollaborationRoomCoverImage(encoded.base64);
    return static_cast<bool>(decoded) &&
           decoded.width == MMM::UI::COLLABORATION_ROOM_COVER_WIDTH &&
           decoded.height == MMM::UI::COLLABORATION_ROOM_COVER_HEIGHT;
}

/// @brief 验证缺失文件与无效 Base64 不会生成目录封面。
bool testInvalidInputs(const std::filesystem::path& sourcePath)
{
    const auto missing = MMM::UI::encodeCollaborationRoomCoverImage(
        sourcePath.parent_path() / "missing-room-cover.png");
    return missing.error ==
               MMM::UI::CollaborationRoomCoverImageError::FileUnavailable &&
           !MMM::UI::decodeCollaborationRoomCoverImage("not-base64");
}
}  // namespace

/// @brief 运行协作房卡封面缩略图生成与解码回归测试。
int main(int argc, char** argv)
{
    if ( argc != 2 || !argv[1] ) return 2;
    const std::filesystem::path sourcePath = MMM::Config::utf8ToPath(argv[1]);
    return testImageRoundTrip(sourcePath) && testInvalidInputs(sourcePath) ? 0
                                                                           : 1;
}
