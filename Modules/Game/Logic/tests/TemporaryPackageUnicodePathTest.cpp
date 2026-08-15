#include "logic/ProjectController.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <filesystem>
#include <system_error>

namespace
{
/// @brief 校验含 Info-ZIP Unicode Path 的旧编码 MCZ 能按 UTF-8 文件名解包。
/// @param packagePath 回归测试使用的 MCZ 路径。
/// @return 解包结果和文件名均正确时返回 true。
bool testUnicodePathExtraField(const std::filesystem::path& packagePath)
{
    auto& controller = MMM::Logic::ProjectController::instance();
    auto  opened     = controller.openTemporaryProjectPackage(packagePath);
    if ( !opened.m_opened ) {
        XERROR("Failed to open Unicode-path MCZ");
        return false;
    }

    const auto cacheRoot = opened.m_actualProjectPath;
    const auto expectedBeatmap =
        cacheRoot / MMM::Config::utf8ToPath("骤雨の狭間_6k_hd.mc");
    std::error_code filesystemError;
    const bool      beatmapExtracted =
        std::filesystem::is_regular_file(expectedBeatmap, filesystemError) &&
        !filesystemError;
    filesystemError.clear();
    const bool configurationSaved =
        std::filesystem::is_regular_file(cacheRoot / ".mmm" / "manifest.json",
                                         filesystemError) &&
        !filesystemError;

    const auto closed = controller.closeProject();

    filesystemError.clear();
    std::filesystem::remove_all(cacheRoot, filesystemError);
    if ( !beatmapExtracted ) {
        XERROR("Unicode-path MCZ did not preserve its beatmap filename");
        return false;
    }
    if ( !configurationSaved ) {
        XERROR("Unicode-path MCZ did not finish saving project storage");
        return false;
    }
    if ( !closed.m_closed ) {
        XERROR("Unicode-path MCZ project was not closed after the test");
        return false;
    }
    if ( filesystemError ) {
        XERROR("Failed to remove temporary Unicode-path MCZ directory: {}",
               filesystemError.message());
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行临时谱面包 Unicode 路径兼容回归测试。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数；第二项为共享测试资源目录。
/// @return 测试通过时返回 0。
int main(int argc, char** argv)
{
    if ( argc != 2 ) {
        XERROR("TemporaryPackageUnicodePathTest requires the resource root");
        return 1;
    }

    const auto packagePath =
        std::filesystem::path(argv[1]) / "ma" /
        MMM::Config::utf8ToPath("骤雨の狭間 - Silentroom.mcz");
    return testUnicodePathExtraField(packagePath) ? 0 : 1;
}
