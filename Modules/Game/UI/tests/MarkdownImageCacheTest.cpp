#include "ui/imgui/markdown/MarkdownImageCache.h"
#include <array>
#include <string>

/// @brief 验证地址策略和两帧 GIF 的解码、时间变化及非法资源回退。
int main(int argc, char** argv)
{
    using namespace MMM::UI;
    // 旧预编译库尚未重建时，可单独验证静态图链路；默认仍要求 GIF 测试通过。
    const bool staticOnly = argc > 1 && std::string(argv[1]) == "--static-only";
    if ( resolveUpdateImageUrl("/download/a.gif") !=
             "https://mmm.xiang233.top/download/a.gif" ||
         resolveUpdateImageUrl("file:///etc/passwd") != "" ||
         resolveUpdateImageUrl("javascript:alert(1)") != "" ||
         resolveUpdateImageUrl("a.png") !=
             "https://mmm.xiang233.top/download/check/a.png" )
        return 1;
    // 两帧分别为黑、白像素；每帧 100 ms，资源由测试源码内联生成。
    constexpr unsigned char GIF[] = {
        'G', 'I', 'F',  '8', '9',  'a',  1, 0, 1,  0,    0x80, 0, 0,    0,    0,
        0,   255, 255,  255, 0x21, 0xf9, 4, 0, 10, 0,    0,    0, 0x2c, 0,    0,
        0,   0,   1,    0,   1,    0,    0, 2, 2,  0x44, 1,    0, 0x21, 0xf9, 4,
        0,   10,  0,    0,   0,    0x2c, 0, 0, 0,  0,    1,    0, 1,    0,    0,
        2,   2,   0x4c, 1,   0,    0x3b
    };
    if ( !staticOnly ) {
        auto image = decodeUpdateImage(GIF);
        if ( image.pixels.empty() || image.frames < 2U ||
             image.duration <= 0.0 ||
             image.pixels.front() == image.pixels[(image.frames - 1U) * 4U] )
            return 2;
    }
    constexpr std::array<unsigned char, 4> INVALID{ 0, 1, 2, 3 };
    if ( !decodeUpdateImage(INVALID).pixels.empty() ) return 3;
    // 显式传入 URL 时才进行网络探针，常规测试完全离线。
    for ( int index = staticOnly ? 2 : 1; index < argc; ++index ) {
        const auto remote = loadUpdateImage(argv[index]);
        if ( remote.pixels.empty() || remote.width > 4096U ||
             remote.height > 4096U )
            return 4;
        if ( std::string(argv[index]).ends_with(".gif") && remote.frames < 2U )
            return 5;
    }
    return 0;
}
