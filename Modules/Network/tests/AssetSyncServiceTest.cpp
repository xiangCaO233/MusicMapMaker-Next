/**
 * @file AssetSyncServiceTest.cpp
 * @brief 资源同步服务单元测试，覆盖清单解析、差异判断与 zip 安全解压。
 * @details 全部远端输入由本机 file URL 或内存字符串提供，不访问公网服务。
 * 每个文件系统用例创建独立临时根并在结束前递归清理。
 */

#include "network/AssetSyncService.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <miniz.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace MMM::Network;

namespace
{

/// @brief 临时覆盖进程环境变量，并在测试结束时恢复原值。
/// @details 环境变量属于进程全局状态；本测试单线程执行，因此在作用域内设置后
/// 可直接验证默认配置读取结果。析构时恢复原值或还原为未设置状态。
class ScopedEnvironmentVariable final
{
public:
    /// @brief 保存旧值，后续通过 set 设置临时测试值。
    /// @param name 环境变量名称。
    explicit ScopedEnvironmentVariable(const char* name) : m_name(name)
    {
        // 复制环境存储中的旧值，后续 setenv 可能替换该存储。
        if ( const char* previous = std::getenv(name) ) {
            m_previous = previous;
        }
    }

    /// @brief 析构时恢复环境变量原值。
    /// @details set 使用 C 环境 API，不抛出异常；清理错误不覆盖测试断言结果。
    ~ScopedEnvironmentVariable() { set(m_previous); }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) =
        delete;

    /// @brief 设置或清除变量，并报告系统调用结果。
    /// @param value 新值；空 optional 表示清除变量。
    /// @return 环境变量操作成功时返回 true。
    bool set(const std::optional<std::string>& value)
    {
#if defined(_WIN32)
        return _putenv_s(m_name.c_str(), value ? value->c_str() : "") == 0;
#else
        if ( value ) return setenv(m_name.c_str(), value->c_str(), 1) == 0;
        return unsetenv(m_name.c_str()) == 0;
#endif
    }

private:
    /// @brief 变量名称在恢复期间保持有效。
    std::string m_name;
    /// @brief 构造前的值；空值表示原本未设置。
    std::optional<std::string> m_previous;
};

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

/// @brief 测试默认资源地址及环境变量覆盖优先级。
/// @return 默认 CDN 地址或显式环境变量覆盖不符合预期时返回失败数。
/// @note 每个变量在测试结束时恢复，避免污染同一进程中的其它用例。
/// @details
/// - 清除继承环境，确认出厂配置使用已选定的 CDN 域名。
/// - 单独覆盖 base URL，检查默认 manifest 与 package 随之迁移。
/// - 再分别覆盖端点 URL，确认完整地址拥有最高优先级。
/// - 所有 URL 都是纯配置断言，不会发起公网请求。
int testDefaultOptionsUsesCdnAndHonorsEnvironment()
{
    int                       fail = 0;
    ScopedEnvironmentVariable baseUrl("MMM_ASSET_BASE_URL");
    ScopedEnvironmentVariable manifestUrl("MMM_ASSET_MANIFEST_URL");
    ScopedEnvironmentVariable packageUrl("MMM_ASSET_PACKAGE_URL");

    const bool cleared = baseUrl.set(std::nullopt) &&
                         manifestUrl.set(std::nullopt) &&
                         packageUrl.set(std::nullopt);
    // 默认值测试必须先隔离调用进程可能携带的镜像配置。
    fail += expectTrue(cleared, "clear asset URL overrides for defaults");
    // 清理失败时不读取不确定配置，后续仍执行恢复作用域。
    if ( cleared ) {
        // 三个默认地址都应由同一 CDN 根生成，避免清单与完整包分流。
        const auto defaults = AssetSyncService::defaultOptions();
        // baseUrl 自身也属于公开选项，避免只验证拼出的两个端点。
        fail += expectTrue(defaults.baseUrl == "https://downloads.xiang233.top",
                           "default asset base URL uses Cloudflare CDN");
        // manifest 位于专用子路径，完整值可发现拼接路径改变。
        fail += expectTrue(
            defaults.manifestUrl ==
                "https://downloads.xiang233.top/download/assets/manifest.json",
            "default manifest URL uses Cloudflare CDN");
        // 首装 zip 是独立回退端点，也必须落在相同 CDN 主机。
        fail +=
            expectTrue(defaults.packageUrl ==
                           "https://downloads.xiang233.top/download/assets.zip",
                       "default package URL uses Cloudflare CDN");
    }

    // 只覆盖 baseUrl 时，未单独配置的两个地址应随之改指镜像根。
    const bool setBase =
        baseUrl.set(std::string("https://mirror.example.test"));
    // 独立清除两项地址，才能观察 baseUrl 的派生行为。
    if ( setBase && manifestUrl.set(std::nullopt) &&
         packageUrl.set(std::nullopt) ) {
        const auto baseOverridden = AssetSyncService::defaultOptions();
        // 根相对 manifest 地址应保留服务端定义的目录层级。
        fail += expectTrue(
            baseOverridden.manifestUrl ==
                "https://mirror.example.test/download/assets/manifest.json",
            "base URL override redirects default manifest");
        // 完整包也必须跟随 base 覆盖，防止冷启动绕过镜像。
        fail +=
            expectTrue(baseOverridden.packageUrl ==
                           "https://mirror.example.test/download/assets.zip",
                       "base URL override redirects default package");
    } else {
        fail += expectTrue(false, "prepare base-only URL override");
    }

    // 分别给端点配置绝对地址，验证它们优先于镜像根派生值。
    const bool setManifest = manifestUrl.set(
        std::string("https://manifest.example.test/assets.json"));
    const bool setPackage =
        packageUrl.set(std::string("https://package.example.test/assets.zip"));
    // 三项设置全部成功后再观察快照，避免部分环境状态造成误判。
    fail += expectTrue(setBase && setManifest && setPackage,
                       "set explicit asset URL overrides");
    if ( setBase && setManifest && setPackage ) {
        // 完整 URL 覆盖需逐项保持，不能再被 baseUrl 拼接或重写。
        const auto overridden = AssetSyncService::defaultOptions();
        fail += expectTrue(overridden.baseUrl == "https://mirror.example.test",
                           "base URL environment override is preserved");
        // 清单可单独留在控制域名，文件 URL 由清单另行指向 CDN。
        fail += expectTrue(overridden.manifestUrl ==
                               "https://manifest.example.test/assets.json",
                           "manifest URL environment override is preserved");
        // 独立 package 覆盖用于清单不可用时的首次安装回退。
        fail += expectTrue(
            overridden.packageUrl == "https://package.example.test/assets.zip",
            "package URL environment override is preserved");
    }
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

/// @brief 用单条多块资源验证块内取消、旧文件保留和重试。
/// @details 取消依据私有暂存文件开始增长，避免依赖回调次数或操作系统调度。
/// 首次解压被中断时旧资源仍可读，完整重试才允许替换目标。
/// @par 场景约束
/// - 使用实际公共解压入口，包含目录预检、回调、CRC 和重命名。
/// - 测试写入一次包，取消和重试读取同一份 ZIP。
/// - 旧资源预先写在最终路径，取消后仍需按原身份读取。
/// - 分块是否发生以磁盘暂存的有效前缀为准，与压缩率无关。
/// - 重试不传取消回调，目标内容由完整展开结果替换。
/// @return 暂存、取消和完整重试断言中的失败数。
int testZipCancellationInsideEntry()
{
    int               fail   = 0;
    const auto        root   = createTempRoot();
    const auto        zip    = root / "assets.zip";
    const auto        out    = root / "out";
    const auto        target = out / "assets/large.txt";
    const std::string contents(4U * 1024U * 1024U, 'z');
    // 压缩后体积很小，解压后仍需经过多次输出回调。
    fail += expectTrue(writeZipFile(zip, { { "assets/large.txt", contents } }),
                       "write multiblock asset archive");
    fail += expectTrue(writeTextFile(target, "previous"),
                       "write existing resource");
    // 独立记录实际观测到的前缀，防止未来仅凭返回状态判定块内取消通过。
    bool        sawPartialEntry = false;
    std::string error;
    /// @brief 选择已经写出非空前缀、尚未写完整条目的取消时点。
    const auto cancelDuringWrite = [&]() {
        std::error_code ec;
        // 观察被测目录里已经写出的暂存文件，因而不会在预检时提前取消。
        std::filesystem::directory_iterator entries(target.parent_path(), ec);
        // 早期调用只检查意图，缺少可观测的暂存文件时仍应让解压继续。
        if ( ec ) return false;
        for ( const auto& entry : entries ) {
            if ( !entry.path().filename().string().starts_with(
                     ".mmm-zip-entry-") )
                continue;
            // stream 尚未打开时文件可能不存在；在下一块之前再次检查。
            // 条目完全写完后观察到的尺寸不能证明中途曾响应取消。
            const auto written =
                std::filesystem::file_size(entry.path() / "entry", ec);
            sawPartialEntry = !ec && written > 0 && written < contents.size();
            return sawPartialEntry;
        }
        return false;
    };
    fail += expectTrue(!AssetSyncService::extractZipArchive(
                           zip, out, error, cancelDuringWrite) &&
                           sawPartialEntry && error == "Asset sync cancelled",
                       "cancels during a single file and reports cancellation");
    // 当前调用只拥有临时条目，失败后不得截断已有文件或留下临时目录。
    fail += expectTrue(readTextFile(target) == "previous",
                       "cancelled write preserves existing resource");
    std::error_code                     ec;
    std::filesystem::directory_iterator entriesAfterCancel(target.parent_path(),
                                                           ec);
    // 父目录应仍存在，旧文件是其中唯一保留的目录项。
    // 检查整个父目录可发现关闭失败或提前返回留下的临时条目。
    // 在重试前执行此检查，避免下一次解压掩盖上一次泄漏。
    if ( !ec ) {
        for ( const auto& entry : entriesAfterCancel ) {
            fail += expectTrue(entry.path() == target,
                               "cancelled entry staging is removed");
        }
    } else {
        ++fail;
    }
    // 重试沿用相同 ZIP 和目标，成功时既验证替换也验证每块内容连续。
    fail += expectTrue(AssetSyncService::extractZipArchive(zip, out, error),
                       "retry publishes complete entry");
    fail += expectTrue(readTextFile(target) == contents,
                       "streamed entry contains the complete original bytes");
    // 最终统一回收 ZIP 与输出，取消分支的暂存清理已在重试前独立断言。
    std::filesystem::remove_all(root, ec);
    return fail;
}

/// @brief 用合法空文件验证归档条目预算先于任何输出。
/// @details 每个空文件具有唯一名称，避免归档工具将同名记录折叠。
/// ZIP 生成在独立根内，输出目录初始不存在，可独立观察拒绝前有无创建。
/// 条目数仅高出一项，覆盖严格大于上限这一侧。
/// 有效的中央目录必须先被打开，因而同时断言业务错误文本排除库级打开错误。
/// 测试不计入源树资源，也不需要制作大文件或等待外部下载。
/// @return 条目数量拒绝及其副作用断言的失败数。
int testZipEntryCountBudget()
{
    int                                              fail = 0;
    const auto                                       root = createTempRoot();
    const auto                                       zip  = root / "assets.zip";
    const auto                                       out  = root / "out";
    std::vector<std::pair<std::string, std::string>> entries;
    // 零字节文件不消耗展开字节预算，只能通过数量限制提前拒绝。
    for ( std::size_t index = 0; index <= 10'000; ++index )
        entries.emplace_back("assets/item-" + std::to_string(index), "");
    // 只有 miniz 已成功构造归档时，拒绝结果才能归因于业务条目预算。
    fail += expectTrue(writeZipFile(zip, entries),
                       "write archive over entry-count limit");
    std::string error;
    fail += expectTrue(!AssetSyncService::extractZipArchive(zip, out, error) &&
                           error == "Asset package contains too many entries",
                       "rejects entry-count limit before creating output");
    std::error_code ec;
    // 文件系统查询本身也须成功，不能将访问错误当作没有输出。
    fail += expectTrue(!std::filesystem::exists(out, ec) && !ec,
                       "entry-count refusal leaves output untouched");
    std::filesystem::remove_all(root, ec);
    return fail;
}

/// @brief 测试已有本地资源时远程 manifest 不可用也允许启动。
/// @return 降级状态或警告信息不符合预期的断言数量。
/// @note 该路径保护离线启动，前提是关键本地皮肤资源已经存在。
/// @details 同时覆盖 Android 旧包只有随包摘要、没有同步版本号的升级状态。
/// 用户配置与工程位于资源根之外，服务只能更新其负责的资源目录。
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

    // Android 旧包只带随包摘要，没有桌面资源版本标记；离线升级仍要保留。
    // 用户配置与工程位于 assets 之外，资源回退不能重建整个配置根。
    fail += expectTrue(
        writeTextFile(assetsRoot / ".android-bundle.sha256", "legacy-bundle"),
        "write legacy Android bundle marker");
    // 配置夹具故意不使用 JSON，确认资源服务不解析或重写上层配置。
    // 工程也按不透明字节保护，文件内容格式不属于启动同步的职责。
    fail +=
        expectTrue(writeTextFile(root / "user_config.json", "custom settings"),
                   "write existing user settings");
    fail +=
        expectTrue(writeTextFile(root / "projects/example.mmm", "user project"),
                   "write existing user project");

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

    // 离线服务不能把旧随包标记升级成未校验的远端版本，也不能改写用户文件。
    fail += expectTrue(
        readTextFile(assetsRoot / ".android-bundle.sha256") == "legacy-bundle",
        "legacy bundle marker survives offline upgrade");
    fail += expectTrue(
        readTextFile(root / "user_config.json") == "custom settings" &&
            readTextFile(root / "projects/example.mmm") == "user project",
        "offline upgrade preserves settings and projects");

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

/// @brief 验证冷启动本地 ZIP 后直接就绪，下一次普通启动才按清单补齐。
/// @details
/// 归档故意写入匹配线上清单的版本号。只有后续联网同步通过后才能保存它。
/// @par 测试输入
/// 本地源包包含一份已匹配资源和一份伪造的已验证版本标记。
/// 完整包下载地址和命中项下载地址均无可用文件。
/// 只有缺失项提供可读取的增量源；无需外部服务或 DNS。
/// @par 发布约束
/// 清单与缺失项始终可访问，因此不发出检查进度且保持缺失项可证明主动跳过。
/// 普通启动复用已导入文件，只下载缺失项。
/// 线上版本号只能来自实际完成校验的清单。
/// 非 assets 顶层条目不进入应用配置目录，源 ZIP 保持可用。
/// @return 阶段、最终文件及下载计数不符合预期的断言数。
int testImportLocalPackageDefersVerification()
{
    int        fail = 0;
    const auto root = createTempRoot();
    // 源归档与安装目录分开，首次导入不能靠它已经在目标目录来满足断言。
    const auto zip           = root / "local.zip";
    const auto assets        = root / "target/assets";
    const auto missingSource = root / "download.txt";
    const auto identical     = root / "identical.txt";
    const auto manifest      = root / "manifest.json";
    // ZIP 内文件使用门户与正式发布一致的 assets 前缀。
    // 放置与清单同版本的标记以模拟手工打包过已安装资源树。
    fail +=
        expectTrue(writeZipFile(zip,
                                { { "assets/.mmm-assets-version", "current\n" },
                                  { "assets/keep.txt", "offline" },
                                  { "outside.txt", "must not be installed" } }),
                   "write local asset package");
    // 测试摘要用独立文件计算，不依赖目标尚未建立时的路径。
    fail += expectTrue(writeTextFile(identical, "offline") &&
                           writeTextFile(missingSource, "new"),
                       "write alternate download fixtures");
    // 匹配条目的 URL 不存在。若错误下载整包或重取匹配文件，都无法得到 Updated。
    // 条目使用不同的 URL 策略。第一项可从包里复用，第二项只能由增量文件提供。
    // 版本号既与归档里一致，也会成为最终标记，可以同时验证旧版快路径被绕开。
    // 期望摘要从与 ZIP 读取独立的普通文件计算，防止修改归档直接影响测试基线。
    std::string manifestText = "{\"version\":\"current\",\"files\":[";
    manifestText += "{\"path\":\"keep.txt\",\"url\":\"" +
                    fileUrlFor(root / "unavailable.txt") + "\",\"sha256\":\"" +
                    AssetSyncService::sha256File(identical) + "\",\"size\":7},";
    manifestText += "{\"path\":\"missing.txt\",\"url\":\"" +
                    fileUrlFor(missingSource) + "\",\"sha256\":\"" +
                    AssetSyncService::sha256File(missingSource) +
                    "\",\"size\":3}]}";
    fail += expectTrue(writeTextFile(manifest, manifestText),
                       "write verification manifest");

    AssetSyncOptions options;
    options.assetsRootPath   = assets;
    options.localPackagePath = zip;
    options.manifestUrl      = fileUrlFor(manifest);
    // 即使从强制校验的重试界面选中 ZIP，本轮也应保持离线导入语义。
    options.forcePreciseVerification = true;
    // 整包和已匹配条目都使用不存在的 URL；测试成功依赖真实的增量路径选择。
    options.packageUrl = fileUrlFor(root / "unavailable-full.zip");
    std::vector<AssetSyncProgressStage> stages;
    options.progressCallback = [&](const AssetSyncProgress& progress) {
        // 回调由当前测试线程执行，文件读取与服务的阶段切换有确定先后。
        stages.push_back(progress.stage);
    };
    // 结果中的远端版本为空，调用方可据此判断尚未与线上版本比较。
    // 完成阶段仍需发布，启动页依赖它把导入进度收尾。
    const auto imported = AssetSyncService::sync(options);
    fail += expectTrue(
        imported.status == AssetSyncStatus::kReady &&
            imported.checkedFileCount == 0 && imported.updatedFileCount == 0 &&
            imported.remoteVersion.empty() && imported.errorMessage.empty() &&
            stages == std::vector{ AssetSyncProgressStage::kImportingPackage,
                                   AssetSyncProgressStage::kFinished },
        "local import is ready without checking the manifest or updating "
        "files");
    // 在空目录上建立的安装须实际写入 ZIP 内容，也不能把归档的标记当成线上证明。
    fail +=
        expectTrue(readTextFile(assets / "keep.txt") == "offline" &&
                       !std::filesystem::exists(assets / "missing.txt") &&
                       !std::filesystem::exists(assets / ".mmm-assets-version"),
                   "local import publishes only available files and leaves "
                   "version unverified");

    // 下一次未选本地包的启动恢复默认模式，以清单补齐缺失文件。
    options.localPackagePath.clear();
    options.forcePreciseVerification = false;
    // 复用同一个资源根，防止重建空目录掩盖导入后的版本状态。
    const auto result = AssetSyncService::sync(options);
    // 用检查数量区分读取 ZIP 与完整清单校验。
    // 用更新数量区分增量更新与重新下载相同内容。
    fail += expectTrue(
        result.status == AssetSyncStatus::kUpdated &&
            result.checkedFileCount == 2 && result.updatedFileCount == 1,
        "normal startup after local import verifies and downloads one file");
    // 文件落盘与版本标记必须同时生效，否则后续启动会漏检更新。
    fail += expectTrue(
        readTextFile(assets / "missing.txt") == "new" &&
            readTextFile(assets / ".mmm-assets-version") == "current\n",
        "incremental file and verified version installed");
    // 普通顶层文件可留在压缩包中，但只允许 assets 内容进入配置目录。
    fail += expectTrue(
        !std::filesystem::exists(assets.parent_path() / "outside.txt") &&
            std::filesystem::exists(zip),
        "only assets tree installed and source ZIP retained");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return fail;
}

/// @brief 验证已有资源的离线导入、取消和无效包不会覆盖无关文件。
/// @details 在同一安装目录执行三次同步，区分以下三种状态。
/// 取消不会发布任何已解压的条目。
/// 无网络但 ZIP 有效时保留用户自定义文件，版本需等到联网后再确认。
/// 无效 ZIP 在后半段报告错误时，不覆盖前一轮可用资源。
/// @return 内容、版本或同步结果不符合预期的断言数。
int testImportLocalPackageOffline()
{
    int        fail   = 0;
    const auto root   = createTempRoot();
    const auto assets = root / "target/assets";
    // 使用已经存在的目标目录，覆盖只验证首装不能触及的合并分支。
    // 旧版文件、用户定制文件和旧标记具有不同的生命周期。
    const auto zip = root / "local.zip";
    fail += expectTrue(
        writeTextFile(assets / "file.txt", "previous") &&
            writeTextFile(assets / "custom.txt", "user asset") &&
            writeTextFile(assets / ".mmm-assets-version", "previous\n") &&
            writeZipFile(zip, { { "assets/file.txt", "new offline" } }),
        "write existing and offline fixtures");
    AssetSyncOptions options;
    options.assetsRootPath   = assets;
    options.localPackagePath = zip;
    // 成功导入不需要可用清单或整包下载地址。
    options.manifestUrl = fileUrlFor(root / "no-manifest.json");
    options.packageUrl  = fileUrlFor(root / "no-full.zip");

    // 第一次进入解压进度就触发取消，目标目录还没有发生发布。
    bool cancelled           = false;
    options.progressCallback = [&](const AssetSyncProgress& progress) {
        if ( progress.stage == AssetSyncProgressStage::kImportingPackage )
            cancelled = true;
    };
    // 取消状态只用于这一轮，避免并发测试线程影响后续检查。
    options.cancellationCallback = [&] { return cancelled; };
    auto result                  = AssetSyncService::sync(options);
    // 不能把取消以错误代替，否则启动页会错误提示用户重试网络。
    // 原文件内容确认仍在，证明解压阶段与发布阶段确实隔离。
    fail += expectTrue(result.status == AssetSyncStatus::kCancelled &&
                           readTextFile(assets / "file.txt") == "previous",
                       "cancelled import preserves existing resources");

    // 同一归档再导入一次，验证取消不会遗留失效暂存或妨碍重试。
    options.progressCallback     = {};
    options.cancellationCallback = {};
    result                       = AssetSyncService::sync(options);
    // Ready 表示本地资源可继续使用，离线导入阶段不会产生网络错误。
    fail += expectTrue(
        result.status == AssetSyncStatus::kReady && result.errorMessage.empty(),
        "local import succeeds without accessing an unavailable manifest");
    // 自定义文件是源包之外的保留资源；离线情况下必须移除上一次的可信标记。
    fail += expectTrue(
        readTextFile(assets / "file.txt") == "new offline" &&
            readTextFile(assets / "custom.txt") == "user asset" &&
            !std::filesystem::exists(assets / ".mmm-assets-version"),
        "offline import overlays resource files and invalidates version");

    // ZIP 先含有效条目，再遇到 Windows 会归一化的尾空格路径。
    // POSIX 上也拒绝该组件，使同一个包在各平台采取相同的发布策略。
    // 真正资源根不能留下前一个文件。
    const auto invalid = root / "invalid.zip";
    fail +=
        expectTrue(writeZipFile(invalid,
                                { { "assets/file.txt", "must not replace" },
                                  { "assets/.. /outside.txt", "invalid" } }),
                   "write invalid package fixture");
    // 此时安装已经由有效离线包建立；失败路径的比较基准是最新可用内容。
    options.localPackagePath = invalid;
    result                   = AssetSyncService::sync(options);
    // ZIP 解析失败的部分输出只会留在暂存目录内，检查多个目标反证覆盖。
    // 失败后仍可用上一次安装的文件；父目录也不能出现非法项目。
    fail += expectTrue(
        result.status == AssetSyncStatus::kError &&
            readTextFile(assets / "file.txt") == "new offline" &&
            !std::filesystem::exists(assets.parent_path() / "outside.txt"),
        "invalid package is rejected before publishing any file");
    std::error_code error;
    std::filesystem::remove_all(root, error);
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

/// @brief 验证 SHA-256 填充边界、读取块边界及不可读路径的摘要语义。
/// @return 与独立的固定向量不一致或读取失败语义错误的断言数。
/// @details 期望由独立计算生成，避免复用生产计算生成自洽但错误的清单。
/// 每个向量独立调用公开入口，验证更新器与资产同步使用的摘要文本格式相同。
int testStreamedAssetSha256()
{
    /// @brief 由独立实现计算的纯 ASCII 'a' 测试向量。
    struct DigestCase {
        /// @brief 重复字节数，包含零字节和不足整块的尾部。
        std::size_t m_size;
        /// @brief 对应完整输入的 64 位小写十六进制 SHA-256。
        std::string_view m_expected;
    };
    // 56 字节是 SHA-256 是否追加额外填充块的边界。
    // 32 KiB 是文件读取边界，应与摘要算法自身的 64 字节分组相互独立。
    constexpr std::array cases{
        // 空文件有正常摘要，和后面不可读路径返回的空字符串严格区分。
        DigestCase{ 0,
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b78"
                    "52b855" },
        // 55 字节还可在原末块容纳填充标记和长度；56 字节必须再追加一块。
        DigestCase{ 55,
                    "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f"
                    "734318" },
        DigestCase{ 56,
                    "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686e"
                    "c6738a" },
        // 63/64 字节检查接近算法块结束时标记的位置及满块结束语义。
        DigestCase{ 63,
                    "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457d"
                    "dc2f34" },
        DigestCase{ 64,
                    "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df15"
                    "4668eb" },
        // 流读取边界左右各取一字节，覆盖短尾、刚好 EOF 与后续短块。
        // 这三个值跨越读取块，但不应该改变算法独立处理 64 字节分组的结果。
        DigestCase{ 32767,
                    "e3ab7df365190d116dd32400a57dd4a1506ac167156b8aa770d29b086c"
                    "c1cfb4" },
        DigestCase{ 32768,
                    "b217b65e6f205f41b3fb8ef90cf7c44da93f630ca03965273485bbb21a"
                    "5cccf5" },
        DigestCase{ 32769,
                    "94c9394bd6ee8fcad7b37332529186fd12354dc39b0109db7e1bce510e"
                    "25048d" },
    };

    // 独占一个临时根。持续覆盖同名文件用于检查读取不会带上上一个样例的后缀。
    const auto root  = createTempRoot();
    const auto asset = root / "hash.dat";
    int        fail  = 0;
    // 一个边界失败后继续核查其余输入，以便诊断问题在填充还是跨块读取。
    for ( const auto& sample : cases ) {
        // 保留原始字节及固定期望，不调用被测实现生成基线。
        if ( !writeTextFile(asset, std::string(sample.m_size, 'a')) ||
             AssetSyncService::sha256File(asset) != sample.m_expected ) {
            XERROR("Asset SHA-256 mismatch for {} bytes", sample.m_size);
            ++fail;
        }
    }
    // 不存在的文件或目录都不应获得空文件的合法摘要。
    // 两个场景分别覆盖目录项查询失败与输入类型不适用，不依赖进程权限。
    fail += expectTrue(AssetSyncService::sha256File(root / "missing").empty() &&
                           AssetSyncService::sha256File(root).empty(),
                       "unreadable asset has no digest");
    // 写入只在临时根，回归失败也不能留下需要维护的测试资源。
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    return fail;
}

/// @brief 经由已有资源的启动同步入口验证摘要执行中途响应取消。
/// @return 取消没有推进资源版本、开始下载或报告完成时为零。
/// @details 通过启动同步的真实精确校验路径进入，不直接调用内部摘要函数。
/// POSIX 用同一入口验证读取前后文件长度不一致时的拒绝语义。
int testCancelDuringAssetSha256()
{
    // 先有有效的本地目录，确保同步进入逐文件校验，而非初次安装包路径。
    const auto root     = createTempRoot();
    const auto assets   = root / "assets";
    const auto asset    = assets / "hash.dat";
    const auto manifest = root / "manifest.json";
    int        fail     = 0;
    // 三个完整读取块保证返回取消时摘要尚未完成。
    fail += expectTrue(writeTextFile(asset, std::string(98304, 'a')),
                       "write multi-block asset for cancellation");
    // 使用外部固定摘要，若取消失效则正常流程会发布这个版本。
    // 输入长度指向整个文件，让同步确实进入摘要，而不是先因清单大小不符退出。
    fail +=
        expectTrue(writeTextFile(manifest, R"json({"version":"after","files":[
        {"path":"hash.dat","url":"hash.dat","size":98304,
         "sha256":"6e03ec5e819aa1579fd8d3e35ea4a88cddbedc2707f1bae3fd3c08710a515831"}]})json"),
                   "write manifest for asset checksum cancellation");

    // 清单和下载根均走 file URL；外部网络状态不影响中途退出的判定。
    AssetSyncOptions options;
    options.assetsRootPath = assets;
    options.baseUrl        = fileUrlFor(root);
    options.manifestUrl    = fileUrlFor(manifest);
    // 确定性回调在同一线程运行，用普通计数和标志即可控制注入时机。
    bool checkingAsset       = false;
    bool advanced            = false;
    int  hashChecks          = 0;
    options.progressCallback = [&](const AssetSyncProgress& progress) {
        // 在单文件的检查进度之后才武装取消，清单读取不受干扰。
        if ( progress.stage == AssetSyncProgressStage::kCheckingFiles &&
             progress.currentFileIndex == 1 )
            checkingAsset = true;
        // 相对清单检查的阶段边界证明没有把取消后的部分结果拿去更新资源。
        if ( progress.stage == AssetSyncProgressStage::kDownloadingFile ||
             progress.stage == AssetSyncProgressStage::kFinished )
            advanced = true;
    };
    // 首次检查在打开文件前，第二次在首块返回时；只在第三次报告取消，
    // 证明调用方传播已经发生的取消，不依赖下一次重复询问仍然为真。
    options.cancellationCallback = [&]() {
        return checkingAsset && ++hashChecks == 3;
    };
    // 第三次询问发生在第二块读取，首块已进入摘要但整个文件尚未完成。
    const auto result = AssetSyncService::sync(options);
    // 下载和完成都不应出现；单看返回值无法发现已启动但随后取消的 I/O。
    fail +=
        expectTrue(result.status == AssetSyncStatus::kCancelled &&
                       hashChecks == 3 && !advanced &&
                       !std::filesystem::exists(assets / ".mmm-assets-version"),
                   "cancel partial hash without publishing assets");

#ifndef _WIN32
    // POSIX 可以在读取流打开时调整文件长度。清单条目的摘要故意只对应前一块，
    // 这样错误接受意外 EOF 得到的前缀摘要时会写入目标版本。
    // 把 size 设为未知，确保原文件不在哈希前就被独立的大小快路径拒绝。
    fail +=
        expectTrue(writeTextFile(manifest, R"json({"version":"after","files":[
        {"path":"hash.dat","url":"missing-hash.dat","size":0,
         "sha256":"b217b65e6f205f41b3fb8ef90cf7c44da93f630ca03965273485bbb21a5cccf5"}]})json"),
                   "write manifest with the digest of the truncation prefix");
    // 缺失的下载源使同步只能走保留已有资源的降级策略。
    // 验收以下载阶段、错误详情和未推进版号为准，不只看最终 Ready 状态。
    // 重用前一轮的配置和回调观察边界，清除上一轮已捕获的检查事件。
    checkingAsset = false;
    advanced      = false;
    hashChecks    = 0;
    std::error_code resizeError;
    options.cancellationCallback = [&]() {
        // 第二次查询在首块读取后，此时初始大小已经记录。
        // 返回 false 允许摘要继续读到 EOF，专门触发实际短读而非取消路径。
        if ( checkingAsset && ++hashChecks == 2 )
            std::filesystem::resize_file(asset, 32768, resizeError);
        return false;
    };
    const auto interrupted = AssetSyncService::sync(options);
    // 不允许用降级继续运行的返回值来证明摘要成功，因此额外检查版本标记。
    // 检查截短真的发生；前缀哈希不能让同步跳过重新下载。
    fail +=
        expectTrue(!resizeError && hashChecks >= 3 && advanced &&
                       !interrupted.errorMessage.empty() &&
                       !std::filesystem::exists(assets / ".mmm-assets-version"),
                   "reject digest of file truncated during verification");
#endif

    // 两轮共用受控根目录，所有断言都记录后再回收资源夹具。
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    return fail;
}

}  // namespace

/// @brief 依次运行全部本地资源同步场景并汇总失败数。
/// @param argc 测试入口参数数目，由套件剔除路由名称。
/// @param argv 可选传入 --hash-file 和现有文件路径，用于独立的内存规模验证。
/// @return 所有断言通过时返回 0，否则返回 1。
int main(int argc, char** argv)
{
    // 多 GiB 探针复用生产接口，由调用者准备文件，不把大资源加入普通 CTest。
    // 调用方可分别用小文件和稀疏大文件测 RSS，再与独立 SHA-256 工具对比。
    // 探针只读现有文件，RSS 测量不包含创建夹具或启动网络同步。
    if ( argc == 3 && std::string_view(argv[1]) == "--hash-file" ) {
        // 与正常 Windows 资源路径一样，从命令行 UTF-8 转入原生路径类型。
        const auto hash =
            AssetSyncService::sha256File(MMM::Config::utf8ToPath(argv[2]));
        // 失败使用进程退出码，外部基准不从日志文本猜测是否拿到了有效摘要。
        if ( hash.empty() ) return 1;
        XINFO("Asset SHA-256: {}", hash);
        return 0;
    }

    // 测试不初始化个人配置或应用资源路径，所有写入都在临时目录。
    // 每个测试返回断言失败数，累加但不因前一场景失败而短路。
    int fail = 0;
    // 先覆盖纯解析帮助器，再进入需要临时文件系统的场景。
    fail += testParseManifest();
    fail += testResolveDownloadUrl();
    fail += testDefaultOptionsUsesCdnAndHonorsEnvironment();
    fail += testCollectOutdatedFiles();
    fail += testStreamedAssetSha256();
    fail += testCancelDuringAssetSha256();
    // ZIP 安全测试位于同步策略前，失败也不阻止后续临时场景清理自身。
    fail += testExtractZipArchive();
    fail += testZipCancellationInsideEntry();
    fail += testZipEntryCountBudget();
    // 同步策略按离线降级、版本快速路径、强制修复和进度顺序执行。
    fail += testSyncKeepsExistingAssetsWhenManifestUnavailable();
    fail += testSyncSkipsPreciseCheckWhenVersionMatches();
    fail += testSyncForcePreciseCheckRepairsMissingFile();
    fail += testSyncReportsPreciseCheckProgress();
    fail += testImportLocalPackageDefersVerification();
    fail += testImportLocalPackageOffline();
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
