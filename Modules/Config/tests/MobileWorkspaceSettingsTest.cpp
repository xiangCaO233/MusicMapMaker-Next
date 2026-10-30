/// @file MobileWorkspaceSettingsTest.cpp
/// @brief 检查真实配置文件上的移动布局恢复契约。
/// @details 用不对称位置、非默认比例和高位标签哈希区分各字段来源。
/// 既测试纯序列化，又验证后台写入完成后重新加载的最终状态。
/// 测试不加载个人配置、不使用平台窗口，也不模拟网络或音频设备。
#include "config/MobileWorkspaceSettings.h"
#include "ConfigSnapshotWriter.h"
#include "config/AppConfig.h"
#include "config/VisualConfig.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

/// @brief 验证移动布局重启恢复与桌面配置隔离，不启动图形或平台窗口。
/// @param argc 必须提供输出目录，禁止回退源码路径写测试文件。
/// @param argv CTest 分配的独立目录，不读取实际用户配置。
/// @return 所有保存、边界与恢复契约成立时为零。
/// @details 顺序覆盖默认值、标量往返、错误输入、实际落盘和重新加载。
/// 每个检查使用对外 API，不写入 AppConfig 的私有字段来制造测试结果。
/// 保存等待采用配置层真实队列屏障，不能因系统负载变化偶发读取旧文件。
/// 这里不测试桌面停靠渲染，只比较移动操作前后的完整桌面视觉值。
int main(int argc, char** argv)
{
    using namespace MMM::Config;
    if ( argc != 2 ) return 1;
    // 目录由 CTest 显式指定，路径错误直接失败，禁止静默回退当前工作目录。
    std::error_code             error;
    const std::filesystem::path root(argv[1]);
    std::filesystem::create_directories(root, error);
    if ( error ) return 1;
    // 用错误码处理测试输出目录失败，不启用异常或隐式终止。
    int        failures = 0;
    const auto check    = [&](bool valid, const char* message) {
        // 汇总检查结果，让一次运行能够区分内存、JSON 和磁盘恢复失败。
        if ( !valid ) {
            ++failures;
            XERROR("Mobile layout: {}", message);
        }
    };
    // 首次启动和旧文件缺字段时必须仍收起辅助窗口，不能继承桌面显隐。
    const auto defaults =
        nlohmann::json::object().get<MobileWorkspaceSettings>();
    check(defaults == MobileWorkspaceSettings{}, "old config defaults");
    // 与代码默认整体比较，未来增加字段时旧配置兼容性仍受覆盖。
    // 构造不对称的现场，防止两个把手共用一个值或隐藏分区比例丢失。
    MobileWorkspaceSettings expected;
    expected.m_toolbar = expected.m_timing = expected.m_preview =
        expected.m_settingsOpen            = true;
    expected.m_leftCenter                  = .2f;
    expected.m_rightCenter                 = .8f;
    expected.m_timingRatio                 = .24f;
    expected.m_previewRatio                = .31f;
    expected.m_selectedTab                 = 0xfedcba98u;
    // 高位身份能够暴露把无符号窗口哈希错误读为有符号整数的实现。
    expected.m_resourceTab = 3;
    // 所有开关同时开启覆盖多个分区的恢复，不以全默认对象冒充往返验证。
    check(nlohmann::json(expected).get<MobileWorkspaceSettings>() == expected,
          "scalar roundtrip");
    // 非法类型不能触发布尔转换；越界坐标夹紧，无限数不得进入几何运算。
    const nlohmann::json invalid = {
        // 直接构造非有限 JSON 数字，确保读取函数自身也防御内存来源配置。
        { "toolbar", "true" },
        { "leftCenter", -5 },
        { "rightCenter", 9 },
        { "timingRatio", std::numeric_limits<double>::infinity() },
        { "selectedTab", -1 },
        { "resourceTab", 99 }
    };
    const auto repaired = invalid.get<MobileWorkspaceSettings>();
    // 同时检查有效边界和缺省字段，不能以整个对象回退掩盖逐字段修复。
    check(!repaired.m_toolbar && repaired.m_leftCenter == 0 &&
              repaired.m_rightCenter == 1 &&
              repaired.m_timingRatio == defaults.m_timingRatio &&
              repaired.m_selectedTab == 0 && repaired.m_resourceTab == 0,
          "malformed fields are isolated");
    // 两个单独有效的宽度相加也必须给主画布保留可用空间。
    const auto crowded =
        // 极端输入只修复比例总量，其他布尔和标签仍应保留代码默认。
        nlohmann::json{ { "timingRatio", .6 }, { "previewRatio", .6 } }
            .get<MobileWorkspaceSettings>();
    check(crowded.m_timingRatio + crowded.m_previewRatio <= .951f,
          "main retains space");
    auto& app = AppConfig::instance();
    // 单例只在本测试隔离进程中使用，reset 不会触及用户持久化文件。
    app.reset();
    const nlohmann::json desktopBefore = app.getVisualConfig();
    // 比较完整桌面视觉 JSON，而非只检查一个宽度字段，覆盖隐藏预设污染。
    const auto file = root / "restart.json";
    app.saveMobileWorkspaceSettings(expected, file);
    // 请求返回时内存值已经更新，磁盘任务只消费不可变快照。
    check(app.getMobileWorkspaceSettings() == expected,
          "memory updates immediately");
    // 等待实际后台完成，不用 sleep 猜测是否已经落盘。
    flushConfigSnapshots();
    // 屏障只在测试中调用；移动外壳提交布局的热路径不能等待写入完成。
    check(nlohmann::json(app.getVisualConfig()) == desktopBefore,
          "mobile save keeps desktop layout");
    app.reset();
    check(app.getMobileWorkspaceSettings() == defaults,
          // 先清空再 load，避免旧内存值导致文件根本没写出时也误报通过。
          "reset clears session values");
    check(app.load(file) && app.getMobileWorkspaceSettings() == expected,
          // 此比较覆盖活动标签和资源分类，不能只恢复窗口显隐。
          "restart restores mobile state");
    // 实际文件必须含独立根键；只在内存保持状态不算完成持久化。
    std::ifstream input(file);
    // 解析失败返回 discarded，由下一条契约检查报告，不抛异常。
    const auto saved = nlohmann::json::parse(input, nullptr, false);
    check(saved.is_object() && saved.contains("mobileWorkspace") &&
              saved["visual"] == desktopBefore,
          "separate client root is persisted");
    // 结束前队列已明确完成，资源释放不依赖固定时长休眠。
    return failures == 0 ? 0 : 1;
}
