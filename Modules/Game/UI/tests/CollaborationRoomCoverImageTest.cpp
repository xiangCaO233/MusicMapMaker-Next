#include "ui/imgui/manager/CollaborationRoomCoverImage.h"
#include "ui/imgui/manager/CollaborationDefaultFollowPolicy.h"

#include "config/Utf8Path.h"

#include <filesystem>

/// @file CollaborationRoomCoverImageTest.cpp
/// @brief 协作房卡封面的编码解码边界与默认字段跟随策略测试。
/// @details CTest 提供真实图片路径用于往返验证；缺失文件和损坏 Base64 场景
/// 不写入项目，默认值策略则在编译期完成校验。

namespace
{
/// @brief 验证真实图片可生成并往返解码为固定尺寸房卡封面。
/// @param sourcePath CTest 提供的可解码图片路径。
/// @return 编码受大小限制且解码尺寸符合协议时返回 true。
bool testImageRoundTrip(const std::filesystem::path& sourcePath)
{
    // 编码阶段同时执行读取、缩放和 Base64 封装。
    const auto encoded = MMM::UI::encodeCollaborationRoomCoverImage(sourcePath);
    // 协议字段具有硬上限，空结果也不能作为有效封面发送。
    if ( encoded.error != MMM::UI::CollaborationRoomCoverImageError::None ||
         encoded.base64.empty() ||
         encoded.base64.size() >
             MMM::UI::COLLABORATION_ROOM_COVER_BASE64_MAX_BYTES ) {
        return false;
    }
    // 解码后必须恢复协议规定的统一房卡尺寸。
    const auto decoded =
        MMM::UI::decodeCollaborationRoomCoverImage(encoded.base64);
    return static_cast<bool>(decoded) &&
           decoded.width == MMM::UI::COLLABORATION_ROOM_COVER_WIDTH &&
           decoded.height == MMM::UI::COLLABORATION_ROOM_COVER_HEIGHT;
}

/// @brief 验证缺失文件与无效 Base64 不会生成目录封面。
/// @param sourcePath 用于构造同目录缺失路径的有效图片路径。
/// @return 两类输入均被拒绝时返回 true。
bool testInvalidInputs(const std::filesystem::path& sourcePath)
{
    // 缺失路径应在读取阶段返回明确错误，而不是空的成功结果。
    const auto missing = MMM::UI::encodeCollaborationRoomCoverImage(
        sourcePath.parent_path() / "missing-room-cover.png");
    // 非 Base64 文本必须在解码边界失败。
    return missing.error ==
               MMM::UI::CollaborationRoomCoverImageError::FileUnavailable &&
           !MMM::UI::decodeCollaborationRoomCoverImage("not-base64");
}

/// @brief 验证房间字段仅在未自定义时跟随谱面默认值。
/// @return Follow 与 Custom 的布尔策略及文本推断均符合预期时返回 true。
constexpr bool testDefaultFollowPolicy()
{
    // 显式 Follow 跟随谱面更新，Custom 保留用户输入。
    using MMM::UI::CollaborationDefaultMode;
    // 文本等于默认值时可继续跟随，差异文本视为用户自定义。
    return MMM::UI::shouldFollowCollaborationDefault(
               CollaborationDefaultMode::Follow) &&
           !MMM::UI::shouldFollowCollaborationDefault(
               CollaborationDefaultMode::Custom) &&
           MMM::UI::resolveCollaborationTextDefaultMode("Map A", "Map A") ==
               CollaborationDefaultMode::Follow &&
           MMM::UI::resolveCollaborationTextDefaultMode("My Room", "Map A") ==
               CollaborationDefaultMode::Custom;
}
}  // namespace

/// @brief 运行协作房卡封面和默认值跟随策略回归测试。
/// @param argc 必须包含一个测试图片路径。
/// @param argv argv[1] 为 UTF-8 编码的图片路径。
/// @return 0 表示全部场景通过，1 表示图片场景失败，2 表示参数无效。
int main(int argc, char** argv)
{
    // 缺少资源路径时不尝试文件访问。
    if ( argc != 2 || !argv[1] ) return 2;
    // UTF-8 边界转换保证 Windows 与 Linux 路径构造一致。
    const std::filesystem::path sourcePath = MMM::Config::utf8ToPath(argv[1]);
    // 默认值策略为纯 constexpr，编译期即可阻止回归。
    static_assert(testDefaultFollowPolicy());
    return testImageRoundTrip(sourcePath) && testInvalidInputs(sourcePath) ? 0
                                                                           : 1;
}
