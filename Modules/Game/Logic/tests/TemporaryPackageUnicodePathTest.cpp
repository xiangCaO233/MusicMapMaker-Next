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
/// @note 实际解包并打开临时项目，输出目录由控制器创建，不写入共享资源目录。
/// @note 只验证指定旧编码样本，不代表所有 ZIP 编码组合均已覆盖。
/// @note 项目清单只检查存在性，不在这里重复校验其 JSON 内容。
bool testUnicodePathExtraField(const std::filesystem::path& packagePath)
{
    auto& controller = MMM::Logic::ProjectController::instance();
    // 走生产临时项目入口，同时覆盖解压文件名与后续项目存储初始化。
    auto opened = controller.openTemporaryProjectPackage(packagePath);
    if ( !opened.m_opened ) {
        XERROR("Failed to open Unicode-path MCZ");
        return false;
    }

    // 后续检查与清理只针对本次返回的临时项目目录，不能使用原始包所在目录。
    const auto cacheRoot = opened.m_actualProjectPath;
    // 通过公共 UTF-8 路径转换构造预期文件名，避免依赖本机窄字符代码页。
    const auto expectedBeatmap =
        cacheRoot / MMM::Config::utf8ToPath("骤雨の狭間_6k_hd.mc");
    std::error_code filesystemError;
    // 必须存在完整 Unicode 文件名的普通文件，仅目录存在不足以证明解码正确。
    const bool beatmapExtracted =
        std::filesystem::is_regular_file(expectedBeatmap, filesystemError) &&
        !filesystemError;
    // 两次查询独立记录错误，不能把第一次的状态误用于项目清单检查。
    filesystemError.clear();
    const bool configurationSaved =
        std::filesystem::is_regular_file(cacheRoot / ".mmm" / "manifest.json",
                                         filesystemError) &&
        !filesystemError;

    // 关闭项目后再删除缓存，避免控制器或文件监视器仍使用待清理目录。
    const auto closed = controller.closeProject();

    // 查询结果先保存在布尔值中，清理完成后仍可分别报告哪一项验证失败。
    filesystemError.clear();
    // 删除范围是控制器为本次解包创建的叶目录，原始 MCZ 测试资源保留不变。
    std::filesystem::remove_all(cacheRoot, filesystemError);
    if ( !beatmapExtracted ) {
        XERROR("Unicode-path MCZ did not preserve its beatmap filename");
        return false;
    }
    if ( !configurationSaved ) {
        // 文件名正确但项目清单未落盘仍算失败，不能只验收解压阶段。
        XERROR("Unicode-path MCZ did not finish saving project storage");
        return false;
    }
    if ( !closed.m_closed ) {
        // 打开成功与关闭成功分别检查，防止生命周期尾部错误被清理结果掩盖。
        XERROR("Unicode-path MCZ project was not closed after the test");
        return false;
    }
    if ( filesystemError ) {
        // 临时输出清理失败也使测试失败，不静默积累解压目录。
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
/// @note 参数指向共享资源根而非具体 MCZ，目录结构与其它模块回归资源一致。
int main(int argc, char** argv)
{
    if ( argc != 2 ) {
        // 缺少资源位置时明确失败，不依赖当前工作目录猜测测试文件。
        XERROR("TemporaryPackageUnicodePathTest requires the resource root");
        return 1;
    }

    // 固定样本含旧编码与 Unicode Path 扩展字段，普通 UTF-8 新包不能替代此回归。
    const auto packagePath =
        std::filesystem::path(argv[1]) / "ma" /
        MMM::Config::utf8ToPath("骤雨の狭間 - Silentroom.mcz");
    // 测试只接收资源路径，未提供选择任意删除目标的命令行入口。
    // 所有临时文件生命周期都由打开结果中的实际缓存路径限定。
    // 将验证结果转换为退出码，便于直接运行与 CTest 使用同一验收逻辑。
    return testUnicodePathExtraField(packagePath) ? 0 : 1;
}
