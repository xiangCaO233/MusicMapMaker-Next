#include "config/skin/SkinPackageService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <miniz.h>
#include <string>
#include <utility>
#include <vector>

namespace
{

/// @brief 写入测试文本文件，并按需创建父目录。
/// @param path 输出文件路径。
/// @param text 要按原始字节保存的文本。
/// @return 目录创建及文件写入均成功时返回 true。
bool writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    // 夹具按真实皮肤布局分层创建，调用点无需重复准备目录。
    std::error_code createError;
    std::filesystem::create_directories(path.parent_path(), createError);
    if ( createError ) return false;

    // 二进制截断模式保持 Lua 和资源内容可进行精确往返比较。
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if ( !output ) return false;
    output << text;
    return output.good();
}

/// @brief 读取测试文本文件的完整内容。
/// @param path 待读取文件路径。
/// @return 文件内容；打开失败时返回空字符串。
std::string readTextFile(const std::filesystem::path& path)
{
    // 这里仅用于已知非空夹具，空返回值足以表示断言所需的失败状态。
    std::ifstream input(path, std::ios::binary);
    if ( !input ) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

/// @brief 创建包含指定条目的 ZIP 兼容 MSK 文件。
/// @param outputPath 输出包路径。
/// @param entries 包内相对名称及对应内容。
/// @return 归档初始化、写入和最终封装均成功时返回 true。
bool writeMskFile(
    const std::filesystem::path&                            outputPath,
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    // 使用与生产实现相同的 miniz 格式，但保留对危险条目名的完全控制。
    mz_zip_archive archive{};
    if ( !mz_zip_writer_init_file(
             &archive, MMM::Config::pathToUtf8(outputPath).c_str(), 0) ) {
        return false;
    }

    // 任一条目失败后停止写入，最终仍调用 end 释放 miniz 状态。
    bool success = true;
    for ( const auto& [name, content] : entries ) {
        // 测试数据直接以内存条目写入，无需额外生成中间目录树。
        if ( !mz_zip_writer_add_mem(&archive,
                                    name.c_str(),
                                    content.data(),
                                    content.size(),
                                    MZ_BEST_SPEED) ) {
            success = false;
            break;
        }
    }
    // 仅在全部条目成功时写中央目录，避免把残缺归档当成有效夹具。
    if ( success && !mz_zip_writer_finalize_archive(&archive) ) {
        success = false;
    }
    mz_zip_writer_end(&archive);
    return success;
}

/// @brief 记录布尔断言结果并累计失败数量。
/// @param condition 待验证条件。
/// @param label 日志中使用的稳定用例标签。
/// @return 条件成立返回 0，否则返回 1。
int expectTrue(bool condition, const char* label)
{
    if ( condition ) {
        XINFO("[skin-package] PASS: {}", label);
        return 0;
    }
    XERROR("[skin-package] FAIL: {}", label);
    return 1;
}

/// @brief 验证皮肤目录导出、导入、覆盖导出及同名保护。
/// @param root 本用例独占的测试根目录。
/// @return 失败断言总数。
int testExportAndImport(const std::filesystem::path& root)
{
    // source 模拟已安装皮肤，target 模拟用户的皮肤安装根。
    int        failures = 0;
    const auto source   = root / "source" / "FancySkin";
    const auto package  = root / "FancySkin.msk";
    const auto target   = root / "target";

    // 入口和嵌套资源共同验证递归枚举时保留相对目录结构。
    failures += expectTrue(
        writeTextFile(source / "skin.lua", "return { name = 'Fancy' }"),
        "write skin.lua fixture");
    failures +=
        expectTrue(writeTextFile(source / "resources" / "note.txt", "note"),
                   "write nested resource fixture");

    // 错误文本只作服务返回通道；成功场景以布尔结果和产物为准。
    std::string exportError;
    failures += expectTrue(MMM::Config::SkinPackageService::exportPackage(
                               source, package, exportError),
                           "export skin package");
    // 导出成功后包文件必须能被同一服务重新打开，间接校验归档收尾完整。

    // 导入刚导出的包，覆盖生产导出与生产导入之间的格式契约。
    const auto imported =
        MMM::Config::SkinPackageService::importPackage(package, target);
    failures += expectTrue(imported.success, "import exported skin package");
    // 成功标志之后继续校验名称和内容，避免只验证表面返回状态。
    // 顶层目录包必须沿用原目录名，不能退化为包文件名推导。
    failures += expectTrue(imported.skinDirectoryName == "FancySkin",
                           "preserve exported skin directory name");
    failures += expectTrue(readTextFile(target / "FancySkin" / "skin.lua") ==
                               "return { name = 'Fancy' }",
                           "restore skin.lua content");
    // 入口脚本和普通资源分别检查，防止实现只处理识别到的 skin.lua。
    failures += expectTrue(
        readTextFile(target / "FancySkin" / "resources" / "note.txt") == "note",
        "restore nested resource content");
    // 嵌套文件检查同时验证归档分隔符被正确转换为本机路径。

    // 第二次导入命中既有目录，服务应拒绝而不是静默覆盖用户文件。
    const auto duplicate =
        MMM::Config::SkinPackageService::importPackage(package, target);
    // 拒绝结果后再次读取资源，证明失败路径没有破坏既有安装。
    failures +=
        expectTrue(!duplicate.success, "reject duplicate skin directory");
    // 失败结果不能通过创建新的后缀目录规避同名保护。
    failures += expectTrue(
        readTextFile(target / "FancySkin" / "resources" / "note.txt") == "note",
        "preserve existing skin after duplicate import");

    // 修改源资源后复用同一包路径，验证导出采用安全覆盖语义。
    failures +=
        expectTrue(writeTextFile(source / "resources" / "note.txt", "updated"),
                   "update export fixture");
    failures += expectTrue(MMM::Config::SkinPackageService::exportPackage(
                               source, package, exportError),
                           "overwrite exported package");
    // 第二次导出应替换整个归档，不能在旧中央目录后追加重复条目。
    // 覆盖包文件是导出操作允许的行为，与导入目录禁止覆盖相互独立。
    // 导入到新目录排除旧安装内容干扰，直接检查新包中的更新字节。
    const auto updatedImport = MMM::Config::SkinPackageService::importPackage(
        package, root / "updated-target");
    failures +=
        expectTrue(updatedImport.success, "import overwritten skin package");
    // 新目标为空目录，因此任何同名冲突都表示导出包布局异常。
    failures += expectTrue(readTextFile(root / "updated-target" / "FancySkin" /
                                        "resources" / "note.txt") == "updated",
                           "overwrite package contains updated resource");
    // 精确内容断言确认读取的是新资源，而不是第一次导出的缓存结果。
    return failures;
}

/// @brief 验证根级 skin.lua 使用包文件名作为安装目录。
/// @param root 本用例独占的测试根目录。
/// @return 失败断言总数。
int testRootSkinLua(const std::filesystem::path& root)
{
    // 无顶层目录的包模拟第三方常见 ZIP 布局。
    const auto package  = root / "RootSkin.msk";
    const auto target   = root / "root-target";
    int        failures = 0;
    // skin.lua 与资源均位于归档根，导入时需要整体包裹到派生目录。
    failures += expectTrue(writeMskFile(package,
                                        { { "skin.lua", "return {}" },
                                          { "resources/icon.txt", "icon" } }),
                           "write root-level skin package");

    // RootSkin 来自去除 .msk 扩展后的文件名，而不是脚本元数据。
    const auto imported =
        MMM::Config::SkinPackageService::importPackage(package, target);
    failures += expectTrue(imported.success, "import root-level skin package");
    // 根布局成功导入后必须生成单一的 RootSkin 安装目录。
    failures += expectTrue(imported.skinDirectoryName == "RootSkin",
                           "derive root skin name from package");
    // 派生名称同时用于返回结果和落盘目录，两处语义必须一致。
    failures += expectTrue(
        readTextFile(target / "RootSkin" / "resources" / "icon.txt") == "icon",
        "copy root-level skin resources");
    // 根级资源导入后应位于派生目录内部，不能直接散落到 target。
    return failures;
}

/// @brief 验证危险路径和多入口皮肤包会被拒绝。
/// @param root 本用例独占的测试根目录。
/// @return 失败断言总数。
int testInvalidPackages(const std::filesystem::path& root)
{
    // 分别构造路径穿越与多入口歧义，两者都不能产生部分安装。
    int        failures     = 0;
    const auto unsafe       = root / "Unsafe.msk";
    const auto ambiguous    = root / "Ambiguous.msk";
    const auto unsafeTarget = root / "unsafe-target";

    // 危险条目排在合法入口之前，验证预检覆盖整个归档而非边解压边校验。
    failures += expectTrue(writeMskFile(unsafe,
                                        { { "../evil.txt", "evil" },
                                          { "Safe/skin.lua", "return {}" } }),
                           "write unsafe package");
    // 导入失败后包外 evil.txt 必须不存在，证明没有越界写入。
    const auto unsafeResult =
        MMM::Config::SkinPackageService::importPackage(unsafe, unsafeTarget);
    failures +=
        expectTrue(!unsafeResult.success, "reject path traversal package");
    // 即使归档中另有合法 skin.lua，任何一个危险条目都应使整体失败。
    failures += expectTrue(!std::filesystem::exists(root / "evil.txt"),
                           "do not write path traversal entry");
    // 越界文件位置按规范化后的真实逃逸目标检查，防止只验证返回值。

    // 两个不同顶层目录都包含入口时无法确定唯一安装根，必须拒绝。
    failures += expectTrue(writeMskFile(ambiguous,
                                        { { "One/skin.lua", "return {}" },
                                          { "Two/skin.lua", "return {}" } }),
                           "write ambiguous package");
    const auto ambiguousResult = MMM::Config::SkinPackageService::importPackage(
        ambiguous, root / "ambiguous-target");
    // 多入口包不能任选其一，否则安装结果会依赖归档遍历顺序。
    failures += expectTrue(!ambiguousResult.success,
                           "reject multiple skin.lua entries");
    // 多入口包不能任选其一，否则安装结果会依赖归档遍历顺序。
    // 失败即为预期结果，两个候选目录均不应被视为有效安装。
    return failures;
}

}  // namespace

/// @brief 运行 MSK 皮肤包导入导出测试。
/// @param argc 参数数量。
/// @param argv 参数数组；首个参数为测试输出目录。
/// @return 所有检查通过时返回 0。
int main(int argc, char** argv)
{
    // CTest 必须提供专用输出路径，避免错误地在当前目录创建夹具。
    if ( argc < 2 || argv[1] == nullptr ) return 1;

    // 仅清理调用方明确传入的测试根，保证重复运行结果可复现。
    const std::filesystem::path root(argv[1]);
    std::error_code             removeError;
    std::filesystem::remove_all(root, removeError);
    // 清理失败并不立即终止；随后的目录创建会给出最终可用性判断。
    std::error_code createError;
    std::filesystem::create_directories(root, createError);
    if ( createError ) return 1;
    // 测试根预先存在后，生产服务仍需自行创建其下各安装目标。
    // 所有文件系统状态均局限在 root 内，结束后可由构建系统统一清理。

    // 三组职责独立的场景全部执行，以一次返回汇总所有失败。
    const int failures = testExportAndImport(root) + testRootSkinLua(root) +
                         testInvalidPackages(root);
    return failures == 0 ? 0 : 1;
}
