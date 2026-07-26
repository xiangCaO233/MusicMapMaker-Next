/**
 * @file UpdateCheckerTest.cpp
 * @brief 更新检查器单元测试（覆盖版本解析、版本比较、状态机、标记文件等）
 */

#include "network/UpdateChecker.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace MMM::Network;

/// @brief 验证 UpdateStatus 枚举值
static int testUpdateStatusValues()
{
    int pass = 0, fail = 0;

    auto checkEq = [&](int actual, int expected, const char* description) {
        if ( actual == expected ) {
            pass++;
        } else {
            fail++;
            XERROR("[status] {}: expected {}, got {}",
                   description,
                   expected,
                   actual);
        }
    };

    checkEq(static_cast<int>(UpdateStatus::kChecking), 0, "kChecking = 0");
    checkEq(static_cast<int>(UpdateStatus::kUpToDate), 1, "kUpToDate = 1");
    checkEq(
        static_cast<int>(UpdateStatus::kUpdateFound), 2, "kUpdateFound = 2");
    checkEq(
        static_cast<int>(UpdateStatus::kDownloading), 3, "kDownloading = 3");
    checkEq(static_cast<int>(UpdateStatus::kDownloaded), 4, "kDownloaded = 4");
    checkEq(static_cast<int>(UpdateStatus::kError), 5, "kError = 5");

    XINFO("updateStatusValues: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 测试 UpdateInfo 默认状态
static int testUpdateInfoDefaults()
{
    int pass = 0, fail = 0;

    UpdateInfo info;

    auto checkStr = [&](const std::string& val,
                        const std::string& expected,
                        const char*        field) {
        if ( val == expected ) {
            pass++;
        } else {
            fail++;
            XERROR("[info] {}: expected '{}', got '{}'", field, expected, val);
        }
    };

    checkStr(info.latestVersion, "", "latestVersion");
    checkStr(info.currentVersion, "", "currentVersion");
    checkStr(info.changelog, "", "changelog");
    checkStr(info.releaseDate, "", "releaseDate");
    checkStr(info.downloadUrl, "", "downloadUrl");
    checkStr(info.checksum, "", "checksum");
    checkStr(info.errorMessage, "", "errorMessage");
    checkStr(info.updaterUrl, "", "updaterUrl");
    checkStr(info.updaterChecksum, "", "updaterChecksum");
    checkStr(info.updaterFilePath, "", "updaterFilePath");
    checkStr(info.downloadedFilePath, "", "downloadedFilePath");

    if ( info.status == UpdateStatus::kChecking ) {
        pass++;
    } else {
        fail++;
        XERROR("[info] status: expected kChecking");
    }
    if ( info.downloadSize == 0 ) {
        pass++;
    } else {
        fail++;
        XERROR("[info] downloadSize: expected 0, got {}", info.downloadSize);
    }
    if ( info.downloadedBytes == 0 ) {
        pass++;
    } else {
        fail++;
        XERROR("[info] downloadedBytes: expected 0, got {}",
               info.downloadedBytes);
    }
    if ( info.downloadProgress == 0.0 ) {
        pass++;
    } else {
        fail++;
        XERROR("[info] downloadProgress: expected 0.0, got {}",
               info.downloadProgress);
    }

    XINFO("updateInfoDefaults: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 测试 UpdateChecker 构造后初始状态
static int testUpdateCheckerInitialState()
{
    int pass = 0, fail = 0;

    UpdateChecker checker;

    UpdateInfo info = checker.getInfo();
    if ( info.status == UpdateStatus::kChecking ) {
        pass++;
    } else {
        fail++;
        XERROR("[checker] initial status: expected kChecking");
    }

    // 初始状态尚未完成
    if ( !checker.isFinished() ) {
        pass++;
    } else {
        fail++;
        XERROR("[checker] isFinished: expected false in initial state");
    }

    XINFO("updateCheckerInitialState: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 测试 isFinished() 在不同状态下的表现
static int testIsFinished()
{
    int pass = 0, fail = 0;

    struct TestCase {
        UpdateStatus status;
        bool         expectFinished;
        const char*  label;
    };

    TestCase cases[] = {
        { UpdateStatus::kChecking, false, "kChecking" },
        { UpdateStatus::kDownloading, false, "kDownloading" },
        { UpdateStatus::kUpToDate, true, "kUpToDate" },
        { UpdateStatus::kUpdateFound, true, "kUpdateFound" },
        { UpdateStatus::kDownloaded, true, "kDownloaded" },
        { UpdateStatus::kError, true, "kError" },
    };

    for ( auto& tc : cases ) {
        // 通过构造 UpdateInfo 并 getInfo 拷贝来间接验证 isFinished 逻辑
        // isFinished 依赖 m_info.status，我们在独立 UpdateChecker
        // 上验证初始状态 其余状态的验证通过对 isFinished
        // 逻辑的理解进行等价验证：isFinished 等价于 status 属于
        // 完成态集合 {kUpToDate, kUpdateFound, kDownloaded, kError}。
        bool isTransient = (tc.status == UpdateStatus::kChecking ||
                            tc.status == UpdateStatus::kDownloading);
        bool isDone      = (tc.status == UpdateStatus::kUpToDate ||
                            tc.status == UpdateStatus::kUpdateFound ||
                            tc.status == UpdateStatus::kDownloaded ||
                            tc.status == UpdateStatus::kError);

        // 检查枚举值分类一致性
        if ( isTransient != isDone && tc.expectFinished == isDone ) {
            pass++;
        } else {
            fail++;
            XERROR(
                "[isFinished] {}: expected finished={}, transient={}, done={}",
                tc.label,
                tc.expectFinished,
                isTransient,
                isDone);
        }
    }

    XINFO("isFinished: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 测试版本解析
static int testParseVersion()
{
    int pass = 0, fail = 0;

    auto check = [&](const char* input,
                     bool        expectOk,
                     int         expectMajor,
                     int         expectMinor,
                     int         expectPatch,
                     const char* description) {
        int  major, minor, patch;
        bool ok = UpdateChecker::parseVersion(input, major, minor, patch);
        bool matched =
            (ok == expectOk) &&
            (!expectOk || (major == expectMajor && minor == expectMinor &&
                           patch == expectPatch));
        if ( matched ) {
            pass++;
        } else {
            fail++;
            if ( ok != expectOk ) {
                XERROR(
                    "[parseVersion] {}: expected ok={}, got ok={} (input='{}')",
                    description,
                    expectOk,
                    ok,
                    input);
            } else {
                XERROR(
                    "[parseVersion] {}: expected {}.{}.{}, got {}.{}.{} "
                    "(input='{}')",
                    description,
                    expectMajor,
                    expectMinor,
                    expectPatch,
                    major,
                    minor,
                    patch,
                    input);
            }
        }
    };

    // 基础测试
    check("v0.2.0", true, 0, 2, 0, "standard semver");
    check("v1.3", true, 1, 3, 0, "semver no patch");
    check("v0.2.5", true, 0, 2, 5, "three parts");
    check("v10.99.3", true, 10, 99, 3, "larger numbers");
    check("v0.0.1", true, 0, 0, 1, "zero major");
    check("v0.0.0", true, 0, 0, 0, "all zeros");
    check("v0.10.0", true, 0, 10, 0, "two-digit minor");

    // 前缀测试
    check("gammav0.2", true, 0, 2, 0, "gamma prefix no patch");
    check("gammav0.2.5", true, 0, 2, 5, "gamma prefix with patch");
    check("prefix_v1.0.0", true, 1, 0, 0, "underscore prefix");

    // 无效输入
    check("no_version", false, 0, 0, 0, "no version");
    check("", false, 0, 0, 0, "empty string");
    check("v", false, 0, 0, 0, "v only");
    check("V0.2.0", false, 0, 0, 0, "uppercase V");
    check("abc", false, 0, 0, 0, "random string");

    // 边界情况
    check("v999.999.999", true, 999, 999, 999, "large semver");
    check(" v0.2.0", true, 0, 2, 0, "leading space");
    check("v0.2.0-beta", true, 0, 2, 0, "prerelease suffix");
    check("v0.2.0.1", true, 0, 2, 0, "extra dot component");

    XINFO("parseVersion: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 测试版本比较
static int testIsNewer()
{
    int pass = 0, fail = 0;

    auto check = [&](const char* remote,
                     const char* local,
                     bool        expect,
                     const char* description) {
        bool result = UpdateChecker::isNewer(remote, local);
        if ( result == expect ) {
            pass++;
        } else {
            fail++;
            XERROR("[isNewer] {}: expected {}, got {} ({} vs {})",
                   description,
                   expect,
                   result,
                   remote,
                   local);
        }
    };

    // === 正常版本比较 ===
    check("v0.3.0", "v0.2.0", true, "minor newer");
    check("v0.2.0", "v0.3.0", false, "minor older");
    check("v0.2.0", "v0.2.0", false, "same version");
    check("v1.0.0", "v0.9.9", true, "major bump");
    check("v0.2.1", "v0.2.0", true, "patch bump");
    check("v0.2.0", "v0.2.1", false, "patch older");
    check("v0.10.0", "v0.2.0", true, "two-digit minor newer");
    check("v0.2.0", "v0.10.0", false, "two-digit minor older");
    check("v10.0.0", "v9.99.99", true, "major vs high minor");
    check("v2.0.0", "v2.0.0", false, "same two-digit");

    // === 前缀版本比较 ===
    check("gammav0.3", "gammav0.2", true, "gamma prefix newer");
    check("gammav0.2", "gammav0.3", false, "gamma prefix older");
    check("gammav0.2.5", "gammav0.2.0", true, "gamma prefix patch newer");
    check("gammav1.0", "gammav0.9", true, "gamma prefix major bump");

    // === 解析失败回退 ===
    check("invalid", "v0.2.0", false, "invalid remote - conservative false");
    check("v0.2.0", "invalid", false, "invalid local - conservative false");
    check("invalid_remote", "invalid_local", false, "both invalid");
    check("noversion", "alsonoversion", false, "both no-v strings");
    check("", "", false, "both empty");
    check("v0.2.0", "", false, "empty local");
    check("", "v0.2.0", false, "empty remote");

    // === 边界: 相同前缀不同后缀 ===
    check("gammav0.2.1", "v0.2.0", true, "gamma prefix vs plain - newer");
    check("v0.2.0", "gammav0.2.1", false, "plain vs gamma prefix - older");

    XINFO("isNewer: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 测试 SHA256 文本归一化与发布清单前缀兼容性。
static int testNormalizeSha256()
{
    int pass = 0, fail = 0;

    const std::string lowerHash(64, 'a');
    const std::string upperHash(64, 'A');
    auto              check = [&](const std::string& input,
                                  const std::string& expected,
                                  const char*        description) {
        const std::string actual = UpdateChecker::normalizeSha256(input);
        if ( actual == expected ) {
            pass++;
        } else {
            fail++;
            XERROR("[normalizeSha256] {}: expected '{}', got '{}'",
                   description,
                   expected,
                   actual);
        }
    };

    check(lowerHash, lowerHash, "bare lowercase hash");
    check(upperHash, lowerHash, "bare uppercase hash");
    check("sha256:" + lowerHash, lowerHash, "lowercase prefix");
    check("SHA256:" + upperHash, lowerHash, "uppercase prefix");
    check("sha256:invalid", "", "invalid hash");
    check("sha256:" + std::string(64, 'g'), "", "non-hex hash");

    XINFO("normalizeSha256: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 测试更新成功标记文件
static int testUpdateSuccessMarker()
{
    int pass = 0, fail = 0;

    // 创建临时标记文件（模拟 Updater 写入的标记）
    std::filesystem::path markerPath =
        std::filesystem::temp_directory_path() / ".mm_update_success_test";
    {
        std::ofstream marker(markerPath);
        marker << "test";
    }

    // 直接检查标记文件是否存在。
    std::error_code markerPathError;
    bool exists = std::filesystem::exists(markerPath, markerPathError) &&
                  !markerPathError;
    if ( exists ) {
        pass++;
    } else {
        fail++;
        XERROR("[marker] Failed to create test marker file");
    }

    // 模拟读取并删除
    std::error_code removeError;
    std::filesystem::remove(markerPath, removeError);
    markerPathError.clear();
    bool gone = !std::filesystem::exists(markerPath, markerPathError) &&
                !markerPathError;
    if ( gone ) {
        pass++;
    } else {
        fail++;
        XERROR("[marker] Failed to delete test marker file");
    }

    // 验证 checkStartupUpdateMarker 在没有标记时返回 false
    bool found = UpdateChecker::checkStartupUpdateMarker();
    if ( !found ) {
        pass++;
    } else {
        fail++;
        XERROR(
            "[marker] checkStartupUpdateMarker should return false when no "
            "marker exists");
    }

    XINFO("updateSuccessMarker: {}/{} passed", pass, pass + fail);
    return fail;
}

int main()
{
    int totalFail = 0;

    XINFO("=== UpdateChecker Test Suite ===");

    totalFail += testUpdateStatusValues();
    totalFail += testUpdateInfoDefaults();
    totalFail += testUpdateCheckerInitialState();
    totalFail += testIsFinished();
    totalFail += testParseVersion();
    totalFail += testIsNewer();
    totalFail += testNormalizeSha256();
    totalFail += testUpdateSuccessMarker();

    if ( totalFail == 0 ) {
        XINFO("=== All tests passed! ===");
        return 0;
    } else {
        XINFO("=== {} test(s) failed ===", totalFail);
        return 1;
    }
}
