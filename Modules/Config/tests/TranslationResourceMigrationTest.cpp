#include "config/skin/TranslationResourceMigration.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <filesystem>
#include <fstream>
#include <string_view>

namespace
{
/// @brief 记录测试断言失败。
/// @param condition 待验证条件。
/// @param message 条件失败时写入日志的说明。
/// @return 条件原值。
/// @warning 仅测试辅助函数，不改变文件系统状态。
bool check(bool condition, std::string_view message)
{
    // 统一记录失败上下文，同时把原条件返回给聚合断言链。
    if ( !condition ) {
        XERROR("TranslationResourceMigrationTest failed: {}", message);
    }
    return condition;
}

/// @brief 写入测试占位文件并创建父目录。
/// @param path 输出文件路径。
/// @param content 文件内容。
/// @return 文件成功写入时返回 true。
bool writeFile(const std::filesystem::path& path, std::string_view content)
{
    // 每个测试文件自行准备父目录，使场景声明不依赖创建顺序。
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if ( filesystemError ) return false;
    // 二进制截断模式保证 Lua 占位内容和标记文件完全可预测。
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    // 写入完成后检查流状态，调用方不会把部分文件当作有效夹具。
    file << content;
    return file.good();
}
}  // namespace

/// @brief 验证旧版皮肤语言文件的一次性无条件清理。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数；附加参数为测试输出目录。
/// @return 全部断言通过时返回 0。
/// @warning 测试会清理 argv[1] 指定目录，CMake 必须传入构建输出路径。
int main(int argc, char* argv[])
{
    // 输出根由 CTest 指向构建目录，禁止把生成文件写进 tests/data。
    if ( argc < 2 || !argv[1] ) {
        XERROR("TranslationResourceMigrationTest requires output path");
        return 1;
    }

    // 每次运行先移除自己的输出根，场景不继承上次完成标记或旧文件。
    const auto      outputRoot = MMM::Config::utf8ToPath(argv[1]);
    std::error_code filesystemError;
    std::filesystem::remove_all(outputRoot, filesystemError);
    // 清理失败状态不复用于后续写入辅助函数，避免错误码串扰。
    filesystemError.clear();

    // 主场景模拟两个皮肤、两种默认语言和一个必须保留的自定义语言。
    const auto skinsRoot        = outputRoot / "assets/skins";
    const auto translationsRoot = outputRoot / "assets/translations";
    const auto markerPath       = outputRoot / "assets/.translation-layout-v2";
    const auto firstEnglish     = skinsRoot / "first/resources/lang/en_us.lua";
    // 第一皮肤同时覆盖中英文删除，第二皮肤验证目录间遍历不会提前停止。
    const auto firstChinese  = skinsRoot / "first/resources/lang/zh_cn.lua";
    const auto secondChinese = skinsRoot / "second/resources/lang/zh_cn.lua";
    // 自定义语言与旧默认文件共处一层，验证删除白名单的精确性。
    const auto unrelatedFile = skinsRoot / "first/resources/lang/custom.lua";

    // 不完整场景单独使用子根，验证前置条件失败绝不会开始删除。
    const auto incompleteRoot = outputRoot / "incomplete";
    const auto incompleteLegacy =
        incompleteRoot / "skins/old/resources/lang/zh_cn.lua";
    bool ok =
        // 只创建英文新版基线，刻意缺少 zh_cn.lua。
        check(writeFile(incompleteRoot / "translations/en_us.lua", "return {}"),
              "不完整默认翻译测试文件写入失败");
    ok &= check(writeFile(incompleteLegacy, "return { keep = true }"),
                "不完整资源迁移的旧文件写入失败");
    const auto incompleteResult =
        MMM::Config::migrateLegacySkinTranslationFiles(
            incompleteRoot / "skins",
            incompleteRoot / "translations",
            incompleteRoot / ".migration-marker");
    // 迁移必须失败且旧中文文件仍存在，证明校验发生在删除之前。
    ok &= check(!incompleteResult.completed &&
                    std::filesystem::is_regular_file(incompleteLegacy),
                "新版默认翻译不完整时不得提前删除旧文件");

    // 主场景先建立完整新版基线，再创建所有旧布局文件。
    ok &= check(writeFile(translationsRoot / "en_us.lua", "return {}"),
                "英文默认翻译测试文件写入失败");
    ok &= check(writeFile(translationsRoot / "zh_cn.lua", "return {}"),
                "中文默认翻译测试文件写入失败");
    ok &= check(writeFile(firstEnglish, "return { modified = true }"),
                "第一份旧英文翻译写入失败");
    // 使用 modified 内容证明迁移按旧固定路径删除，而非比较默认文件内容。
    ok &= check(writeFile(firstChinese, "return { modified = true }"),
                "第一份旧中文翻译写入失败");
    ok &= check(writeFile(secondChinese, "return { modified = true }"),
                "第二份旧中文翻译写入失败");
    ok &= check(writeFile(unrelatedFile, "return { keep = true }"),
                "无关翻译文件写入失败");
    // 准备失败时不调用迁移，避免以残缺夹具产生误导断言。
    if ( !ok ) return 1;

    // 首次调用应执行实际遍历、删除和完成标记写入。
    const auto firstResult = MMM::Config::migrateLegacySkinTranslationFiles(
        skinsRoot, translationsRoot, markerPath);
    // 结果对象既报告整体完成，也列出供启动日志使用的实际删除项。
    ok &= check(firstResult.completed, "首次迁移必须成功");
    // 两个皮肤合计创建三份固定旧文件，因此删除清单长度必须精确为三。
    ok &= check(firstResult.removedFiles.size() == 3,
                "首次迁移必须删除所有已存在的固定路径语言文件");
    // 内容是否被用户修改不影响旧默认副本的无条件清理规则。
    ok &= check(!std::filesystem::exists(firstEnglish) &&
                    !std::filesystem::exists(firstChinese) &&
                    !std::filesystem::exists(secondChinese),
                "用户修改过的旧版默认语言文件也必须无条件删除");
    // custom.lua 位于相同目录，可检测实现是否误用宽泛扩展名清理。
    ok &= check(std::filesystem::is_regular_file(unrelatedFile),
                "迁移不得删除其他语言文件");
    // 标记是第二次调用跳过遍历的唯一持久化依据。
    ok &= check(std::filesystem::is_regular_file(markerPath),
                "首次迁移必须写入一次性完成标记");

    // 在迁移后重新创建同路径文件，模拟用户有意添加的新覆写。
    ok &= check(writeFile(firstChinese, "return { new_override = true }"),
                "迁移后同路径覆写文件写入失败");
    const auto secondResult = MMM::Config::migrateLegacySkinTranslationFiles(
        skinsRoot, translationsRoot, markerPath);
    // 二次调用应报告完成但没有删除项，证明标记快速路径生效。
    ok &= check(secondResult.completed && secondResult.removedFiles.empty(),
                "完成标记存在时不得重复执行删除");
    ok &= check(std::filesystem::is_regular_file(firstChinese),
                "一次性迁移不得误删后来创建的覆写文件");
    // 皮肤本身是真实目录，只有中间的 lang 指向另一个目录。
    // 原实现会沿目录链接删除共享语言文件，故断言外部哨兵和可重试标记。
    // 本场景使用独立根及标记，否则前置成功场景会直接触发快速返回。
    // 共享目录与皮肤根同属构建树，但相互独立，不触碰用户皮肤。
    const auto linkRoot = outputRoot / "linked-skins";
    // 外部根单独命名，确保候选皮肤枚举不会把共享目标当作正常皮肤。
    const auto sharedLang = outputRoot / "shared-lang";
    const auto parent     = linkRoot / "linked/resources";
    const auto linkMarker = outputRoot / ".linked-marker";
    // 选择迁移确实会删除的文件名，防止无关哨兵无法检出越界删除。
    const auto sentinel = sharedLang / "zh_cn.lua";
    ok &= check(writeFile(sentinel, "return { shared = true }"),
                "共享哨兵写入失败");
    // 先创建真实 resources 父目录，再单独把 lang 设置为目录链接。
    std::filesystem::create_directories(parent, filesystemError);
    ok &= check(!filesystemError, "链接父目录创建失败");
    // 创建时传完整的外部目录，测试不依赖相对链接的基准解析。
    std::filesystem::create_directory_symlink(
        sharedLang, parent / "lang", filesystemError);
#if defined(_WIN32)
    // 无开发者模式或提升权限的 Windows 环境不能创建符号链接。
    if ( !filesystemError )
#else
    ok &= check(!filesystemError, "目录符号链接创建失败");
#endif
    {
        // 禁止仅靠返回错误判断安全；必须同时验证真实外部文件还在。
        // 标记不存在意味着下次启动仍有机会迁移这份本地皮肤。
        const auto blocked = MMM::Config::migrateLegacySkinTranslationFiles(
            linkRoot, translationsRoot, linkMarker);
        ok &= check(!blocked.completed &&
                        std::filesystem::is_regular_file(sentinel) &&
                        !std::filesystem::exists(linkMarker),
                    "外部翻译必须保留，且迁移可重试");
        // 用户将链接改回本地文件后再次迁移；旧共享目录不能受到影响。
        // 删除的是目录链接本身，外部哨兵不能被测试清理步骤遮蔽。
        std::filesystem::remove(parent / "lang", filesystemError);
        ok &= check(!filesystemError &&
                        writeFile(parent / "lang/zh_cn.lua", "return {}"),
                    "替换为皮肤内目录失败");
        // 使用原根和标记再次调用，检查恢复后的正常删除路径。
        const auto retry = MMM::Config::migrateLegacySkinTranslationFiles(
            linkRoot, translationsRoot, linkMarker);
        // 外部哨兵经历失败和成功两个阶段都应保持原状。
        ok &= check(retry.completed &&
                        !std::filesystem::exists(parent / "lang/zh_cn.lua") &&
                        std::filesystem::is_regular_file(sentinel),
                    "重试只清除皮肤内翻译");
    }
    // 二次调用后的保留文件是一次性语义的最终行为证据。
    // 测试输出留在构建目录供失败诊断，下次运行会在开头安全重建。
    // 聚合返回码让所有后置断言执行完毕并保留完整失败日志。
    return ok ? 0 : 1;
}
