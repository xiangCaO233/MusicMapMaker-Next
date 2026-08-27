#include "config/EditorSettings.h"
#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

namespace
{

/// @brief 验证定时自动保存的模式、单位和边界值可稳定持久化。
/// @return 往返值一致且非法间隔被限制到 5~60 时返回 true。
bool testTimedAutoSaveRoundTrip()
{
    MMM::Config::EditorSettings source;
    source.autoSave.mode          = MMM::Config::AutoSaveMode::Timed;
    source.autoSave.intervalUnit  = MMM::Config::AutoSaveIntervalUnit::Minutes;
    source.autoSave.intervalValue = 45;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           tooShort = nlohmann::json{
                  { "autoSave", { { "mode", "Timed" }, { "intervalValue", 1 } } }
    }.get<MMM::Config::EditorSettings>();
    const auto tooLong = nlohmann::json{
        { "autoSave", { { "mode", "Timed" }, { "intervalValue", 120 } } }
    }.get<MMM::Config::EditorSettings>();

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
    return true;
}

/// @brief 验证事件自动保存的独立触发开关和旧配置默认值。
/// @return 事件开关往返无损且旧配置默认关闭自动保存时返回 true。
bool testEventAutoSaveRoundTrip()
{
    MMM::Config::EditorSettings source;
    source.autoSave.mode = MMM::Config::AutoSaveMode::EventTriggered;
    source.autoSave.onObjectModified        = false;
    source.autoSave.onBeatmapSwitch         = true;
    source.autoSave.onImGuiWindowFocusLost  = false;
    source.autoSave.onNativeWindowFocusLost = true;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    const auto invalid = nlohmann::json{
        { "autoSave", { { "mode", "Unknown" } } }
    }.get<MMM::Config::EditorSettings>();

    if ( restored.autoSave.mode != MMM::Config::AutoSaveMode::EventTriggered ||
         restored.autoSave.onObjectModified ||
         !restored.autoSave.onBeatmapSwitch ||
         restored.autoSave.onImGuiWindowFocusLost ||
         !restored.autoSave.onNativeWindowFocusLost ||
         legacy.autoSave.mode != MMM::Config::AutoSaveMode::Disabled ||
         invalid.autoSave.mode != MMM::Config::AutoSaveMode::Disabled ) {
        XERROR("Event auto-save config did not preserve compatibility");
        return false;
    }
    return true;
}

/// @brief 验证自动备份配置的定时、事件与保留数量可稳定持久化。
/// @return 往返值一致、旧配置默认关闭且非法数量被限制时返回 true。
bool testAutoBackupRoundTrip()
{
    MMM::Config::EditorSettings source;
    source.autoBackup.mode         = MMM::Config::AutoSaveMode::EventTriggered;
    source.autoBackup.intervalUnit = MMM::Config::AutoSaveIntervalUnit::Seconds;
    source.autoBackup.intervalValue          = 15;
    source.autoBackup.onObjectModified       = false;
    source.autoBackup.onBeatmapSwitch        = true;
    source.autoBackup.onImGuiWindowFocusLost = false;
    source.autoBackup.maxBackupCount         = 24;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    const auto tooFew = nlohmann::json{
        { "autoBackup", { { "maxBackupCount", 0 } } }
    }.get<MMM::Config::EditorSettings>();
    const auto tooMany = nlohmann::json{
        { "autoBackup", { { "maxBackupCount", 1000 } } }
    }.get<MMM::Config::EditorSettings>();

    if ( restored.autoBackup.mode !=
             MMM::Config::AutoSaveMode::EventTriggered ||
         restored.autoBackup.intervalUnit !=
             MMM::Config::AutoSaveIntervalUnit::Seconds ||
         restored.autoBackup.intervalValue != 15 ||
         restored.autoBackup.onObjectModified ||
         !restored.autoBackup.onBeatmapSwitch ||
         restored.autoBackup.onImGuiWindowFocusLost ||
         restored.autoBackup.maxBackupCount != 24 ||
         legacy.autoBackup.mode != MMM::Config::AutoSaveMode::Disabled ||
         tooFew.autoBackup.maxBackupCount !=
             MMM::Config::AUTO_BACKUP_COUNT_MIN ||
         tooMany.autoBackup.maxBackupCount !=
             MMM::Config::AUTO_BACKUP_COUNT_MAX ) {
        XERROR("Auto-backup config did not preserve safe values");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行软件全局自动保存与自动备份配置的持久化兼容测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testTimedAutoSaveRoundTrip() && testEventAutoSaveRoundTrip() &&
                   testAutoBackupRoundTrip()
               ? 0
               : 1;
}
