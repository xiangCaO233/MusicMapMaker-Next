#include "config/EditorSettings.h"
#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

namespace
{

/// @brief 验证定时自动保存的模式、单位和边界值可稳定持久化。
/// @return 往返值一致且非法间隔被限制到 5~60 时返回 true。
/// @details 正常值使用分钟单位，越界输入同时验证默认单位恢复。
bool testTimedAutoSaveRoundTrip()
{
    // 使用分钟制和非边界值，验证模式、单位与数值三个字段都参与往返。
    MMM::Config::EditorSettings source;
    source.autoSave.mode = MMM::Config::AutoSaveMode::Timed;
    // Minutes 必须在 JSON 中稳定保存，不能在序列化时提前换算为秒。
    source.autoSave.intervalUnit  = MMM::Config::AutoSaveIntervalUnit::Minutes;
    source.autoSave.intervalValue = 45;
    // 45 位于合法区间，正常往返不得被边界钳制逻辑改写。

    // 当前配置直接序列化后恢复，作为正常路径基准。
    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    // 过小和过大输入分别覆盖解析器的下限与上限收敛。
    const auto tooShort = nlohmann::json{
        // 下界输入保留 Timed 模式，隔离验证 intervalValue 钳制。
        { "autoSave", { { "mode", "Timed" }, { "intervalValue", 1 } } }
    }.get<MMM::Config::EditorSettings>();
    const auto tooLong = nlohmann::json{
        // 上界输入与下界采用相同结构，只有数值方向不同。
        { "autoSave", { { "mode", "Timed" }, { "intervalValue", 120 } } }
    }.get<MMM::Config::EditorSettings>();

    // intervalSeconds 同时验证单位换算，45 分钟应得到 2700 秒。
    if ( restored.autoSave.mode != MMM::Config::AutoSaveMode::Timed ||
         restored.autoSave.intervalUnit !=
             MMM::Config::AutoSaveIntervalUnit::Minutes ||
         restored.autoSave.intervalValue != 45 ||
         restored.autoSave.intervalSeconds() != 2700.0 ||
         tooShort.autoSave.intervalValue != 5 ||
         tooLong.autoSave.intervalValue != 60 ) {
        XERROR("Timed auto-save config did not preserve safe bounds");
        return false;
    }
    // 正常值和两个边界输入全部符合约定才确认定时配置通过。
    return true;
}

/// @brief 验证事件自动保存的独立触发开关和旧配置默认值。
/// @return 事件开关往返无损且旧配置默认关闭自动保存时返回 true。
/// @details 四个事件开关使用交错值，另覆盖缺失和未知模式输入。
bool testEventAutoSaveRoundTrip()
{
    // 交错设置四个事件开关，避免全 true 或全 false 掩盖字段错位。
    MMM::Config::EditorSettings source;
    source.autoSave.mode = MMM::Config::AutoSaveMode::EventTriggered;
    // 物件修改关闭而谱面切换开启，验证相邻布尔字段不会串位。
    source.autoSave.onObjectModified       = false;
    source.autoSave.onBeatmapSwitch        = true;
    source.autoSave.onImGuiWindowFocusLost = false;
    // 原生窗口与 ImGui 窗口失焦属于两个独立触发来源。
    source.autoSave.onNativeWindowFocusLost = true;

    // legacy 空对象验证新增 autoSave 分组缺失时的兼容默认值。
    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 未知模式文本必须回退 Disabled，不能误启用后台保存。
    // 未知枚举文本不能被当作 EventTriggered 或 Timed 接受。
    const auto invalid = nlohmann::json{
        { "autoSave", { { "mode", "Unknown" } } }
    }.get<MMM::Config::EditorSettings>();

    // 每个开关单独比较，确保序列化键没有在两个焦点事件之间串位。
    if ( restored.autoSave.mode != MMM::Config::AutoSaveMode::EventTriggered ||
         // false 开关必须保持 false，不能因字段存在而解释为 true。
         restored.autoSave.onObjectModified ||
         !restored.autoSave.onBeatmapSwitch ||
         restored.autoSave.onImGuiWindowFocusLost ||
         !restored.autoSave.onNativeWindowFocusLost ||
         legacy.autoSave.mode != MMM::Config::AutoSaveMode::Disabled ||
         invalid.autoSave.mode != MMM::Config::AutoSaveMode::Disabled ) {
        // 任一开关、默认值或枚举回退异常都属于事件配置兼容失败。
        XERROR("Event auto-save config did not preserve compatibility");
        return false;
    }
    // 旧配置与非法模式均保持安全关闭后才算兼容性成立。
    return true;
}

/// @brief 验证自动备份配置的定时、事件与保留数量可稳定持久化。
/// @return 往返值一致、旧配置默认关闭且非法数量被限制时返回 true。
/// @details 事件模式仍保存秒制间隔，确保切换模式不会丢失备用参数。
bool testAutoBackupRoundTrip()
{
    // 自动备份复用保存模式和间隔类型，但拥有独立事件开关与保留数量。
    MMM::Config::EditorSettings source;
    source.autoBackup.mode = MMM::Config::AutoSaveMode::EventTriggered;
    // Seconds 与自动保存用例的 Minutes 形成另一单位覆盖。
    source.autoBackup.intervalUnit = MMM::Config::AutoSaveIntervalUnit::Seconds;
    source.autoBackup.intervalValue = 15;
    // 合法 15 秒用于证明备份间隔不会套用分钟制换算。
    source.autoBackup.onObjectModified = false;
    // 谱面切换开启而 ImGui 失焦关闭，验证备份事件字段独立序列化。
    source.autoBackup.onBeatmapSwitch        = true;
    source.autoBackup.onImGuiWindowFocusLost = false;
    source.autoBackup.maxBackupCount         = 24;

    // 当前值往返与旧配置默认值在同一用例中比较。
    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 非法保留数量分别验证公开常量定义的最小和最大边界。
    // 零低于公开最小备份数，恢复后必须提升到安全下界。
    const auto tooFew = nlohmann::json{
        { "autoBackup", { { "maxBackupCount", 0 } } }
    }.get<MMM::Config::EditorSettings>();
    // 极大数量必须限制，避免配置导致无界备份文件增长。
    const auto tooMany = nlohmann::json{
        { "autoBackup", { { "maxBackupCount", 1000 } } }
    }.get<MMM::Config::EditorSettings>();

    // 秒制间隔保持原值，事件开关和数量也必须独立往返。
    if ( restored.autoBackup.mode !=
             MMM::Config::AutoSaveMode::EventTriggered ||
         restored.autoBackup.intervalUnit !=
             MMM::Config::AutoSaveIntervalUnit::Seconds ||
         restored.autoBackup.intervalValue != 15 ||
         restored.autoBackup.onObjectModified ||
         !restored.autoBackup.onBeatmapSwitch ||
         restored.autoBackup.onImGuiWindowFocusLost ||
         restored.autoBackup.maxBackupCount != 24 ||
         // 缺失分组安全关闭自动备份，不能继承 source 的事件模式。
         legacy.autoBackup.mode != MMM::Config::AutoSaveMode::Disabled ||
         tooFew.autoBackup.maxBackupCount !=
             MMM::Config::AUTO_BACKUP_COUNT_MIN ||
         tooMany.autoBackup.maxBackupCount !=
             MMM::Config::AUTO_BACKUP_COUNT_MAX ) {
        // 任一字段或兼容边界失败都由同一用例日志归类为自动备份问题。
        XERROR("Auto-backup config did not preserve safe values");
        return false;
    }
    // 所有正常、缺失和越界输入通过后返回成功。
    return true;
}

}  // namespace

/// @brief 运行软件全局自动保存与自动备份兼容测试。
/// @return 全部测试通过时返回 0。
int main()
{
    // 从定时保存到事件保存再到备份，按配置结构由简单到复杂执行。
    // 每个子测试自行记录失败类别，main 只汇总返回码。
    return testTimedAutoSaveRoundTrip() && testEventAutoSaveRoundTrip() &&
                   // 自动备份最后执行，确认字段不会误写入 autoSave 分组。
                   testAutoBackupRoundTrip()
               ? 0
               : 1;
}
