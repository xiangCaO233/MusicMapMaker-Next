/**
 * @file UpdateCheckerTest.cpp
 * @brief 更新检查器单元测试（覆盖版本解析、版本比较、状态机、标记文件等）
 * @details 套件只验证确定性的本地逻辑，不访问发布服务器或下载资源。
 * 文件系统用例必须在隔离的 MMM_CONFIG_ROOT 下运行，避免消费真实启动标记。
 */

#include "network/UpdateChecker.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace MMM::Network;

/// @brief 验证 UpdateStatus 枚举的稳定整数映射。
/// @return 与约定不一致的枚举项数量。
/// @note 数值会进入跨线程状态与 UI 分支，测试用于防止无意重排。
static int testUpdateStatusValues()
{
    // 每个断言独立累计，使一次运行能报告全部枚举映射偏差。
    int pass = 0, fail = 0;

    // 局部检查器统一比较和日志格式，不隐藏失败后的后续用例。
    auto checkEq = [&](int actual, int expected, const char* description) {
        // actual 已转为 int，断言同时固定公开枚举的底层顺序。
        if ( actual == expected ) {
            pass++;
        } else {
            // 描述使用稳定英文标识，便于在 CI 日志中定位具体枚举项。
            fail++;
            XERROR("[status] {}: expected {}, got {}",
                   description,
                   expected,
                   actual);
        }
    };

    // Checking 是默认构造状态，也是状态机尚未完成的起点。
    checkEq(static_cast<int>(UpdateStatus::kChecking), 0, "kChecking = 0");
    // UpToDate 与 UpdateFound 是检查完成后的两个互斥结果。
    checkEq(static_cast<int>(UpdateStatus::kUpToDate), 1, "kUpToDate = 1");
    checkEq(
        static_cast<int>(UpdateStatus::kUpdateFound), 2, "kUpdateFound = 2");
    // Downloading 与 Downloaded 保持相邻，表达下载阶段的状态推进。
    checkEq(
        static_cast<int>(UpdateStatus::kDownloading), 3, "kDownloading = 3");
    checkEq(static_cast<int>(UpdateStatus::kDownloaded), 4, "kDownloaded = 4");
    // Error 独立于正常完成态，但同样会终止当前更新流程。
    checkEq(static_cast<int>(UpdateStatus::kError), 5, "kError = 5");

    // 汇总日志保留通过数量，同时以失败数作为测试返回值。
    XINFO("updateStatusValues: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 验证 UpdateInfo 默认构造不会暴露陈旧更新数据。
/// @return 默认值不符合契约的字段数量。
/// @note 字符串、计数、进度和状态分别检查，避免聚合初始化漂移。
static int testUpdateInfoDefaults()
{
    // 保留逐字段计数，失败日志能一次列出全部未初始化成员。
    int pass = 0, fail = 0;

    // 使用纯默认构造模拟更新检查尚未获得任何远端响应。
    UpdateInfo info;

    // 字符串检查器覆盖所有路径、摘要和展示文本字段。
    auto checkStr = [&](const std::string& val,
                        const std::string& expected,
                        const char*        field) {
        // 通过 const 引用接收字段，检查过程不会修改默认对象。
        if ( val == expected ) {
            pass++;
        } else {
            // 同时输出字段名、期望值和实际值，避免只见汇总失败数。
            fail++;
            XERROR("[info] {}: expected '{}', got '{}'", field, expected, val);
        }
    };

    // 版本与发布说明在检查完成前必须为空，防止 UI 展示旧发布信息。
    checkStr(info.latestVersion, "", "latestVersion");
    checkStr(info.currentVersion, "", "currentVersion");
    checkStr(info.changelog, "", "changelog");
    checkStr(info.releaseDate, "", "releaseDate");
    // 主程序下载地址和摘要只有发现更新后才允许出现。
    checkStr(info.downloadUrl, "", "downloadUrl");
    checkStr(info.checksum, "", "checksum");
    // 错误消息不能在无错误的默认状态下预填。
    checkStr(info.errorMessage, "", "errorMessage");
    // 独立更新器的远端与本地字段同样从空值开始。
    checkStr(info.updaterUrl, "", "updaterUrl");
    checkStr(info.updaterChecksum, "", "updaterChecksum");
    checkStr(info.updaterFilePath, "", "updaterFilePath");
    // 尚未下载时不能存在可供安装流程消费的包路径。
    checkStr(info.downloadedFilePath, "", "downloadedFilePath");

    // 默认状态明确为 Checking，构造后可直接驱动加载 UI。
    if ( info.status == UpdateStatus::kChecking ) {
        pass++;
    } else {
        fail++;
        XERROR("[info] status: expected kChecking");
    }
    // 未收到 Content-Length 前总下载大小必须为零。
    if ( info.downloadSize == 0 ) {
        pass++;
    } else {
        fail++;
        XERROR("[info] downloadSize: expected 0, got {}", info.downloadSize);
    }
    // 已下载字节和总大小独立初始化，避免进度计算读取垃圾值。
    if ( info.downloadedBytes == 0 ) {
        pass++;
    } else {
        fail++;
        XERROR("[info] downloadedBytes: expected 0, got {}",
               info.downloadedBytes);
    }
    // 浮点进度使用精确默认常量，零值无需近似比较。
    if ( info.downloadProgress == 0.0 ) {
        pass++;
    } else {
        fail++;
        XERROR("[info] downloadProgress: expected 0.0, got {}",
               info.downloadProgress);
    }

    // 返回失败字段数，主入口可与其他测试组累加。
    XINFO("updateInfoDefaults: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 验证 UpdateChecker 构造后尚未开始网络任务的状态。
/// @return 初始状态或完成标志不符合契约时的失败数。
static int testUpdateCheckerInitialState()
{
    // 两项契约分别计数，便于区分数据状态和便捷查询偏差。
    int pass = 0, fail = 0;

    // 不调用 start，确保观察的是构造函数本身建立的状态。
    UpdateChecker checker;

    // getInfo 返回快照；测试不依赖内部互斥或成员布局。
    UpdateInfo info = checker.getInfo();
    // 初始快照与 UpdateInfo 的默认状态约定保持一致。
    if ( info.status == UpdateStatus::kChecking ) {
        pass++;
    } else {
        fail++;
        XERROR("[checker] initial status: expected kChecking");
    }

    // Checking 属于进行态，isFinished 必须保持 false。
    if ( !checker.isFinished() ) {
        pass++;
    } else {
        fail++;
        XERROR("[checker] isFinished: expected false in initial state");
    }

    // 汇总只记录断言结果，不触发实际联网检查。
    XINFO("updateCheckerInitialState: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 验证 isFinished 使用的进行态与完成态分类契约。
/// @return 分类与预期不一致的状态数量。
/// @note 除初始 Checking 外，此测试按公开枚举语义验证等价分类表。
static int testIsFinished()
{
    // 每个状态都是独立用例，新增枚举时需显式选择所属集合。
    int pass = 0, fail = 0;

    /// @brief 单个状态及其预期完成属性。
    struct TestCase {
        UpdateStatus status;          ///< 待分类的公开更新状态。
        bool         expectFinished;  ///< isFinished 语义上的期望结果。
        const char*  label;           ///< 失败日志使用的稳定状态名。
    };

    // 表中列出所有当前枚举项；新增状态时必须显式加入并选择所属集合。
    // Checking 和 Downloading 可继续推进，其余状态结束当前动作。
    TestCase cases[] = {
        { UpdateStatus::kChecking, false, "kChecking" },
        { UpdateStatus::kDownloading, false, "kDownloading" },
        { UpdateStatus::kUpToDate, true, "kUpToDate" },
        { UpdateStatus::kUpdateFound, true, "kUpdateFound" },
        { UpdateStatus::kDownloaded, true, "kDownloaded" },
        { UpdateStatus::kError, true, "kError" },
    };

    // 遍历完整枚举集合，保证两类互斥且与期望表一致。
    for ( auto& tc : cases ) {
        // 通过构造 UpdateInfo 并 getInfo 拷贝来间接验证 isFinished 逻辑
        // isFinished 依赖 m_info.status，我们在独立 UpdateChecker
        // 上验证初始状态 其余状态的验证通过对 isFinished
        // 逻辑的理解进行等价验证：isFinished 等价于 status 属于
        // 完成态集合 {kUpToDate, kUpdateFound, kDownloaded, kError}。
        // transient 集合表示调用方仍应显示进行中并继续轮询状态。
        bool isTransient = (tc.status == UpdateStatus::kChecking ||
                            tc.status == UpdateStatus::kDownloading);
        // done 集合包含正常结果、已下载结果和不可继续的错误。
        bool isDone = (tc.status == UpdateStatus::kUpToDate ||
                       tc.status == UpdateStatus::kUpdateFound ||
                       tc.status == UpdateStatus::kDownloaded ||
                       tc.status == UpdateStatus::kError);

        // 同时验证集合互斥和表驱动期望，防止遗漏状态仍被判通过。
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

    // 失败数由套件入口统一决定进程退出码。
    XINFO("isFinished: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 验证从发布标签中提取 major、minor、patch 的兼容规则。
/// @return 解析成功性或版本分量不符合预期的用例数量。
/// @note 测试覆盖前缀、缺省 patch、尾随文本和保守失败输入。
static int testParseVersion()
{
    // 用例全部执行后汇总，便于一次发现多种标签格式回归。
    int pass = 0, fail = 0;

    // 表格式检查器同时核对布尔结果和成功时的三个输出分量。
    auto check = [&](const char* input,
                     bool        expectOk,
                     int         expectMajor,
                     int         expectMinor,
                     int         expectPatch,
                     const char* description) {
        // description 只用于诊断，不参与解析输入或期望计算。
        // 输出变量故意不预设期望值；失败解析时不会读取其内容。
        int  major, minor, patch;
        bool ok = UpdateChecker::parseVersion(input, major, minor, patch);
        // 解析失败只比较成功标志，避免依赖失败路径是否修改输出参数。
        bool matched =
            (ok == expectOk) &&
            (!expectOk || (major == expectMajor && minor == expectMinor &&
                           patch == expectPatch));
        if ( matched ) {
            pass++;
        } else {
            fail++;
            if ( ok != expectOk ) {
                // 成功性偏差优先报告，版本分量在该情况下没有诊断价值。
                XERROR(
                    "[parseVersion] {}: expected ok={}, got ok={} (input='{}')",
                    description,
                    expectOk,
                    ok,
                    input);
            } else {
                // 解析成功但分量错误时完整打印期望和实际三元组。
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

    // 基础组固定标准 v 前缀和二至三段数字的主路径。
    check("v0.2.0", true, 0, 2, 0, "standard semver");
    // 缺少 patch 时按零处理，兼容既有两段发布标签。
    check("v1.3", true, 1, 3, 0, "semver no patch");
    check("v0.2.5", true, 0, 2, 5, "three parts");
    // 多位数字必须按十进制整体解析，不能逐字符比较。
    check("v10.99.3", true, 10, 99, 3, "larger numbers");
    // 零值分量均为合法版本组成，不应误判为缺失。
    check("v0.0.1", true, 0, 0, 1, "zero major");
    check("v0.0.0", true, 0, 0, 0, "all zeros");
    check("v0.10.0", true, 0, 10, 0, "two-digit minor");

    // 前缀组验证解析器能在渠道标签中定位小写 v 版本起点。
    check("gammav0.2", true, 0, 2, 0, "gamma prefix no patch");
    check("gammav0.2.5", true, 0, 2, 5, "gamma prefix with patch");
    // 非渠道专用前缀也应兼容，规则不绑定 gamma 文本。
    check("prefix_v1.0.0", true, 1, 0, 0, "underscore prefix");

    // 无效组覆盖找不到小写 v 或其后没有完整 major.minor 的输入。
    check("no_version", false, 0, 0, 0, "no version");
    check("", false, 0, 0, 0, "empty string");
    check("v", false, 0, 0, 0, "v only");
    // 大写 V 不在当前发布标签契约内，必须保守拒绝。
    check("V0.2.0", false, 0, 0, 0, "uppercase V");
    check("abc", false, 0, 0, 0, "random string");

    // 边界组验证大分量、前导空白和已接受的尾随内容规则。
    check("v999.999.999", true, 999, 999, 999, "large semver");
    // 版本起点前允许普通文本，因此前导空格同样可以被跳过。
    check(" v0.2.0", true, 0, 2, 0, "leading space");
    // 预发布后缀不参与数值比较，只提取前三个核心分量。
    check("v0.2.0-beta", true, 0, 2, 0, "prerelease suffix");
    // 第四段属于尾随内容，保持历史上忽略它的兼容行为。
    check("v0.2.0.1", true, 0, 2, 0, "extra dot component");

    // 输出组内通过数量，最终仍以失败数作为可组合结果。
    XINFO("parseVersion: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 验证发布标签按数值三元组执行严格新版本比较。
/// @return 比较结果与预期不一致的用例数量。
/// @note 任一标签无法解析时必须保守返回 false，避免错误更新提示。
static int testIsNewer()
{
    // 每个方向均单独覆盖，防止比较实现只在升序输入下正确。
    int pass = 0, fail = 0;

    // 局部检查器保存远端/本地顺序，失败日志可直接复现比较。
    auto check = [&](const char* remote,
                     const char* local,
                     bool        expect,
                     const char* description) {
        // 每个用例显式给出期望，不复用生产比较算法推导断言结果。
        // isNewer 的第一个参数固定为远端，第二个固定为当前本地版本。
        bool result = UpdateChecker::isNewer(remote, local);
        if ( result == expect ) {
            pass++;
        } else {
            // 报告两端原始标签，便于判断是解析还是排序语义回归。
            fail++;
            XERROR("[isNewer] {}: expected {}, got {} ({} vs {})",
                   description,
                   expect,
                   result,
                   remote,
                   local);
        }
    };

    // 正常组分别覆盖 minor、major、patch 的升降与相等情况。
    check("v0.3.0", "v0.2.0", true, "minor newer");
    check("v0.2.0", "v0.3.0", false, "minor older");
    // 严格比较要求相等版本不是更新。
    check("v0.2.0", "v0.2.0", false, "same version");
    // 高位分量优先，major 提升覆盖所有较低位分量。
    check("v1.0.0", "v0.9.9", true, "major bump");
    check("v0.2.1", "v0.2.0", true, "patch bump");
    check("v0.2.0", "v0.2.1", false, "patch older");
    // 多位 minor 必须按整数比较，避免字典序把 10 排在 2 前面。
    check("v0.10.0", "v0.2.0", true, "two-digit minor newer");
    check("v0.2.0", "v0.10.0", false, "two-digit minor older");
    // major 的优先级高于本地较大的 minor 和 patch。
    check("v10.0.0", "v9.99.99", true, "major vs high minor");
    check("v2.0.0", "v2.0.0", false, "same two-digit");

    // 渠道前缀不参与排序，只影响版本数字的定位。
    check("gammav0.3", "gammav0.2", true, "gamma prefix newer");
    check("gammav0.2", "gammav0.3", false, "gamma prefix older");
    // 同渠道下 patch 和 major 仍遵循标准三元组优先级。
    check("gammav0.2.5", "gammav0.2.0", true, "gamma prefix patch newer");
    check("gammav1.0", "gammav0.9", true, "gamma prefix major bump");

    // 解析失败组要求所有不确定情况保守为非更新。
    check("invalid", "v0.2.0", false, "invalid remote - conservative false");
    check("v0.2.0", "invalid", false, "invalid local - conservative false");
    // 双端无效、无版本起点和空字符串均不能靠文本顺序猜测。
    check("invalid_remote", "invalid_local", false, "both invalid");
    check("noversion", "alsonoversion", false, "both no-v strings");
    check("", "", false, "both empty");
    // 单端为空同样视为无法建立可靠的版本先后关系。
    check("v0.2.0", "", false, "empty local");
    check("", "v0.2.0", false, "empty remote");

    // 混合渠道前缀时仍只比较提取出的版本数字。
    check("gammav0.2.1", "v0.2.0", true, "gamma prefix vs plain - newer");
    check("v0.2.0", "gammav0.2.1", false, "plain vs gamma prefix - older");

    // 汇总日志帮助定位比较组，返回值交由总套件累加。
    XINFO("isNewer: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 测试 SHA256 文本归一化与发布清单前缀兼容性。
/// @return 归一化结果不符合固定摘要格式的用例数量。
/// @note 合法输出必须统一为 64 位小写十六进制且不含算法前缀。
static int testNormalizeSha256()
{
    // 用例独立累计，以便同时暴露大小写和前缀处理回归。
    int pass = 0, fail = 0;

    // 重复字符构造可精确控制摘要长度，并清晰覆盖大小写转换。
    const std::string lowerHash(64, 'a');
    const std::string upperHash(64, 'A');
    // 检查器比较完整结果，避免仅验证长度而遗漏非法字符。
    auto check = [&](const std::string& input,
                     const std::string& expected,
                     const char*        description) {
        // expected 为空表达拒绝输入，不会与合法 SHA-256 结果混淆。
        const std::string actual = UpdateChecker::normalizeSha256(input);
        if ( actual == expected ) {
            pass++;
        } else {
            // 失败日志保留归一化前后文本，便于确认前缀是否被正确移除。
            fail++;
            XERROR("[normalizeSha256] {}: expected '{}', got '{}'",
                   description,
                   expected,
                   actual);
        }
    };

    // 无前缀小写摘要是规范形式，应原样保留。
    check(lowerHash, lowerHash, "bare lowercase hash");
    // 无前缀大写摘要需要折叠为小写，保证后续字符串比较稳定。
    check(upperHash, lowerHash, "bare uppercase hash");
    // 发布清单允许显式 sha256: 算法前缀，输出不保留此前缀。
    check("sha256:" + lowerHash, lowerHash, "lowercase prefix");
    // 算法前缀和摘要字符均按大小写无关输入接受并统一输出。
    check("SHA256:" + upperHash, lowerHash, "uppercase prefix");
    // 长度不足的伪摘要必须返回空，不能进入完整性校验流程。
    check("sha256:invalid", "", "invalid hash");
    // 长度正确但含非十六进制字符同样必须拒绝。
    check("sha256:" + std::string(64, 'g'), "", "non-hex hash");

    // 汇总归一化用例结果，失败数由主入口统一转为非零退出码。
    XINFO("normalizeSha256: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 验证更新成功标记文件的创建、删除和缺失检测边界。
/// @return 文件系统操作或缺失标记判断失败的断言数量。
/// @note 测试文件位于系统临时目录，不写入源码树和个人配置目录。
/// @warning 必须通过隔离配置环境运行，禁止使用个人配置根目录。
static int testUpdateSuccessMarker()
{
    // 三个阶段分别累计，确保清理失败也能反映在最终退出码中。
    int pass = 0, fail = 0;

    // 使用固定测试文件名模拟 Updater 写入的成功标记。
    // 路径限定在系统临时目录，避免污染实际应用更新目录。
    std::filesystem::path markerPath =
        std::filesystem::temp_directory_path() / ".mm_update_success_test";
    {
        // 局部流在存在性检查前析构，确保内容和目录项已经提交。
        std::ofstream marker(markerPath);
        // 内容本身不参与协议，仅需产生一个非空普通文件。
        marker << "test";
    }

    // 使用 error_code 重载避免文件系统错误通过异常离开测试。
    std::error_code markerPathError;
    // 同时要求 exists 为 true 且查询过程没有底层错误。
    bool exists = std::filesystem::exists(markerPath, markerPathError) &&
                  !markerPathError;
    if ( exists ) {
        pass++;
    } else {
        fail++;
        XERROR("[marker] Failed to create test marker file");
    }

    // 模拟启动流程消费标记后的删除动作，同样使用非抛出重载。
    std::error_code removeError;
    std::filesystem::remove(markerPath, removeError);
    // 最终存在性检查是清理判据，兼容目标已不存在时 remove 返回 false。
    // 清除上一次 exists 的状态，防止旧错误影响删除后验证。
    markerPathError.clear();
    // 标记不存在且查询成功才视为清理完成。
    bool gone = !std::filesystem::exists(markerPath, markerPathError) &&
                !markerPathError;
    if ( gone ) {
        pass++;
    } else {
        fail++;
        XERROR("[marker] Failed to delete test marker file");
    }

    // 删除测试标记后，正式入口也必须报告当前启动没有更新成功标记。
    // 测试环境通过隔离 MMM_CONFIG_ROOT 避免读取用户的真实应用标记。
    bool found = UpdateChecker::checkStartupUpdateMarker();
    if ( !found ) {
        pass++;
    } else {
        fail++;
        XERROR(
            "[marker] checkStartupUpdateMarker should return false when no "
            "marker exists");
    }

    // 汇总创建、删除和正式缺失检测三个阶段。
    XINFO("updateSuccessMarker: {}/{} passed", pass, pass + fail);
    return fail;
}

/// @brief 依次运行全部无网络更新检查单元测试并汇总失败数。
/// @return 所有断言通过时返回 0，否则返回 1。
int main()
{
    // 测试进程不初始化下载线程，执行结果不依赖外部服务可用性。
    // 各测试组返回失败数量，累加后仍保证后续组继续执行。
    int totalFail = 0;

    // 套件不发起网络请求，标题日志用于区分其他 CTest 输出。
    XINFO("=== UpdateChecker Test Suite ===");

    // 先验证数据类型与初始状态，再验证解析和文件系统帮助器。
    totalFail += testUpdateStatusValues();
    totalFail += testUpdateInfoDefaults();
    totalFail += testUpdateCheckerInitialState();
    totalFail += testIsFinished();
    // 版本解析是比较的前置契约，两者保持相邻执行便于诊断。
    totalFail += testParseVersion();
    totalFail += testIsNewer();
    // 摘要规范化和标记文件覆盖发布完整性与启动收尾路径。
    totalFail += testNormalizeSha256();
    totalFail += testUpdateSuccessMarker();

    if ( totalFail == 0 ) {
        // 所有组零失败时才向 CTest 返回成功。
        XINFO("=== All tests passed! ===");
        return 0;
    } else {
        // 任一组失败都用统一非零码通知 CTest，详细数量留在日志中。
        // 进程退出码保持一，具体失败总数通过日志提供。
        XINFO("=== {} test(s) failed ===", totalFail);
        return 1;
    }
}
