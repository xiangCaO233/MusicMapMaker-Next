/**
 * @file AssetSyncServiceTest.cpp
 * @brief 资源同步服务单元测试，覆盖清单解析、差异判断与 zip 安全解压。
 * @details 全部远端输入由本机 file URL 或内存字符串提供，不访问公网服务。
 * 每个文件系统用例创建独立临时根并在结束前递归清理。
 */

#include "network/AssetSyncService.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <miniz.h>
#include <string>
#include <utility>
#include <vector>

using namespace MMM::Network;

namespace
{

/// @brief 创建唯一临时目录。
/// @return 已尝试创建的测试根路径；失败时后续写入断言负责报告。
/// @note 名称使用单调时钟值，降低同一进程多用例之间的碰撞概率。
/// @warning 仅供测试低频调用，不可用于生产资源目录分配。
std::filesystem::path createTempRoot()
{
    // 使用 error_code 重载，避免临时目录查询失败抛出异常。
    std::error_code       tempPathError;
    std::filesystem::path root =
        std::filesystem::temp_directory_path(tempPathError);
    // 无法取得系统临时目录时退回当前测试工作目录，仍保留唯一子目录。
    if ( tempPathError ) root = ".";

    // steady_clock 只承担进程内唯一后缀，不作为持久时间戳解释。
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root /= "mmm_asset_sync_test_" + std::to_string(stamp);

    // 创建失败不在帮助器内记录，使具体测试通过首个 fixture 断言报告。
    std::error_code createError;
    std::filesystem::create_directories(root, createError);
    // 即使失败也返回预期路径，调用方的统一清理仍可安全执行。
    return root;
}

/// @brief 写入 UTF-8 文本测试文件。
/// @param path 目标文件路径。
/// @param text 原样写入的 UTF-8 或 ASCII 字节。
/// @return 父目录创建且文件完整写入时返回 true。
/// @note 不使用异常，所有创建和写入失败均转换为 false。
bool writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    // 嵌套资源 fixture 需要先创建父目录，根级文件则无需该步骤。
    const auto parentPath = path.parent_path();
    if ( !parentPath.empty() ) {
        // 非抛出重载允许测试把权限或路径错误转为普通失败结果。
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if ( createError ) return false;
    }

    // binary 保持摘要输入跨平台一致，trunc 防止旧 fixture 尾部残留。
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    // 写入后检查流状态，不能仅凭文件成功打开判定 fixture 完整。
    file << text;
    return file.good();
}

/// @brief 从文件读取 UTF-8 文本。
/// @param path 待读取的测试文件路径。
/// @return 成功时返回全部原始字节，打开失败时返回空字符串。
/// @note 仅用于确定性小文件断言，不适合作为大资源读取帮助器。
std::string readTextFile(const std::filesystem::path& path)
{
    // 二进制模式保证换行和 ZIP 解压内容不会被平台转换。
    std::ifstream file(path, std::ios::binary);
    if ( !file ) return {};
    // 流迭代器读取到 EOF，适合测试中的小型确定性 fixture。
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

/// @brief 将本地路径转换为 libcurl 可读取的 file URL。
/// @param path 本机 fixture 文件路径。
/// @return 使用通用斜杠形式的 file URL。
std::string fileUrlFor(const std::filesystem::path& path)
{
    // pathToUtf8Generic 统一路径分隔符，使 curl 在各平台解析一致。
    return "file://" + MMM::Config::pathToUtf8Generic(path);
}

/// @brief 创建 zip 文件。
/// @param zipPath 目标归档路径。
/// @param entries 归档内相对名称与文本内容列表。
/// @return 全部条目写入且归档 finalize 成功时返回 true。
/// @note 条目名称不在此帮助器校验，以便构造路径逃逸测试输入。
bool writeZipFile(
    const std::filesystem::path&                            zipPath,
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    // 零初始化满足 miniz 对归档状态结构的前置要求。
    mz_zip_archive zipArchive{};
    // 创建失败时 miniz 未建立可结束的 writer，立即返回。
    if ( !mz_zip_writer_init_file(
             &zipArchive, MMM::Config::pathToUtf8(zipPath).c_str(), 0) ) {
        return false;
    }

    // success 跨条目保留首个失败，失败后不继续写入后续内容。
    bool success = true;
    for ( const auto& [name, content] : entries ) {
        // 内存条目按给定名称写入，MZ_BEST_SPEED 减少测试压缩耗时。
        if ( !mz_zip_writer_add_mem(&zipArchive,
                                    name.c_str(),
                                    content.data(),
                                    content.size(),
                                    MZ_BEST_SPEED) ) {
            // 中止条目循环，但仍必须调用 writer_end 释放内部资源。
            success = false;
            break;
        }
    }

    // 只有所有条目成功时才 finalize，避免把部分归档误判为合法 fixture。
    if ( success && !mz_zip_writer_finalize_archive(&zipArchive) ) {
        success = false;
    }
    // writer_end 在成功与失败路径统一执行，不以其返回值覆盖主结果。
    mz_zip_writer_end(&zipArchive);
    return success;
}

/// @brief 检查条件并记录失败。
/// @param condition 当前断言结果。
/// @param label 失败日志使用的稳定用例描述。
/// @return 通过返回 0，失败返回 1，便于各测试累加。
int expectTrue(bool condition, const char* label)
{
    if ( condition ) {
        // 成功项也记录，便于在单个大场景中确认已推进到哪个阶段。
        XINFO("[asset-sync] PASS: {}", label);
        return 0;
    }

    // 统一前缀让 CTest 日志可以筛选所有资源同步断言失败。
    XERROR("[asset-sync] FAIL: {}", label);
    return 1;
}

/// @brief 测试 manifest 解析。
/// @return 清单字段或安全校验不符合预期的断言数量。
/// @note 同时覆盖摘要大小写归一化和相对路径逃逸拒绝。
int testParseManifest()
{
    // 所有断言继续执行，使字段解析偏差可在一次运行中完整呈现。
    int fail = 0;

    // 合法清单同时包含完整包和单文件增量信息。
    const std::string validManifest =
        R"json({
          "version": "v0.4.0-assets.1",
          "package": {
            "url": "/download/assets.zip",
            "sha256": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
          },
          "files": [
            {
              "path": "skins/mmm-default/skin.lua",
              "url": "/download/assets/files/skins/mmm-default/skin.lua",
              "sha256": "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
              "size": 12
            }
          ]
        })json";

    // 错误文本与 optional 同时观察，成功路径应提供结构化结果。
    std::string errorMessage;
    const auto  manifest =
        AssetSyncService::parseManifest(validManifest, errorMessage);
    fail += expectTrue(manifest.has_value(), "valid manifest parses");
    if ( manifest ) {
        // 版本文本由服务原样保留，供本地版本标记比较。
        fail += expectTrue(manifest->version == "v0.4.0-assets.1",
                           "manifest version parsed");
        fail += expectTrue(manifest->packageSha256 ==
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                               "aaaaaaaaaaaaaaaaa",
                           "package checksum normalized");
        // 带 sha256: 前缀的包摘要应去前缀并保持固定 64 位小写。
        // 单项列表证明嵌套 files 数组被完整读取。
        fail += expectTrue(manifest->files.size() == 1,
                           "manifest file count parsed");
        fail += expectTrue(manifest->files.front().sha256 ==
                               "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                               "bbbbbbbbbbbbbbbbb",
                           "file checksum normalized");
        // 大写文件摘要归一化后可直接与本地 sha256File 输出比较。
    }

    // 非法清单使用 ../ 尝试逃逸 assets 根目录，其他字段保持合法。
    const std::string unsafeManifest =
        R"json({
          "version": "bad",
          "files": [
            {
              "path": "../evil.txt",
              "url": "/evil.txt",
              "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
              "size": 1
            }
          ]
        })json";

    // 清除上一轮消息，确保错误断言来自当前非法路径用例。
    errorMessage.clear();
    const auto unsafe =
        AssetSyncService::parseManifest(unsafeManifest, errorMessage);
    fail += expectTrue(!unsafe.has_value(), "unsafe manifest path rejected");
    // 拒绝必须携带可展示诊断，不能只返回空 optional。
    fail += expectTrue(!errorMessage.empty(),
                       "unsafe manifest reports error message");

    // 两份清单均验证完成，返回累计失败数而不把错误文本跨用例传播。
    return fail;
}

/// @brief 测试下载 URL 补全。
/// @return URL 基址拼接或绝对地址保留失败的断言数量。
/// @note 覆盖基址和资源路径两侧斜杠的规范化边界。
int testResolveDownloadUrl()
{
    // 三种来源分别累加，任一失败都保留实际帮助器行为日志标签。
    int fail = 0;

    // 资源以根路径开头时，基址尾部无需斜杠也只能产生一个分隔符。
    fail +=
        expectTrue(AssetSyncService::resolveDownloadUrl(
                       "https://mmm.xiang233.top", "/download/assets.zip") ==
                       "https://mmm.xiang233.top/download/assets.zip",
                   "absolute path URL resolved against base");
    // 相对资源路径配合带斜杠基址同样不能形成双斜杠路径。
    fail +=
        expectTrue(AssetSyncService::resolveDownloadUrl(
                       "https://mmm.xiang233.top/", "download/assets.zip") ==
                       "https://mmm.xiang233.top/download/assets.zip",
                   "relative path URL resolved against base");
    // 已含 scheme 的 CDN URL 不得被错误前置应用基址。
    fail += expectTrue(
        AssetSyncService::resolveDownloadUrl(
            "https://mmm.xiang233.top", "https://cdn.example.com/assets.zip") ==
            "https://cdn.example.com/assets.zip",
        "absolute URL preserved");

    // 三种斜杠与 scheme 组合都符合下载器预期后返回零。
    return fail;
}

/// @brief 测试本地差异收集。
/// @return 文件写入、摘要比较或清单顺序不符合预期的断言数量。
/// @note 使用相同、内容变化和缺失三类条目覆盖筛选边界。
int testCollectOutdatedFiles()
{
    // fixture 和结果检查共享失败计数，清理无论结果如何都会执行。
    int fail = 0;

    // 每个用例独占根目录，assetsRoot 模拟正式资源安装位置。
    const auto root       = createTempRoot();
    const auto assetsRoot = root / "assets";
    const auto samePath   = assetsRoot / "same.txt";
    const auto oldPath    = assetsRoot / "old.txt";
    const auto newPath    = root / "new.txt";

    // same 与 old 放在资源根内，new 只提供目标摘要内容。
    fail += expectTrue(writeTextFile(samePath, "same"), "write same fixture");
    fail += expectTrue(writeTextFile(oldPath, "old"), "write old fixture");
    fail += expectTrue(writeTextFile(newPath, "new"), "write new fixture");

    // 清单顺序固定为相同、变化、缺失，结果应保留后两项相对顺序。
    AssetManifest manifest;
    manifest.version = "v-test";
    // same.txt 的清单摘要直接来自当前本地文件，应被判定为最新。
    manifest.files.push_back(AssetFileEntry{
        "same.txt",
        "/same.txt",
        AssetSyncService::sha256File(samePath),
        4,
    });
    // old.txt 的本地内容为 old，但期望摘要取自 new fixture。
    manifest.files.push_back(AssetFileEntry{
        "old.txt",
        "/old.txt",
        AssetSyncService::sha256File(newPath),
        3,
    });
    // missing.txt 不创建本地文件，目标摘要仍保持合法。
    manifest.files.push_back(AssetFileEntry{
        "missing.txt",
        "/missing.txt",
        AssetSyncService::sha256File(newPath),
        3,
    });

    const auto outdated =
        AssetSyncService::collectOutdatedFiles(manifest, assetsRoot);
    // 内容相同项被过滤，仅变化和缺失两项进入增量下载集合。
    fail += expectTrue(outdated.size() == 2,
                       "collects changed and missing files only");
    fail +=
        expectTrue(outdated[0].path == "old.txt", "changed file listed first");
    fail += expectTrue(outdated[1].path == "missing.txt",
                       "missing file listed second");

    // 非抛出递归清理不覆盖主要断言结果，避免失败时遗留 fixture。
    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    // 清理错误不覆盖差异算法断言，CI 临时目录由运行环境兜底回收。
    return fail;
}

/// @brief 测试 zip 解压路径安全。
/// @return 归档创建、内容解压或路径逃逸防护失败的断言数量。
/// @note 安全归档与恶意归档共用输出根，验证拒绝不会写到根外。
int testExtractZipArchive()
{
    // 先验证正常解压，再用单独归档覆盖拒绝路径。
    int fail = 0;

    // 归档与输出目录都位于同一临时根，便于结束时统一清理。
    const auto root    = createTempRoot();
    const auto zipPath = root / "assets.zip";
    const auto outRoot = root / "out";

    // 安全 fixture 包含嵌套皮肤文件和根级说明文件。
    fail += expectTrue(
        writeZipFile(zipPath,
                     {
                         { "assets/skins/mmm-default/skin.lua", "skin" },
                         { "assets/readme.txt", "hello" },
                     }),
        "write safe zip fixture");

    // errorMessage 在成功路径应不影响布尔结果，失败路径需提供详情。
    std::string errorMessage;
    fail += expectTrue(
        AssetSyncService::extractZipArchive(zipPath, outRoot, errorMessage),
        "safe zip extracts");
    // 逐文件比较内容证明不仅创建路径，也正确写出条目字节。
    fail += expectTrue(
        readTextFile(outRoot / "assets/skins/mmm-default/skin.lua") == "skin",
        "skin file extracted");
    fail += expectTrue(readTextFile(outRoot / "assets/readme.txt") == "hello",
                       "readme file extracted");

    // 恶意条目使用父目录分量尝试写到 outRoot 之外。
    const auto badZipPath = root / "bad.zip";
    fail += expectTrue(writeZipFile(badZipPath, { { "../evil.txt", "evil" } }),
                       "write unsafe zip fixture");

    // 清空前一次状态后，要求解压器拒绝整个不安全归档。
    errorMessage.clear();
    fail += expectTrue(
        !AssetSyncService::extractZipArchive(badZipPath, outRoot, errorMessage),
        "unsafe zip path rejected");
    // 安全拒绝也必须提供错误文本，便于启动界面解释资源包问题。
    fail += expectTrue(!errorMessage.empty(), "unsafe zip reports error");

    // root 同时包含归档、输出和潜在中间文件，一次递归清理即可。
    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    // 无论不安全条目是否被拒绝，测试都尝试删除完整临时根。
    return fail;
}

/// @brief 测试已有本地资源时远程 manifest 不可用也允许启动。
/// @return 降级状态或警告信息不符合预期的断言数量。
/// @note 该路径保护离线启动，前提是关键本地皮肤资源已经存在。
/// @warning 同步调用执行文件 I/O，仅在独立测试进程运行。
int testSyncKeepsExistingAssetsWhenManifestUnavailable()
{
    // fixture 写入与同步结果共用失败计数，确保清理路径始终到达。
    int fail = 0;

    // 创建最小可用默认皮肤文件，使服务具备离线降级条件。
    const auto root       = createTempRoot();
    const auto assetsRoot = root / "assets";
    fail += expectTrue(
        writeTextFile(assetsRoot / "skins/mmm-default/skin.lua", "skin"),
        "write existing local asset");

    // manifest 指向不存在的本地 file URL，避免网络超时影响测试。
    AssetSyncOptions options;
    // assetsRootPath 指向已存在资源，构成允许离线 Ready 的依据。
    options.assetsRootPath = assetsRoot;
    // baseUrl 无效但不会被访问，因为 manifest 使用显式 file URL。
    options.baseUrl = "https://invalid.local";
    options.manifestUrl =
        "file://" + MMM::Config::pathToUtf8(root / "missing-manifest.json");
    // 禁用完整包回退，确保结果只来自已有资源容错逻辑。
    options.packageUrl = {};

    // sync 同步执行测试场景，返回状态应允许应用继续启动。
    const auto result = AssetSyncService::sync(options);
    fail += expectTrue(result.status == AssetSyncStatus::kReady,
                       "existing assets survive missing manifest");
    // Ready 仍保留警告文本，让 UI 能提示远端检查失败而非静默忽略。
    fail += expectTrue(!result.errorMessage.empty(),
                       "missing manifest warning is retained");

    // 结束时删除本地皮肤和缺失清单所在的整个临时根。
    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    // 清理失败不改变已验证的离线启动语义。
    return fail;
}

/// @brief 测试本地版本一致时跳过精确文件校验。
/// @return 快速路径状态、进度或本地文件保持性失败的断言数量。
/// @note 匹配版本是可信缓存提示，默认路径不重新哈希每个资源文件。
/// @warning 通过缺失下载源验证快速路径，不能改为真实网络 URL。
int testSyncSkipsPreciseCheckWhenVersionMatches()
{
    // 场景故意让本地文件内容陈旧，以证明快速路径确实跳过校验。
    int fail = 0;

    const auto root       = createTempRoot();
    const auto assetsRoot = root / "assets";
    const auto wantedPath = root / "wanted.txt";
    // assetsRoot 下的版本标记与文件内容故意表达不一致状态。
    // wanted 只用于生成清单期望摘要，不作为可访问下载源。
    fail += expectTrue(writeTextFile(wantedPath, "wanted"),
                       "write wanted checksum fixture");
    fail += expectTrue(writeTextFile(assetsRoot / "theme.txt", "stale"),
                       "write stale local asset");
    fail += expectTrue(
        writeTextFile(assetsRoot / ".mmm-assets-version", "v-skip\n"),
        "write matching local asset version");

    // 清单版本与本地标记一致，但文件摘要刻意指向 wanted 内容。
    const auto manifestPath = root / "manifest.json";
    const std::string manifestText = std::string(R"json({
          "version": "v-skip",
          "files": [
            {
              "path": "theme.txt",
              "url": ")json") + fileUrlFor(root / "missing-download.txt") +
                                     R"json(",
              "sha256": ")json" + AssetSyncService::sha256File(wantedPath) +
                                     R"json(",
              "size": 6
            }
          ]
        })json";
    // 缺失下载 URL 是哨兵：若错误进入精确路径，同步将直接失败。
    fail += expectTrue(writeTextFile(manifestPath, manifestText),
                       "write version skip manifest");

    bool sawFileCheck = false;

    // progressCallback 只记录是否出现文件校验阶段，不影响服务行为。
    AssetSyncOptions options;
    // 所有路径均在独立测试根内，sync 不会触碰应用真实资源目录。
    options.assetsRootPath = assetsRoot;
    options.baseUrl        = "https://invalid.local";
    options.manifestUrl    = fileUrlFor(manifestPath);
    // 空 packageUrl 禁止精确路径意外退回完整包下载。
    options.packageUrl = {};
    options.progressCallback =
        [&sawFileCheck](const AssetSyncProgress& progress) {
            // 任意 CheckingFiles 回调都说明快速版本路径未被采用。
            if ( progress.stage == AssetSyncProgressStage::kCheckingFiles ) {
                sawFileCheck = true;
            }
        };

    // 匹配版本应直接 Ready，既不哈希也不访问缺失下载 URL。
    const auto result = AssetSyncService::sync(options);
    fail += expectTrue(result.status == AssetSyncStatus::kReady,
                       "matching version reports ready");
    fail += expectTrue(result.errorMessage.empty(),
                       "matching version does not hit missing download");
    // 空错误文本证明缺失 URL 确实没有被访问。
    fail += expectTrue(result.checkedFileCount == 0,
                       "matching version skips file hash checks");
    // 计数和进度回调两侧同时验证，避免仅漏报回调的假通过。
    fail += expectTrue(!sawFileCheck,
                       "matching version emits no file check progress");
    fail += expectTrue(readTextFile(assetsRoot / "theme.txt") == "stale",
                       "matching version leaves local files untouched");

    // 清理包含清单、摘要源和模拟资源安装目录的整个场景。
    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    // 保留陈旧内容的断言在清理前完成，避免读取已删除 fixture。
    return fail;
}

/// @brief 测试强制精确校验可修复版本一致但本地文件缺失的资源。
/// @return 强制校验、下载计数或修复内容失败的断言数量。
/// @note forcePreciseVerification 必须覆盖相同版本标记的快速路径。
/// @warning 同步期间写入临时资源根，调用前必须确保路径隔离。
int testSyncForcePreciseCheckRepairsMissingFile()
{
    // 该场景不创建目标 theme.txt，使清单条目明确处于缺失状态。
    int fail = 0;

    const auto root       = createTempRoot();
    const auto assetsRoot = root / "assets";
    const auto wantedPath = root / "wanted.txt";
    // wanted 文件同时充当期望摘要来源和 file URL 下载源。
    fail += expectTrue(writeTextFile(wantedPath, "wanted"),
                       "write forced verification fixture");
    fail += expectTrue(
        writeTextFile(assetsRoot / ".mmm-assets-version", "v-force\n"),
        "write matching version for forced verification");
    // 只创建版本标记而不创建 theme.txt，隔离“文件缺失”修复分支。

    // 清单版本刻意匹配本地标记，唯一触发因素是强制精确开关。
    const auto manifestPath = root / "manifest.json";
    const std::string manifestText = std::string(R"json({
          "version": "v-force",
          "files": [
            {
              "path": "theme.txt",
              "url": ")json") + fileUrlFor(wantedPath) +
                                     R"json(",
              "sha256": ")json" + AssetSyncService::sha256File(wantedPath) +
                                     R"json(",
              "size": 6
            }
          ]
        })json";
    // 写入本地清单后，整个测试仍不需要 HTTP 服务器。
    fail += expectTrue(writeTextFile(manifestPath, manifestText),
                       "write forced verification manifest");

    AssetSyncOptions options;
    // baseUrl 作为未使用兜底；清单与文件均提供完整 file URL。
    options.assetsRootPath = assetsRoot;
    options.baseUrl        = "https://invalid.local";
    options.manifestUrl    = fileUrlFor(manifestPath);
    // 不提供整包地址，强制服务使用单文件下载路径。
    options.packageUrl = {};
    // 显式开启后必须逐项哈希并下载缺失文件。
    options.forcePreciseVerification = true;

    // 缺失条目修复后状态应为 Updated，而不是仅 Ready。
    const auto result = AssetSyncService::sync(options);
    fail += expectTrue(result.status == AssetSyncStatus::kUpdated,
                       "forced verification repairs missing asset");
    // Updated 表示资源树发生实际写入，不只是版本标记刷新。
    fail += expectTrue(result.checkedFileCount == 1,
                       "forced verification hashes manifest entries");
    // 单项缺失清单应精确产生一次更新计数。
    fail += expectTrue(result.updatedFileCount == 1,
                       "forced verification downloads missing asset");
    fail += expectTrue(readTextFile(assetsRoot / "theme.txt") == "wanted",
                       "forced verification writes repaired asset");
    // 内容相等从最终产物侧验证 file URL 下载和原子替换均完成。

    // 清理下载源、清单、本地版本标记和修复后的资源。
    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    // 所有结果读取完成后再清理，避免生命周期影响同步判断。
    return fail;
}

/// @brief 测试版本不一致时逐文件校验会汇报进度。
/// @return 精确校验状态、计数、进度或版本落盘失败的断言数量。
/// @note 本地文件内容已匹配，流程只应检查并更新版本标记，不下载。
/// @warning 回调在同步调用线程执行，测试引用只在该函数栈内有效。
int testSyncReportsPreciseCheckProgress()
{
    // 该场景同时观察同步结果和回调最后一次文件索引。
    int fail = 0;

    // same.txt 直接位于模拟资源根，并作为自身清单下载 URL。
    const auto root       = createTempRoot();
    const auto assetsRoot = root / "assets";
    const auto assetPath  = assetsRoot / "same.txt";
    // 固定四字节内容让清单 size 和摘要都易于核对。
    fail += expectTrue(writeTextFile(assetPath, "same"),
                       "write matching local asset");

    // 不创建本地版本标记，迫使服务进入逐文件精确校验阶段。
    const auto manifestPath = root / "manifest.json";
    const std::string manifestText = std::string(R"json({
          "version": "v-progress",
          "files": [
            {
              "path": "same.txt",
              "url": ")json") + fileUrlFor(assetPath) +
                                     R"json(",
              "sha256": ")json" + AssetSyncService::sha256File(assetPath) +
                                     R"json(",
              "size": 4
            }
          ]
        })json";
    // 清单下载同样使用 file URL，测试不依赖网络服务。
    fail += expectTrue(writeTextFile(manifestPath, manifestText),
                       "write progress manifest");

    // 回调状态只由同步调用线程写入，无需原子或互斥保护。
    bool sawFileCheck = false;
    // 保存最后观察到的单基索引和总文件数，验证 UI 进度语义。
    std::size_t lastFileIndex      = 0;
    std::size_t lastTotalFileCount = 0;

    AssetSyncOptions options;
    // 模拟安装根只包含待核对的 same.txt，不预置版本标记。
    options.assetsRootPath = assetsRoot;
    // 无效基址不会被使用，能暴露清单 URL 解析错误导致的意外联网路径。
    options.baseUrl = "https://invalid.local";
    // 本地清单提供确定性输入，不受远端发布状态影响。
    options.manifestUrl = fileUrlFor(manifestPath);
    // 精确校验无差异时无需完整包，因此显式清空兜底地址。
    options.packageUrl = {};
    // 只消费 CheckingFiles 阶段，其他下载或完成事件不覆盖观测值。
    options.progressCallback =
        [&sawFileCheck, &lastFileIndex, &lastTotalFileCount](
            const AssetSyncProgress& progress) {
            // 非目标阶段立即返回，保持最后一次文件检查快照。
            if ( progress.stage != AssetSyncProgressStage::kCheckingFiles ) {
                return;
            }
            // 单项清单应最终报告 current=1、total=1。
            sawFileCheck       = true;
            lastFileIndex      = progress.currentFileIndex;
            lastTotalFileCount = progress.totalFileCount;
        };

    // 文件摘要已匹配，因此状态为 Ready 而非 Updated。
    const auto result = AssetSyncService::sync(options);
    fail += expectTrue(result.status == AssetSyncStatus::kReady,
                       "matching files report ready after precise check");
    // Ready 与零更新计数共同表达资源内容无需改写。
    fail +=
        expectTrue(result.errorMessage.empty(), "precise check has no error");
    // 本地文件和清单均可读，整个路径不应产生降级警告。
    fail += expectTrue(result.checkedFileCount == 1,
                       "precise check counts manifest files");
    // 没有内容差异时 updatedFileCount 必须保持零。
    fail += expectTrue(result.updatedFileCount == 0,
                       "precise check downloads no matching files");
    fail += expectTrue(sawFileCheck, "precise check emits progress");
    // 回调存在性与最终索引分开断言，便于区分未通知和计数错误。
    // 回调计数采用单基当前索引，避免 UI 永远显示零进度。
    fail += expectTrue(lastFileIndex == 1 && lastTotalFileCount == 1,
                       "precise check progress includes file count");
    // 即使没有下载，完成精确校验后也要写入新的可信版本标记。
    fail += expectTrue(
        readTextFile(assetsRoot / ".mmm-assets-version") == "v-progress\n",
        "precise check writes new local version");

    // 清理资源、清单及同步写入的版本标记。
    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    // 版本标记内容已在删除前读取，清理不影响结果判断。
    return fail;
}

/// @brief 测试同步开始前的取消请求会立即终止启动期资源流程。
/// @return 服务在任何 I/O 前返回 Cancelled 时为零，否则为一。
/// @note options 只设置取消回调，确保默认路径也不会被访问。
int testSyncHonorsCancellationRequest()
{
    // 永真回调模拟调用方在 sync 入口前已经请求取消。
    AssetSyncOptions options;
    // 回调不捕获状态，保证每次检查都得到相同取消结果。
    options.cancellationCallback = []() { return true; };

    // 结果必须明确区分取消与错误，供启动流程选择静默终止。
    const auto result = AssetSyncService::sync(options);
    // 未配置路径仍可取消，证明服务在解析默认选项前检查请求。
    return expectTrue(result.status == AssetSyncStatus::kCancelled,
                      "asset sync honors cancellation request");
}

}  // namespace

/// @brief 依次运行全部本地资源同步场景并汇总失败数。
/// @return 所有断言通过时返回 0，否则返回 1。
int main()
{
    // 测试不初始化个人配置或应用资源路径，所有写入都在临时目录。
    // 每个测试返回断言失败数，累加但不因前一场景失败而短路。
    int fail = 0;
    // 先覆盖纯解析帮助器，再进入需要临时文件系统的场景。
    fail += testParseManifest();
    fail += testResolveDownloadUrl();
    fail += testCollectOutdatedFiles();
    // ZIP 安全测试位于同步策略前，失败也不阻止后续临时场景清理自身。
    fail += testExtractZipArchive();
    // 同步策略按离线降级、版本快速路径、强制修复和进度顺序执行。
    fail += testSyncKeepsExistingAssetsWhenManifestUnavailable();
    fail += testSyncSkipsPreciseCheckWhenVersionMatches();
    fail += testSyncForcePreciseCheckRepairsMissingFile();
    fail += testSyncReportsPreciseCheckProgress();
    // 取消用例最后执行，证明默认 options 无需 fixture 即可安全退出。
    fail += testSyncHonorsCancellationRequest();

    if ( fail == 0 ) {
        // 零失败时记录套件级成功，便于从详细 PASS 日志中快速收尾。
        XINFO("AssetSyncServiceTest passed.");
    } else {
        // 非零时输出所有场景累计失败数，CTest 仍只接收统一退出码。
        XERROR("AssetSyncServiceTest failed: {}", fail);
    }
    // 所有局部测试根均已离开作用域并执行显式 remove_all。
    // 不把失败数直接作为退出码，避免平台对大值截断产生歧义。
    return fail == 0 ? 0 : 1;
}
