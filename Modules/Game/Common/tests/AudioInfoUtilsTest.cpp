#include "common/AudioInfoUtils.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{

/// @brief 报告一次媒体标签断言的结果。
/// @param condition 断言是否成立。
/// @param description 失败时供定位的场景描述。
/// @return 原样返回 condition，便于累计多个独立结果。
bool check(bool condition, std::string_view description)
{
    // 探测失败也继续检查另一个资源，让回归输出展示完整缺失范围。
    if ( !condition ) std::cerr << "FAIL: " << description << '\n';
    return condition;
}

}  // namespace

/// @brief 用真实 Ogg 资源验证流级标签读取和无标签回退。
/// @param argc 必须包含两个音频资源路径。
/// @param argv 先是含标题/艺术家/专辑标签的 Ogg，再是无标签 Ogg。
/// @return 所有探测和字段断言成功时返回零。
int main(int argc, char* argv[])
{
    // 两份输入由 CTest 显式提供，避免测试依赖进程工作目录。
    if ( argc != 3 ) return EXIT_FAILURE;

    // 使用正式日志生命周期，探测器输出的路径诊断不会访问失效 logger。
    XLogger::init("AudioInfoUtilsTest");
    bool ok = true;

    // CTest 参数为 UTF-8，经过统一转换才能在 Windows 打开含中文的夹具路径。
    // 此资源的三个标签在音频流上，不在容器上；旧探针会退回文件名。
    const auto tagged = MMM::Utils::AudioInfoUtils::probeAudioInfo(
        MMM::Config::utf8ToPath(argv[1]));
    ok &= check(tagged.has_value(), "tagged Ogg can be probed");
    if ( tagged ) {
        // 这些文本来自夹具已有标签；艺术家值虽不理想，仍须忠实读取。
        // 若旧实现只读容器，此处会得到文件名和空艺术家。
        ok &= check(tagged->title == "09 Don't Fight The Music",
                    "stream title is preserved");
        ok &= check(tagged->artist == "VideoProc Converter",
                    "stream artist is preserved");
        ok &= check(tagged->album == "ONGEKI Sound Collection 05",
                    "stream album is preserved");
        // 时长仍来自音频帧和采样率，标签修复不应破坏媒体时长。
        ok &= check(tagged->duration > 0.0, "tagged Ogg has duration");
    }

    // 同一探测器处理无标签文件时，标题只回退为文件名，其他字段保持空。
    const auto untagged = MMM::Utils::AudioInfoUtils::probeAudioInfo(
        MMM::Config::utf8ToPath(argv[2]));
    ok &= check(untagged.has_value(), "untagged Ogg can be probed");
    if ( untagged ) {
        // 缺失信息不能沿用上一份已探测文件的标题、艺术家或专辑。
        ok &= check(untagged->title == "audio", "missing title uses filename");
        ok &= check(untagged->artist.empty(), "missing artist stays empty");
        ok &= check(untagged->album.empty(), "missing album stays empty");
    }

    // 先关闭日志再返回，保留所有断言失败的非零退出码给 CTest。
    XLogger::shutdown();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
