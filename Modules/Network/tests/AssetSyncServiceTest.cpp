/**
 * @file AssetSyncServiceTest.cpp
 * @brief 资源同步服务单元测试，覆盖清单解析、差异判断与 zip 安全解压。
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
std::filesystem::path createTempRoot()
{
    std::error_code       tempPathError;
    std::filesystem::path root =
        std::filesystem::temp_directory_path(tempPathError);
    if ( tempPathError ) root = ".";

    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root /= "mmm_asset_sync_test_" + std::to_string(stamp);

    std::error_code createError;
    std::filesystem::create_directories(root, createError);
    return root;
}

/// @brief 写入 UTF-8 文本测试文件。
bool writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    const auto parentPath = path.parent_path();
    if ( !parentPath.empty() ) {
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if ( createError ) return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    file << text;
    return file.good();
}

/// @brief 从文件读取 UTF-8 文本。
std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if ( !file ) return {};
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

/// @brief 将本地路径转换为 libcurl 可读取的 file URL。
std::string fileUrlFor(const std::filesystem::path& path)
{
    return "file://" + MMM::Config::pathToUtf8Generic(path);
}

/// @brief 创建 zip 文件。
bool writeZipFile(
    const std::filesystem::path&                            zipPath,
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    mz_zip_archive zipArchive{};
    if ( !mz_zip_writer_init_file(
             &zipArchive, MMM::Config::pathToUtf8(zipPath).c_str(), 0) ) {
        return false;
    }

    bool success = true;
    for ( const auto& [name, content] : entries ) {
        if ( !mz_zip_writer_add_mem(&zipArchive,
                                    name.c_str(),
                                    content.data(),
                                    content.size(),
                                    MZ_BEST_SPEED) ) {
            success = false;
            break;
        }
    }

    if ( success && !mz_zip_writer_finalize_archive(&zipArchive) ) {
        success = false;
    }
    mz_zip_writer_end(&zipArchive);
    return success;
}

/// @brief 检查条件并记录失败。
int expectTrue(bool condition, const char* label)
{
    if ( condition ) {
        XINFO("[asset-sync] PASS: {}", label);
        return 0;
    }

    XERROR("[asset-sync] FAIL: {}", label);
    return 1;
}

/// @brief 测试 manifest 解析。
int testParseManifest()
{
    int fail = 0;

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

    std::string errorMessage;
    const auto  manifest =
        AssetSyncService::parseManifest(validManifest, errorMessage);
    fail += expectTrue(manifest.has_value(), "valid manifest parses");
    if ( manifest ) {
        fail += expectTrue(manifest->version == "v0.4.0-assets.1",
                           "manifest version parsed");
        fail += expectTrue(manifest->packageSha256 ==
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                               "aaaaaaaaaaaaaaaaa",
                           "package checksum normalized");
        fail += expectTrue(manifest->files.size() == 1,
                           "manifest file count parsed");
        fail += expectTrue(manifest->files.front().sha256 ==
                               "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                               "bbbbbbbbbbbbbbbbb",
                           "file checksum normalized");
    }

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

    errorMessage.clear();
    const auto unsafe =
        AssetSyncService::parseManifest(unsafeManifest, errorMessage);
    fail += expectTrue(!unsafe.has_value(), "unsafe manifest path rejected");
    fail += expectTrue(!errorMessage.empty(),
                       "unsafe manifest reports error message");

    return fail;
}

/// @brief 测试下载 URL 补全。
int testResolveDownloadUrl()
{
    int fail = 0;

    fail +=
        expectTrue(AssetSyncService::resolveDownloadUrl(
                       "https://mmm.xiang233.top", "/download/assets.zip") ==
                       "https://mmm.xiang233.top/download/assets.zip",
                   "absolute path URL resolved against base");
    fail +=
        expectTrue(AssetSyncService::resolveDownloadUrl(
                       "https://mmm.xiang233.top/", "download/assets.zip") ==
                       "https://mmm.xiang233.top/download/assets.zip",
                   "relative path URL resolved against base");
    fail += expectTrue(
        AssetSyncService::resolveDownloadUrl(
            "https://mmm.xiang233.top", "https://cdn.example.com/assets.zip") ==
            "https://cdn.example.com/assets.zip",
        "absolute URL preserved");

    return fail;
}

/// @brief 测试本地差异收集。
int testCollectOutdatedFiles()
{
    int fail = 0;

    const auto root       = createTempRoot();
    const auto assetsRoot = root / "assets";
    const auto samePath   = assetsRoot / "same.txt";
    const auto oldPath    = assetsRoot / "old.txt";
    const auto newPath    = root / "new.txt";

    fail += expectTrue(writeTextFile(samePath, "same"), "write same fixture");
    fail += expectTrue(writeTextFile(oldPath, "old"), "write old fixture");
    fail += expectTrue(writeTextFile(newPath, "new"), "write new fixture");

    AssetManifest manifest;
    manifest.version = "v-test";
    manifest.files.push_back(AssetFileEntry{
        "same.txt",
        "/same.txt",
        AssetSyncService::sha256File(samePath),
        4,
    });
    manifest.files.push_back(AssetFileEntry{
        "old.txt",
        "/old.txt",
        AssetSyncService::sha256File(newPath),
        3,
    });
    manifest.files.push_back(AssetFileEntry{
        "missing.txt",
        "/missing.txt",
        AssetSyncService::sha256File(newPath),
        3,
    });

    const auto outdated =
        AssetSyncService::collectOutdatedFiles(manifest, assetsRoot);
    fail += expectTrue(outdated.size() == 2,
                       "collects changed and missing files only");
    fail +=
        expectTrue(outdated[0].path == "old.txt", "changed file listed first");
    fail += expectTrue(outdated[1].path == "missing.txt",
                       "missing file listed second");

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    return fail;
}

/// @brief 测试 zip 解压路径安全。
int testExtractZipArchive()
{
    int fail = 0;

    const auto root    = createTempRoot();
    const auto zipPath = root / "assets.zip";
    const auto outRoot = root / "out";

    fail += expectTrue(
        writeZipFile(zipPath,
                     {
                         { "assets/skins/mmm-default/skin.lua", "skin" },
                         { "assets/readme.txt", "hello" },
                     }),
        "write safe zip fixture");

    std::string errorMessage;
    fail += expectTrue(
        AssetSyncService::extractZipArchive(zipPath, outRoot, errorMessage),
        "safe zip extracts");
    fail += expectTrue(
        readTextFile(outRoot / "assets/skins/mmm-default/skin.lua") == "skin",
        "skin file extracted");
    fail += expectTrue(readTextFile(outRoot / "assets/readme.txt") == "hello",
                       "readme file extracted");

    const auto badZipPath = root / "bad.zip";
    fail += expectTrue(writeZipFile(badZipPath, { { "../evil.txt", "evil" } }),
                       "write unsafe zip fixture");

    errorMessage.clear();
    fail += expectTrue(
        !AssetSyncService::extractZipArchive(badZipPath, outRoot, errorMessage),
        "unsafe zip path rejected");
    fail += expectTrue(!errorMessage.empty(), "unsafe zip reports error");

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    return fail;
}

/// @brief 测试已有本地资源时远程 manifest 不可用也允许启动。
int testSyncKeepsExistingAssetsWhenManifestUnavailable()
{
    int fail = 0;

    const auto root       = createTempRoot();
    const auto assetsRoot = root / "assets";
    fail += expectTrue(
        writeTextFile(assetsRoot / "skins/mmm-default/skin.lua", "skin"),
        "write existing local asset");

    AssetSyncOptions options;
    options.assetsRootPath = assetsRoot;
    options.baseUrl        = "https://invalid.local";
    options.manifestUrl =
        "file://" + MMM::Config::pathToUtf8(root / "missing-manifest.json");
    options.packageUrl = {};

    const auto result = AssetSyncService::sync(options);
    fail += expectTrue(result.status == AssetSyncStatus::kReady,
                       "existing assets survive missing manifest");
    fail += expectTrue(!result.errorMessage.empty(),
                       "missing manifest warning is retained");

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    return fail;
}

/// @brief 测试本地版本一致时跳过精确文件校验。
int testSyncSkipsPreciseCheckWhenVersionMatches()
{
    int fail = 0;

    const auto root       = createTempRoot();
    const auto assetsRoot = root / "assets";
    const auto wantedPath = root / "wanted.txt";
    fail += expectTrue(writeTextFile(wantedPath, "wanted"),
                       "write wanted checksum fixture");
    fail += expectTrue(writeTextFile(assetsRoot / "theme.txt", "stale"),
                       "write stale local asset");
    fail += expectTrue(
        writeTextFile(assetsRoot / ".mmm-assets-version", "v-skip\n"),
        "write matching local asset version");

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
    fail += expectTrue(writeTextFile(manifestPath, manifestText),
                       "write version skip manifest");

    bool sawFileCheck = false;

    AssetSyncOptions options;
    options.assetsRootPath = assetsRoot;
    options.baseUrl        = "https://invalid.local";
    options.manifestUrl    = fileUrlFor(manifestPath);
    options.packageUrl     = {};
    options.progressCallback =
        [&sawFileCheck](const AssetSyncProgress& progress) {
            if ( progress.stage == AssetSyncProgressStage::kCheckingFiles ) {
                sawFileCheck = true;
            }
        };

    const auto result = AssetSyncService::sync(options);
    fail += expectTrue(result.status == AssetSyncStatus::kReady,
                       "matching version reports ready");
    fail += expectTrue(result.errorMessage.empty(),
                       "matching version does not hit missing download");
    fail += expectTrue(result.checkedFileCount == 0,
                       "matching version skips file hash checks");
    fail += expectTrue(!sawFileCheck,
                       "matching version emits no file check progress");
    fail += expectTrue(readTextFile(assetsRoot / "theme.txt") == "stale",
                       "matching version leaves local files untouched");

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    return fail;
}

/// @brief 测试版本不一致时逐文件校验会汇报进度。
int testSyncReportsPreciseCheckProgress()
{
    int fail = 0;

    const auto root       = createTempRoot();
    const auto assetsRoot = root / "assets";
    const auto assetPath  = assetsRoot / "same.txt";
    fail += expectTrue(writeTextFile(assetPath, "same"),
                       "write matching local asset");

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
    fail += expectTrue(writeTextFile(manifestPath, manifestText),
                       "write progress manifest");

    bool        sawFileCheck       = false;
    std::size_t lastFileIndex      = 0;
    std::size_t lastTotalFileCount = 0;

    AssetSyncOptions options;
    options.assetsRootPath = assetsRoot;
    options.baseUrl        = "https://invalid.local";
    options.manifestUrl    = fileUrlFor(manifestPath);
    options.packageUrl     = {};
    options.progressCallback =
        [&sawFileCheck, &lastFileIndex, &lastTotalFileCount](
            const AssetSyncProgress& progress) {
            if ( progress.stage != AssetSyncProgressStage::kCheckingFiles ) {
                return;
            }
            sawFileCheck       = true;
            lastFileIndex      = progress.currentFileIndex;
            lastTotalFileCount = progress.totalFileCount;
        };

    const auto result = AssetSyncService::sync(options);
    fail += expectTrue(result.status == AssetSyncStatus::kReady,
                       "matching files report ready after precise check");
    fail +=
        expectTrue(result.errorMessage.empty(), "precise check has no error");
    fail += expectTrue(result.checkedFileCount == 1,
                       "precise check counts manifest files");
    fail += expectTrue(result.updatedFileCount == 0,
                       "precise check downloads no matching files");
    fail += expectTrue(sawFileCheck, "precise check emits progress");
    fail += expectTrue(lastFileIndex == 1 && lastTotalFileCount == 1,
                       "precise check progress includes file count");
    fail += expectTrue(
        readTextFile(assetsRoot / ".mmm-assets-version") == "v-progress\n",
        "precise check writes new local version");

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    return fail;
}

}  // namespace

int main()
{
    int fail = 0;
    fail += testParseManifest();
    fail += testResolveDownloadUrl();
    fail += testCollectOutdatedFiles();
    fail += testExtractZipArchive();
    fail += testSyncKeepsExistingAssetsWhenManifestUnavailable();
    fail += testSyncSkipsPreciseCheckWhenVersionMatches();
    fail += testSyncReportsPreciseCheckProgress();

    if ( fail == 0 ) {
        XINFO("AssetSyncServiceTest passed.");
    } else {
        XERROR("AssetSyncServiceTest failed: {}", fail);
    }
    return fail == 0 ? 0 : 1;
}
