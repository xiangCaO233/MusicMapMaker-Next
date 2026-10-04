#include "ui/imgui/markdown/MarkdownImageCache.h"
#include "log/colorful-log.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

/// @file MarkdownImageCacheTest.cpp
/// @brief 更新图片 URL 策略、内存解码和可选网络探针回归测试。
/// @details 默认测试完全离线；仅调用方显式追加 URL 时才执行远程下载。
/// 静态模式用于依赖尚不支持动画时单独验证 URL 和非法输入边界。
/// 内联 GIF 避免测试依赖源码目录或外部资源文件。
/// 真实 CanonRock GIF 则单独验证分发资源的帧处理兼容性；资源只读。
/// 原始高清动画应进入有界内存路径，不依赖 FFmpeg 的 GIF 模块。

/// @brief 验证地址策略和两帧 GIF 的解码、时间变化及非法资源回退。
/// @param argc 可选模式或远程图片 URL 数量。
/// @param argv 可包含 --static-only 及远程 HTTP(S) URL。
/// @return 0 表示全部通过，非零值标识 URL、GIF、非法数据或网络场景失败。
int main(int argc, char** argv)
{
    using namespace MMM::UI;
    // 旧预编译库尚未重建时，可单独验证静态图链路；默认仍要求 GIF 测试通过。
    const bool staticOnly = argc > 1 && std::string(argv[1]) == "--static-only";
    // 根相对地址绑定官方站点，本地和脚本 scheme 必须拒绝，普通相对地址
    // 绑定更新检查目录。
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
        // GIF 解码应产生至少两帧、正时长，并保留黑白帧差异。
        auto image = decodeUpdateImage(GIF);
        if ( image.pixels.empty() || image.frames < 2U ||
             image.duration <= 0.0 ||
             image.pixels.front() == image.pixels[(image.frames - 1U) * 4U] )
            return 2;
    }
    // 无有效图片头的短字节序列必须返回空像素结果。
    constexpr std::array<unsigned char, 4> INVALID{ 0, 1, 2, 3 };
    if ( !decodeUpdateImage(INVALID).pixels.empty() ) return 3;
    // 本地教程 scheme 只接受打包目录内的单文件名；目录穿越在排队前
    // 就必须进入失败状态，不能由网络 URL 解析意外接管。
    MarkdownImageCache cache;
    cache.prepareImage("walkthrough-gif:place-tap.gif");
    cache.prepareImage("walkthrough-gif:../secret.gif");
    if ( cache.findImage("walkthrough-gif:place-tap.gif").failed ||
         !cache.findImage("walkthrough-gif:../secret.gif").failed )
        return 10;
    // 全部打包原图在 macOS 无 GIF FFmpeg 解码器时也必须走有界内存解码。
    // 逐张验收分辨率与帧 disposal，防止素材再次被预压缩或运行时缩小。
    if ( !staticOnly ) {
        std::error_code error;
        // 不跟随项目列表加载配置根；直接检查构建时打包的源码资产。
        const auto directory =
            std::filesystem::path(MMM_WALKTHROUGH_GIF_DIRECTORY);
        std::size_t count = 0;
        // 顺序无关：每个文件独立解码且在本轮迭代后释放图集内存。
        for ( std::filesystem::directory_iterator iterator(directory, error),
              end;
              !error && iterator != end;
              iterator.increment(error) ) {
            // 目录中的其他说明文件不属于动画；只验收 GIF。
            if ( iterator->path().extension() != ".gif" ) continue;
            std::ifstream source(iterator->path(), std::ios::binary);
            if ( !source ) return 6;
            // 读取原始 GIF 字节，确保测试经过与运行时相同的解码器。
            const std::vector<unsigned char> bytes{
                std::istreambuf_iterator<char>{ source },
                std::istreambuf_iterator<char>{}
            };
            const auto image =
                decodeUpdateImage(bytes, 1280U, 512U * 1024U * 1024U);
            // 打包源最窄仍有 948 像素；显示帧须保留 900 像素以上。
            // 每段图集限制在 128 MiB，切步时由缓存回收前一段纹理。
            if ( image.pixels.empty() || image.frames < 2U ||
                 image.frameWidth < 900U || image.frameHeight > 1280U ||
                 image.pixels.size() > 128U * 1024U * 1024U )
                return 7;
            ++count;
        }
        // 文件缺失和迭代失败都应显式失败，避免空目录得到假通过。
        // 数量断言与批注目录一致，新增段落时需要同步增加动画。
        // 这里不依赖文件遍历顺序，便于不同文件系统得到同一结果。
        if ( error || count != 14U ) return 8;
    }
    // 显式传入 URL 时才进行网络探针，常规测试完全离线。
    for ( int index = staticOnly ? 2 : 1; index < argc; ++index ) {
        // 远程探针仍验证解码结果非空且图集不超过实现尺寸边界。
        const auto remote = loadUpdateImage(argv[index]);
        if ( remote.pixels.empty() || remote.width > 4096U ||
             remote.height > 4096U )
            return 4;
        if ( std::string(argv[index]).ends_with(".gif") && remote.frames < 2U )
            // 显式 GIF URL 必须保留动画而非退化为单帧。
            return 5;
        // 网络探针同时请求与放大弹窗相同的高清解码路径，防止只验证缩略图。
        // 远程 GIF 可能走 FFmpeg 逐帧路径，不能以可信本地源的较高预算代替。
        const auto preview = loadUpdateImage(argv[index], 1280U);
        // 宽高分别检查，竖向图片不能只凭最长边判断是否退回了正文尺寸。
        // 单图字节上限也覆盖最后一行空槽，避免高清修复无意突破图集预算。
        if ( preview.pixels.empty() || preview.frameWidth < remote.frameWidth ||
             preview.frameHeight < remote.frameHeight ||
             preview.pixels.size() > 128U * 1024U * 1024U )
            return 11;
        // 记录两档的实际分辨率与容量，复查网络原图和客户端是否一致。
        // 此日志只由显式探针触发，正常离线 CTest 不访问站点。
        XINFO("更新图片探针: 缩略图 {}x{} / {} 字节，高清 {}x{} / {} 字节",
              remote.frameWidth,
              remote.frameHeight,
              remote.pixels.size(),
              preview.frameWidth,
              preview.frameHeight,
              preview.pixels.size());
    }
    return 0;
}
