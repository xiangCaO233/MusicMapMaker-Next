/**
 * @file UpdateCheckerTest.cpp
 * @brief 更新检查器单元测试（模拟更新流程）
 */

#include "network/UpdateChecker.h"
#include "log/colorful-log.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace MMM::Network;

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

    check("v0.2.0", true, 0, 2, 0, "standard semver");
    check("v1.3", true, 1, 3, 0, "semver no patch");
    check("gammav0.2", true, 0, 2, 0, "gamma prefix");
    check("v0.2.5", true, 0, 2, 5, "three parts");
    check("v10.99.3", true, 10, 99, 3, "larger numbers");
    check("v0.0.1", true, 0, 0, 1, "zero major");
    check("no_version", false, 0, 0, 0, "no version");
    check("", false, 0, 0, 0, "empty string");

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

    check("v0.3.0", "v0.2.0", true, "minor newer");
    check("v0.2.0", "v0.3.0", false, "minor older");
    check("v0.2.0", "v0.2.0", false, "same version");
    check("v1.0.0", "v0.9.9", true, "major bump");
    check("v0.2.1", "v0.2.0", true, "patch bump");
    check("v0.2.0", "v0.2.1", false, "patch older");
    check("v0.10.0", "v0.2.0", true, "two-digit minor");
    check("v0.2.0", "v0.10.0", false, "two-digit older");
    check("gammav0.3", "gammav0.2", true, "gamma prefix newer");
    check("invalid", "v0.2.0", true, "invalid remote falls back");
    check("v0.2.0", "invalid", true, "invalid local falls back");

    XINFO("isNewer: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 测试更新成功标记文件
static int testUpdateSuccessMarker()
{
    int pass = 0, fail = 0;

    // 创建临时标记文件（模拟 Updater 写入的标记）
    std::string markerPath = "/tmp/.mm_update_success_test";
    {
        std::ofstream marker(markerPath);
        marker << "test";
    }

    // 直接检查标记文件是否存在
    bool exists = std::filesystem::exists(markerPath);
    if ( exists ) {
        pass++;
    } else {
        fail++;
        XERROR("[marker] Failed to create test marker file");
    }

    // 模拟读取并删除
    std::filesystem::remove(markerPath);
    bool gone = !std::filesystem::exists(markerPath);
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

    totalFail += testParseVersion();
    totalFail += testIsNewer();
    totalFail += testUpdateSuccessMarker();

    if ( totalFail == 0 ) {
        XINFO("=== All tests passed! ===");
        return 0;
    } else {
        XINFO("=== {} test(s) failed ===", totalFail);
        return 1;
    }
}
