#include "logic/ProjectResourceService.h"

#include "log/colorful-log.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace
{

/// @brief 使用小容差比较音轨配置中的单精度数值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 构造旧版顶层 m_volume 音频资源 JSON。
/// @param id 资源 ID。
/// @param type 旧版持久化音轨类型。
/// @param volume 旧版音量。
/// @return 旧版音频资源 JSON。
nlohmann::json makeLegacyAudioResourceJson(const std::string& id,
                                           const std::string& type,
                                           float              volume)
{
    return nlohmann::json{ { "m_id", id },
                           { "m_path", id },
                           { "m_type", type },
                           { "m_volume", volume } };
}

/// @brief 构造覆盖旧版音频字段和旧版缺失设置项的项目 JSON。
/// @return 可复现旧项目打开失败的最小项目 JSON。
nlohmann::json makeLegacyProjectJson()
{
    return nlohmann::json{
        { "m_metadata",
          { { "m_title", "Legacy Project" },
            { "m_artist", "Unknown" },
            { "m_mapper", "Unknown" },
            { "m_version", "1.0.0" } } },
        { "m_settings",
          { { "m_visualOverride", nullptr },
            { "m_editorOverride", nullptr },
            { "m_lastOpenedBeatmap", "" } } },
        { "m_audioResources",
          nlohmann::json::array(
              { makeLegacyAudioResourceJson("audio.mp3", "Main", 0.7F),
                makeLegacyAudioResourceJson("hit.wav", "Main", 0.8F) }) },
        { "m_beatmaps",
          nlohmann::json::array({ { { "m_name", "Legacy.osu" },
                                    { "m_filePath", "Legacy.osu" },
                                    { "m_audioTrackId", "hit.wav" } } }) }
    };
}

/// @brief 验证旧项目可以反序列化、迁移音量并写回当前格式。
/// @return 兼容行为符合预期时返回 true。
bool testLegacyProjectDeserialization()
{
    const auto  legacyJson = makeLegacyProjectJson();
    const auto& audioJson  = legacyJson.at("m_audioResources");
    if ( !MMM::requiresLegacyAudioResourceMigration(audioJson.at(0)) ||
         !MMM::requiresLegacyAudioResourceMigration(audioJson.at(1)) ) {
        XERROR("Legacy audio resource schema was not detected");
        return false;
    }

    const auto project = legacyJson.get<MMM::Project>();
    if ( project.m_audioResources.size() != 2 ) {
        XERROR("Legacy project audio resources were not deserialized");
        return false;
    }

    const auto& mainResource   = project.m_audioResources[0];
    const auto& effectResource = project.m_audioResources[1];
    if ( !near(mainResource.m_config.volume, 0.7F) ||
         !near(effectResource.m_config.volume, 0.8F) ||
         !near(effectResource.m_config.playbackSpeed, 1.0F) ||
         !near(effectResource.m_config.playbackPitch, 0.0F) ||
         effectResource.m_config.muted || effectResource.m_config.eqEnabled ||
         effectResource.m_config.eqPreset != 0 ||
         !effectResource.m_config.eqBandGains.empty() ||
         !effectResource.m_config.eqBandQs.empty() ) {
        XERROR("Legacy audio configuration defaults were not preserved");
        return false;
    }

    const nlohmann::json migratedJson  = project;
    const auto&          migratedAudio = migratedJson.at("m_audioResources");
    for ( const auto& resourceJson : migratedAudio ) {
        if ( resourceJson.contains("m_volume") ||
             MMM::requiresLegacyAudioResourceMigration(resourceJson) ) {
            XERROR("Migrated project still uses the legacy audio schema");
            return false;
        }
    }
    return true;
}

/// @brief 验证旧版全 Main 配置不会覆盖目录扫描出的 Main/Effect 类型。
/// @return 合并行为符合预期时返回 true。
bool testLegacyMergePreservesScannedTypes()
{
    MMM::Project scannedProject;
    scannedProject.m_audioResources = {
        MMM::AudioResource{ .m_id   = "audio.mp3",
                            .m_path = "audio.mp3",
                            .m_type = MMM::AudioTrackType::Main },
        MMM::AudioResource{ .m_id   = "hit.wav",
                            .m_path = "hit.wav",
                            .m_type = MMM::AudioTrackType::Effect },
    };

    const auto legacyProjectJson = makeLegacyProjectJson();
    const auto persistedProject  = legacyProjectJson.get<MMM::Project>();
    const auto legacyKeys =
        MMM::Logic::ProjectResourceService::collectLegacyAudioResourceKeys(
            legacyProjectJson);
    MMM::Logic::ProjectResourceService{}.mergePersistedAudioResources(
        scannedProject, persistedProject, legacyKeys);

    const auto& mainResource   = scannedProject.m_audioResources[0];
    const auto& effectResource = scannedProject.m_audioResources[1];
    if ( mainResource.m_type != MMM::AudioTrackType::Main ||
         effectResource.m_type != MMM::AudioTrackType::Effect ||
         !near(mainResource.m_config.volume, 0.7F) ||
         !near(effectResource.m_config.volume, 0.8F) ) {
        XERROR("Legacy merge overwrote scanned audio resource types");
        return false;
    }

    const nlohmann::json migratedProject = scannedProject;
    const auto& migratedEffect = migratedProject.at("m_audioResources").at(1);
    if ( migratedEffect.at("m_type") != "Effect" ||
         migratedEffect.contains("m_volume") ||
         MMM::requiresLegacyAudioResourceMigration(migratedEffect) ) {
        XERROR("Legacy merge result was not persisted with the current schema");
        return false;
    }
    return true;
}

/// @brief 验证当前 m_config 优先于旧音量且仍可恢复用户保存的音轨类型。
/// @return 当前格式行为保持不变时返回 true。
bool testCurrentConfigAndTypeMerge()
{
    const nlohmann::json persistedJson{
        { "m_id", "hit.wav" },
        { "m_path", "hit.wav" },
        { "m_type", "Main" },
        { "m_config", { { "volume", 0.9F }, { "muted", true } } },
    };
    if ( MMM::requiresLegacyAudioResourceMigration(persistedJson) ) {
        XERROR("Current audio resource schema was treated as legacy");
        return false;
    }
    auto mixedPersistedJson        = persistedJson;
    mixedPersistedJson["m_volume"] = 0.2F;
    if ( !near(mixedPersistedJson.get<MMM::AudioResource>().m_config.volume,
               0.9F) ) {
        XERROR("Current m_config volume did not override legacy m_volume");
        return false;
    }

    MMM::Project scannedProject;
    scannedProject.m_audioResources = {
        MMM::AudioResource{ .m_id   = "hit.wav",
                            .m_path = "hit.wav",
                            .m_type = MMM::AudioTrackType::Effect },
    };
    MMM::Project persistedProject;
    persistedProject.m_audioResources = {
        persistedJson.get<MMM::AudioResource>(),
    };

    MMM::Logic::ProjectResourceService{}.mergePersistedAudioResources(
        scannedProject, persistedProject, {});

    const auto& resource = scannedProject.m_audioResources.front();
    if ( resource.m_type != MMM::AudioTrackType::Main ||
         !near(resource.m_config.volume, 0.9F) || !resource.m_config.muted ||
         !near(resource.m_config.playbackSpeed, 1.0F) ||
         resource.m_config.eqEnabled ) {
        XERROR("Current audio configuration merge behavior changed");
        return false;
    }
    return true;
}

/// @brief 验证同一项目中的旧版和当前资源分别采用各自的类型合并规则。
/// @return 混合格式合并行为符合预期时返回 true。
bool testMixedSchemaMerge()
{
    nlohmann::json projectJson = makeLegacyProjectJson();
    projectJson["m_audioResources"].push_back(
        nlohmann::json{ { "m_id", "manual-main.wav" },
                        { "m_path", "effects/manual-main.wav" },
                        { "m_type", "Main" },
                        { "m_config", { { "volume", 0.6F } } } });

    MMM::Project scannedProject;
    scannedProject.m_audioResources = {
        MMM::AudioResource{ .m_id   = "hit.wav",
                            .m_path = "hit.wav",
                            .m_type = MMM::AudioTrackType::Effect },
        MMM::AudioResource{ .m_id   = "manual-main.wav",
                            .m_path = "effects/manual-main.wav",
                            .m_type = MMM::AudioTrackType::Effect },
    };
    const auto persistedProject = projectJson.get<MMM::Project>();
    const auto legacyKeys =
        MMM::Logic::ProjectResourceService::collectLegacyAudioResourceKeys(
            projectJson);
    MMM::Logic::ProjectResourceService{}.mergePersistedAudioResources(
        scannedProject, persistedProject, legacyKeys);

    const auto& legacyResource  = scannedProject.m_audioResources[0];
    const auto& currentResource = scannedProject.m_audioResources[1];
    if ( legacyResource.m_type != MMM::AudioTrackType::Effect ||
         currentResource.m_type != MMM::AudioTrackType::Main ||
         !near(legacyResource.m_config.volume, 0.8F) ||
         !near(currentResource.m_config.volume, 0.6F) ) {
        XERROR("Mixed project audio schemas were not merged independently");
        return false;
    }
    return true;
}

/// @brief 验证大批量当前格式资源通过路径和 ID 哈希索引合并配置。
///
/// 持久化资源采用反序排列，使输入能够暴露逐扫描资源线性查找退化；
/// 断言不依赖耗时，只验证每项配置和类型完整恢复。
/// @return 全部资源均恢复对应持久化配置时返回 true。
bool testBulkCurrentConfigMerge()
{
    constexpr std::size_t RESOURCE_COUNT = 512U;

    MMM::Project scannedProject;
    MMM::Project persistedProject;
    scannedProject.m_audioResources.reserve(RESOURCE_COUNT);
    persistedProject.m_audioResources.reserve(RESOURCE_COUNT);
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        const auto id   = "bulk-" + std::to_string(index);
        const auto path = "audio/" + id + ".wav";
        scannedProject.m_audioResources.push_back(MMM::AudioResource{
            .m_id   = id,
            .m_path = path,
            .m_type = MMM::AudioTrackType::Effect,
        });

        MMM::AudioTrackConfig config;
        config.volume = static_cast<float>(index + 1U) /
                        static_cast<float>(RESOURCE_COUNT + 1U);
        persistedProject.m_audioResources.push_back(MMM::AudioResource{
            .m_id     = id,
            .m_path   = path,
            .m_type   = MMM::AudioTrackType::Main,
            .m_config = std::move(config),
        });
    }
    std::reverse(persistedProject.m_audioResources.begin(),
                 persistedProject.m_audioResources.end());

    MMM::Logic::ProjectResourceService{}.mergePersistedAudioResources(
        scannedProject, persistedProject, {});
    if ( scannedProject.m_audioResources.size() != RESOURCE_COUNT ) {
        return false;
    }
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        const auto& resource       = scannedProject.m_audioResources[index];
        const auto  expectedVolume = static_cast<float>(index + 1U) /
                                    static_cast<float>(RESOURCE_COUNT + 1U);
        if ( resource.m_type != MMM::AudioTrackType::Main ||
             !near(resource.m_config.volume, expectedVolume) ) {
            XERROR("Bulk audio config merge failed at {}", index);
            return false;
        }
    }
    return true;
}

/// @brief 验证项目工作区分拍线模式与磁吸设置的兼容迁移。
/// @return 旧开关迁移和当前磁吸设置往返均稳定时返回 true。
bool testBeatLineToolbarStateMigration()
{
    const nlohmann::json legacyJson{
        { "m_valid", true },
        { "m_drawBeatLines", false },
        { "m_scrollSnap", true },
    };
    const auto legacyState =
        legacyJson.get<MMM::ProjectWorkspaceToolbarState>();
    if ( legacyState.m_beatLineDisplayMode != "Hidden" ||
         legacyState.m_drawBeatLines || !legacyState.m_objectPlacementSnap ||
         !MMM::Config::isCommonBeatDivisorEnabled(
             legacyState.m_commonBeatDivisorMask, 2) ||
         MMM::Config::isCommonBeatDivisorEnabled(
             legacyState.m_commonBeatDivisorMask, 5) ) {
        XERROR("Legacy toolbar state was not migrated");
        return false;
    }

    MMM::ProjectWorkspaceToolbarState currentState;
    currentState.m_valid                   = true;
    currentState.m_beatLineDisplayMode     = "NearCursor";
    currentState.m_objectPlacementSnap     = true;
    currentState.m_objectPlacementSnapMode = "CommonBeatDivisors";
    currentState.m_commonBeatDivisorMask   = 0U;
    MMM::Config::setCommonBeatDivisorEnabled(
        currentState.m_commonBeatDivisorMask, 3, true);
    MMM::Config::setCommonBeatDivisorEnabled(
        currentState.m_commonBeatDivisorMask, 7, true);
    const nlohmann::json currentJson = currentState;
    const auto           restoredState =
        currentJson.get<MMM::ProjectWorkspaceToolbarState>();
    if ( restoredState.m_beatLineDisplayMode != "NearCursor" ||
         !restoredState.m_drawBeatLines ||
         !currentJson.value("m_drawBeatLines", false) ||
         !restoredState.m_objectPlacementSnap ||
         restoredState.m_objectPlacementSnapMode != "CommonBeatDivisors" ||
         !MMM::Config::isCommonBeatDivisorEnabled(
             restoredState.m_commonBeatDivisorMask, 3) ||
         !MMM::Config::isCommonBeatDivisorEnabled(
             restoredState.m_commonBeatDivisorMask, 7) ||
         MMM::Config::isCommonBeatDivisorEnabled(
             restoredState.m_commonBeatDivisorMask, 4) ) {
        XERROR("Current toolbar state did not survive round trip");
        return false;
    }
    return true;
}

/// @brief 验证项目级自动备份覆盖可往返且旧项目继续继承软件配置。
/// @return 覆盖字段稳定且缺失字段恢复为空值时返回 true。
bool testProjectAutoBackupOverrideCompatibility()
{
    MMM::ProjectSettings settings;
    settings.m_autoBackupOverride.emplace();
    settings.m_autoBackupOverride->mode =
        MMM::Config::AutoSaveMode::EventTriggered;
    settings.m_autoBackupOverride->onBeatmapSwitch = false;
    settings.m_autoBackupOverride->maxBackupCount  = 17;

    const nlohmann::json encoded  = settings;
    const auto           restored = encoded.get<MMM::ProjectSettings>();
    const auto legacy = nlohmann::json::object().get<MMM::ProjectSettings>();
    if ( !encoded.contains("m_autoBackupOverride") ||
         !restored.m_autoBackupOverride ||
         restored.m_autoBackupOverride->mode !=
             MMM::Config::AutoSaveMode::EventTriggered ||
         restored.m_autoBackupOverride->onBeatmapSwitch ||
         restored.m_autoBackupOverride->maxBackupCount != 17 ||
         legacy.m_autoBackupOverride ) {
        XERROR("Project auto-backup override compatibility failed");
        return false;
    }
    return true;
}

/// @brief 验证项目编辑器覆盖不再持久化工具栏显示配置。
/// @return 新项目未写出相关字段且旧项目字段读取后回落到软件默认值时返回 true。
bool testProjectToolbarDisplaySettingsAreGlobal()
{
    MMM::ProjectSettings settings;
    settings.m_editorOverride.emplace();
    auto& projectEditor             = *settings.m_editorOverride;
    projectEditor.showToolLabels    = true;
    projectEditor.fixedToolWindow   = false;
    projectEditor.showManagerLabels = false;
    projectEditor.toolbarVisibility.stateTools.colorBrush          = true;
    projectEditor.toolbarVisibility.independentButtons.notePalette = true;

    const nlohmann::json encoded    = settings;
    const auto&          editorJson = encoded.at("m_editorOverride");
    if ( editorJson.contains("showToolLabels") ||
         editorJson.contains("fixedToolWindow") ||
         editorJson.contains("showManagerLabels") ||
         editorJson.contains("toolbarVisibility") ) {
        XERROR("Project settings persisted global toolbar display fields");
        return false;
    }

    nlohmann::json legacyEditor       = projectEditor;
    nlohmann::json legacyProject      = encoded;
    legacyProject["m_editorOverride"] = std::move(legacyEditor);
    const auto restored = legacyProject.get<MMM::ProjectSettings>();
    if ( !restored.m_editorOverride ) {
        XERROR("Project editor override was not restored");
        return false;
    }

    const auto& restoredEditor = *restored.m_editorOverride;
    if ( restoredEditor.showToolLabels || !restoredEditor.fixedToolWindow ||
         !restoredEditor.showManagerLabels ||
         restoredEditor.toolbarVisibility.stateTools.colorBrush ||
         restoredEditor.toolbarVisibility.independentButtons.notePalette ) {
        XERROR("Legacy project toolbar display fields remained project-scoped");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行旧版项目音频配置兼容测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testLegacyProjectDeserialization() &&
                   testLegacyMergePreservesScannedTypes() &&
                   testCurrentConfigAndTypeMerge() && testMixedSchemaMerge() &&
                   testBulkCurrentConfigMerge() &&
                   testBeatLineToolbarStateMigration() &&
                   testProjectAutoBackupOverrideCompatibility() &&
                   testProjectToolbarDisplaySettingsAreGlobal()
               ? 0
               : 1;
}
