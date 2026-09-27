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

// 兼容场景直接构造 JSON
// 与扫描结果，在内存中验证迁移和合并，不访问实际项目目录。
// 文件名只充当资源身份；这些用例不证明对应音频可以解码，也不执行项目磁盘保存。

/// @brief 使用小容差比较音轨配置中的单精度数值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
/// @note 比较配置值而非解码音频幅度，容差仅吸收单精度表示差异。
bool near(float lhs, float rhs)
{
    // 容差远小于夹具中不同音量之间的差值，不允许资源错配被近似比较掩盖。
    // 非有限差值不会通过严格容差比较，不能把损坏音量视为成功恢复。
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 构造旧版顶层 m_volume 音频资源 JSON。
/// @param id 资源 ID。
/// @param type 旧版持久化音轨类型。
/// @param volume 旧版音量。
/// @return 旧版音频资源 JSON。
/// @pre type 使用可反序列化的枚举字符串，volume 为有效配置值。
/// @note 故意不写 m_config，使迁移入口能够根据原始字段形状识别旧资源。
nlohmann::json makeLegacyAudioResourceJson(const std::string& id,
                                           const std::string& type,
                                           float              volume)
{
    // 旧格式音量位于资源顶层，不能用当前 AudioResource 序列化生成这一夹具，
    // 否则测试尚未开始就已经把待测旧字段转换成新格式。
    return nlohmann::json{ { "m_id", id },
                           { "m_path", id },
                           { "m_type", type },
                           { "m_volume", volume } };
}

/// @brief 构造覆盖旧版音频字段和旧版缺失设置项的项目 JSON。
/// @return 可复现旧项目打开失败的最小项目 JSON。
/// @note 每次返回新对象，混合格式测试可追加资源而不污染其他旧格式场景。
/// @note 两个旧资源都标成 Main，为后续扫描类型优先级测试提供有意冲突。
/// @note 设置项只保留旧字段，不预填当前版本新增的默认配置。
nlohmann::json makeLegacyProjectJson()
{
    // 展示元数据只保证项目形状完整，断言不依赖标题、作者或版本字符串的具体内容。
    // m_lastOpenedBeatmap 留空，避免给配置迁移用例引入上次打开谱面的恢复要求。
    // 覆盖项显式为 null，验证继承配置的旧状态不会被要求具有完整子对象。
    // 谱面引用 hit.wav，使音效资源同时也是旧项目中的实际引用目标。
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
/// @note 写回指重新序列化为 JSON，不创建项目文件或验证文件系统原子保存。
bool testLegacyProjectDeserialization()
{
    // 成功读取缺失设置的旧项目不代表逐项核对所有新增设置，只核对下方音频默认值。
    // at() 读取的是测试自建且形状确定的对象，不把非法 JSON 容错混入此场景。
    const auto  legacyJson = makeLegacyProjectJson();
    const auto& audioJson  = legacyJson.at("m_audioResources");
    // 先确认夹具确实被识别为旧格式，避免新版夹具误入默认路径仍通过后续断言。
    if ( !MMM::requiresLegacyAudioResourceMigration(audioJson.at(0)) ||
         !MMM::requiresLegacyAudioResourceMigration(audioJson.at(1)) ) {
        XERROR("Legacy audio resource schema was not detected");
        return false;
    }

    const auto project = legacyJson.get<MMM::Project>();
    // legacyJson 保持只读，用于区分原始旧输入与随后生成的当前格式输出。
    // 从完整项目入口反序列化，而非只测试单个资源，可同时覆盖缺失项目设置的兼容。
    if ( project.m_audioResources.size() != 2 ) {
        // 后面按固定夹具索引读取，先检查数量也能防止断言自身读取缺失资源。
        XERROR("Legacy project audio resources were not deserialized");
        return false;
    }

    const auto& mainResource   = project.m_audioResources[0];
    const auto& effectResource = project.m_audioResources[1];
    // 两种不同旧音量都要保留，不能以统一默认音量替代迁移。
    // 新增的速度、音高、静音和均衡器字段则应从各自默认值补齐。
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

    // 此处 effectResource 只是按用途命名，旧模型仍将它标成 Main。
    // 本用例验证配置迁移，类型修复需要随后与目录扫描结果合并才会发生。
    const nlohmann::json migratedJson = project;
    // 再经过当前序列化入口后应脱离旧 schema，避免每次打开都重复执行迁移。
    const auto& migratedAudio = migratedJson.at("m_audioResources");
    for ( const auto& resourceJson : migratedAudio ) {
        // 对每个资源检查 schema，不能只验证项目级 JSON 出现过一次新字段。
        // 顶层 m_volume 必须消失；仅有新字段而仍保留旧字段不算完成格式升级。
        if ( resourceJson.contains("m_volume") ||
             MMM::requiresLegacyAudioResourceMigration(resourceJson) ) {
            // 这两个条件分别排除旧字段残留和迁移判定仍为真，不相互替代。
            XERROR("Migrated project still uses the legacy audio schema");
            return false;
        }
    }
    return true;
}

/// @brief 验证旧版全 Main 配置不会覆盖目录扫描出的 Main/Effect 类型。
/// @return 合并行为符合预期时返回 true。
/// @note 扫描结果是人工构造，不在此验证文件扩展名或目录规则的分类算法。
bool testLegacyMergePreservesScannedTypes()
{
    // 音轨类型比较使用枚举，写回检查再使用 JSON
    // 字符串，覆盖模型与存储两层表示。
    // 两边资源顺序相同，本场景只隔离旧类型覆盖规则，乱序匹配另有批量用例。
    // 扫描结果将 hit.wav 分类为 Effect，与旧持久化数据中的 Main 明确冲突。
    // audio.mp3 仍是 Main，防止实现简单地把所有旧资源统一改成 Effect。
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
    // 同一个旧 JSON 分别提供已解码配置和迁移键，两者不可来自不同版本的项目。
    const auto persistedProject = legacyProjectJson.get<MMM::Project>();
    const auto legacyKeys =
        MMM::Logic::ProjectResourceService::collectLegacyAudioResourceKeys(
            legacyProjectJson);
    // 旧格式身份从原始 JSON 收集；反序列化后的模型已含默认 m_config，
    // 不能仅看模型字段反推某个资源在磁盘上究竟使用哪种 schema。
    MMM::Logic::ProjectResourceService{}.mergePersistedAudioResources(
        scannedProject, persistedProject, legacyKeys);

    const auto& mainResource   = scannedProject.m_audioResources[0];
    const auto& effectResource = scannedProject.m_audioResources[1];
    // 兼容合并应保留扫描类型但恢复旧音量，两类字段并不是全盘接受或全盘忽略。
    if ( mainResource.m_type != MMM::AudioTrackType::Main ||
         effectResource.m_type != MMM::AudioTrackType::Effect ||
         !near(mainResource.m_config.volume, 0.7F) ||
         !near(effectResource.m_config.volume, 0.8F) ) {
        XERROR("Legacy merge overwrote scanned audio resource types");
        return false;
    }

    const nlohmann::json migratedProject = scannedProject;
    // 需序列化可变扫描侧的合并结果，不检查只读的持久化输入对象。
    const auto& migratedEffect = migratedProject.at("m_audioResources").at(1);
    // 检查序列化结果中的类型字符串，确保正确内存类型不会在写回时退回旧 Main。
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
/// @note 同时覆盖新旧音量字段冲突和当前格式手动类型覆盖，不能沿用旧格式策略。
bool testCurrentConfigAndTypeMerge()
{
    // 当前 m_config 只写部分字段，解码需同时恢复显式值和其余字段的默认值。
    // 混合字段只用于单资源解码，随后的合并输入仍使用干净的当前格式对象。
    const nlohmann::json persistedJson{
        { "m_id", "hit.wav" },
        { "m_path", "hit.wav" },
        { "m_type", "Main" },
        { "m_config", { { "volume", 0.9F }, { "muted", true } } },
    };
    if ( MMM::requiresLegacyAudioResourceMigration(persistedJson) ) {
        // 若当前字段形状被误判，后续类型合并的分支前提已不成立，立即报告失败。
        XERROR("Current audio resource schema was treated as legacy");
        return false;
    }
    // 在已有 m_config 的对象上混入不同旧音量，用明确冲突验证新字段优先级。
    auto mixedPersistedJson        = persistedJson;
    mixedPersistedJson["m_volume"] = 0.2F;
    // 只增加冲突旧音量，不改变 ID、路径或类型，结果差异只能来自字段优先级。
    if ( !near(mixedPersistedJson.get<MMM::AudioResource>().m_config.volume,
               0.9F) ) {
        XERROR("Current m_config volume did not override legacy m_volume");
        return false;
    }

    // 扫描推断为 Effect，但当前格式明确保存
    // Main，合并时应尊重用户已保存的选择。
    MMM::Project scannedProject;
    scannedProject.m_audioResources = {
        MMM::AudioResource{ .m_id   = "hit.wav",
                            .m_path = "hit.wav",
                            .m_type = MMM::AudioTrackType::Effect },
    };
    MMM::Project persistedProject;
    // 输入和输出使用两个独立项目，不能原地自合并后检查未改变的原始值。
    persistedProject.m_audioResources = {
        persistedJson.get<MMM::AudioResource>(),
    };

    MMM::Logic::ProjectResourceService{}.mergePersistedAudioResources(
        scannedProject, persistedProject, {});

    // 空旧键集合明确声明这是当前格式路径，不能把所有 Main 都当作旧版误分类。
    const auto& resource = scannedProject.m_audioResources.front();
    // muted=true 与非默认音量一起恢复，防止只迁移音量而遗漏其他音轨配置成员。
    // 未显式写入的速度与均衡器字段仍需保留默认值，不能只恢复两个已存在字段。
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
/// @note 同一项目中按资源分别识别 schema，项目级“全部旧版”布尔量不足以表达。
bool testMixedSchemaMerge()
{
    // 当前资源特意放在 effects 子目录却声明
    // Main，持久化选择不能被扫描分类覆盖。 同一调用同时处理两种
    // schema，不能分两次合并掩盖项目级统一分类的错误。 旧夹具还含扫描侧未列出的
    // audio.mp3，本断言聚焦两个指定的匹配结果。
    nlohmann::json projectJson = makeLegacyProjectJson();
    projectJson["m_audioResources"].push_back(
        nlohmann::json{ { "m_id", "manual-main.wav" },
                        { "m_path", "effects/manual-main.wav" },
                        { "m_type", "Main" },
                        { "m_config", { { "volume", 0.6F } } } });
    // 当前资源的 ID 与路径不同，模拟实际子目录资源；旧 hit.wav 仍保持原始字段。

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
    // 旧键仍从原始混合 JSON 收集，不能把追加的当前资源也标成旧格式。
    const auto legacyKeys =
        MMM::Logic::ProjectResourceService::collectLegacyAudioResourceKeys(
            projectJson);
    MMM::Logic::ProjectResourceService{}.mergePersistedAudioResources(
        scannedProject, persistedProject, legacyKeys);

    const auto& legacyResource  = scannedProject.m_audioResources[0];
    const auto& currentResource = scannedProject.m_audioResources[1];
    // 两个扫描资源都为 Effect，但只有当前格式项能由持久化 Main 覆盖。
    // 不同音量还用于确认配置来自各自资源，而非错误套用第一项。
    if ( legacyResource.m_type != MMM::AudioTrackType::Effect ||
         currentResource.m_type != MMM::AudioTrackType::Main ||
         !near(legacyResource.m_config.volume, 0.8F) ||
         !near(currentResource.m_config.volume, 0.6F) ) {
        XERROR("Mixed project audio schemas were not merged independently");
        return false;
    }
    return true;
}

/// @brief 验证反序持久化资源仍能按资源身份正确合并配置。
///
/// 持久化资源采用反序排列，避免按数组位置配对也能通过测试。
/// 断言不依赖耗时，只验证每项配置和类型完整恢复，不证明查找复杂度。
/// @return 全部资源均恢复对应持久化配置时返回 true。
bool testBulkCurrentConfigMerge()
{
    // 数量固定且不测量用时，本测试不会因机器性能变化调整通过阈值。
    // ID 与路径同时唯一，本用例不能区分服务内部究竟优先采用哪个匹配键。
    // 断言保持扫描侧顺序，不要求持久化资源的反序排列成为输出顺序。
    constexpr std::size_t RESOURCE_COUNT = 512U;
    // 固定批量规模用于覆盖大量身份配对，不作为性能阈值或压力测试负载。

    MMM::Project scannedProject;
    MMM::Project persistedProject;
    scannedProject.m_audioResources.reserve(RESOURCE_COUNT);
    persistedProject.m_audioResources.reserve(RESOURCE_COUNT);
    // 两边预留容量只减少夹具构造开销，不改变被测合并服务采用的查找方式。
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        const auto id = "bulk-" + std::to_string(index);
        // 稳定十进制身份便于从失败索引复现场景，不依赖随机数或本机目录。
        const auto path = "audio/" + id + ".wav";
        // ID 与相对路径都唯一，避免重名歧义干扰本场景的乱序匹配目标。
        scannedProject.m_audioResources.push_back(MMM::AudioResource{
            .m_id   = id,
            .m_path = path,
            .m_type = MMM::AudioTrackType::Effect,
        });

        MMM::AudioTrackConfig config;
        // 每轮创建新配置，避免上一个资源的非默认字段无意泄漏到下一项。
        config.volume = static_cast<float>(index + 1U) /
                        static_cast<float>(RESOURCE_COUNT + 1U);
        // 每项音量唯一且落在 (0,1)，错配到任何邻项都能被逐项断言识别。
        persistedProject.m_audioResources.push_back(MMM::AudioResource{
            .m_id     = id,
            .m_path   = path,
            .m_type   = MMM::AudioTrackType::Main,
            .m_config = std::move(config),
        });
    }
    std::reverse(persistedProject.m_audioResources.begin(),
                 persistedProject.m_audioResources.end());
    // 资源本身按值存储，反转只打乱顺序，不改变 ID、路径与配置之间的归属关系。
    // 只反转持久化一侧；若两边一起反转，错误的按数组下标合并仍会通过。

    MMM::Logic::ProjectResourceService{}.mergePersistedAudioResources(
        scannedProject, persistedProject, {});
    if ( scannedProject.m_audioResources.size() != RESOURCE_COUNT ) {
        // 合并不应丢项或重复追加；先检查数量再按扫描顺序读取每个结果。
        return false;
    }
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        const auto& resource = scannedProject.m_audioResources[index];
        // 期望音量按原扫描索引重算，不从已反转的持久化数组按位置读取。
        const auto expectedVolume = static_cast<float>(index + 1U) /
                                    static_cast<float>(RESOURCE_COUNT + 1U);
        if ( resource.m_type != MMM::AudioTrackType::Main ||
             !near(resource.m_config.volume, expectedVolume) ) {
            XERROR("Bulk audio config merge failed at {}", index);
            // 逐项检查而不是比较音量总和；两个资源配置互换不能保持测试通过。
            return false;
        }
    }
    return true;
}

/// @brief 验证项目工作区分拍线模式与磁吸设置的兼容迁移。
/// @return 旧开关迁移和当前磁吸设置往返均稳定时返回 true。
/// @note 使用工作区值对象序列化，不启动真实工具栏或模拟鼠标磁吸。
bool testBeatLineToolbarStateMigration()
{
    // 旧布尔只表达显示与隐藏，当前 NearCursor 场景用于验证更细的新模式不退化。
    // m_scrollSnap 属于旧夹具已有字段，新版物件吸附开关由缺省兼容逻辑补齐。
    // 这里不验证真实滚轮行为，只检查存储形式到工作区值对象的转换。
    const nlohmann::json legacyJson{
        { "m_valid", true },
        { "m_drawBeatLines", false },
        { "m_scrollSnap", true },
    };
    const auto legacyState =
        legacyJson.get<MMM::ProjectWorkspaceToolbarState>();
    // 旧布尔关闭值应映射成 Hidden，新版磁吸字段缺失时则继承兼容默认。
    // 用分母 2 与 5 分别检验默认启用和默认未启用，不只验证掩码非零。
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
    // 当前格式重新创建状态，不在 legacyState 上追加配置，避免迁移残留相互影响。
    currentState.m_valid                   = true;
    currentState.m_beatLineDisplayMode     = "NearCursor";
    currentState.m_objectPlacementSnap     = true;
    currentState.m_objectPlacementSnapMode = "CommonBeatDivisors";
    currentState.m_commonBeatDivisorMask   = 0U;
    // 三分拍与七分拍都是明确选择，不能用恢复默认掩码替代按位保存用户选择。
    // 先清默认掩码，只启用 3 和 7，防止默认分拍位残留掩盖往返丢字段的问题。
    MMM::Config::setCommonBeatDivisorEnabled(
        currentState.m_commonBeatDivisorMask, 3, true);
    MMM::Config::setCommonBeatDivisorEnabled(
        currentState.m_commonBeatDivisorMask, 7, true);
    // 通过统一位操作入口设置掩码，测试重点是存储往返，不重复实现分母到位号换算。
    const nlohmann::json currentJson = currentState;
    const auto           restoredState =
        currentJson.get<MMM::ProjectWorkspaceToolbarState>();
    // NearCursor 既保留新模式，也需生成兼容旧读取端的 drawBeatLines=true。
    // 掩码检查包含未启用的 4，避免实现错误地恢复成“全部启用”仍通过。
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
        // 模式名、兼容开关与掩码三部分缺一不可，任一项失真即判整个往返失败。
        return false;
    }
    return true;
}

/// @brief 验证项目级自动备份覆盖可往返且旧项目继续继承软件配置。
/// @return 覆盖字段稳定且缺失字段恢复为空值时返回 true。
/// @note 空 optional 表示继承软件设置，不等同于创建一个采用默认值的项目覆盖。
bool testProjectAutoBackupOverrideCompatibility()
{
    // 旧输入验证字段缺失，不覆盖显式 null 或损坏子对象的其他反序列化分支。
    // 此处只验证继承表达，不启动自动备份定时器，也不实际生成备份文件。
    MMM::ProjectSettings settings;
    settings.m_autoBackupOverride.emplace();
    // 显式创建项目覆盖，使用事件触发、关闭切谱触发及非默认保留数验证字段往返。
    settings.m_autoBackupOverride->mode =
        MMM::Config::AutoSaveMode::EventTriggered;
    settings.m_autoBackupOverride->onBeatmapSwitch = false;
    settings.m_autoBackupOverride->maxBackupCount  = 17;
    // 使用有效但非默认的保留数量，不将非法值纠正策略混入序列化测试。

    const nlohmann::json encoded  = settings;
    const auto           restored = encoded.get<MMM::ProjectSettings>();
    const auto legacy = nlohmann::json::object().get<MMM::ProjectSettings>();
    // 空 optional 表示没有项目选择，与持有一份默认备份配置的 optional 不同。
    // 两者会决定后续是否继承软件配置，因此不能仅比较内部字段默认值。
    // 空对象模拟旧项目没有备份覆盖字段，不能用当前 settings 的序列化结果替代。
    if ( !encoded.contains("m_autoBackupOverride") ||
         !restored.m_autoBackupOverride ||
         restored.m_autoBackupOverride->mode !=
             MMM::Config::AutoSaveMode::EventTriggered ||
         restored.m_autoBackupOverride->onBeatmapSwitch ||
         restored.m_autoBackupOverride->maxBackupCount != 17 ||
         legacy.m_autoBackupOverride ) {
        // optional 存在性条件放在解引用之前，失败的恢复结果不能让断言自身失效。
        // 序列化字段存在、当前覆盖恢复、旧项目仍继承三个条件必须同时成立。
        XERROR("Project auto-backup override compatibility failed");
        return false;
    }
    return true;
}

/// @brief 验证项目编辑器覆盖不再持久化工具栏显示配置。
/// @return 新项目未写出相关字段且旧项目字段读取后回落到软件默认值时返回 true。
/// @note 验证项目序列化边界，不修改个人设置文件，也不检查实际工具栏外观。
bool testProjectToolbarDisplaySettingsAreGlobal()
{
    // 普通 EditorSettings 序列化与 ProjectSettings
    // 嵌套序列化职责不同，夹具故意使用两者。
    // 回落目标是类型默认值，不读取当前用户自定义的全局显示偏好。
    // 测试保持项目覆盖对象存在，只要求软件级显示字段不能继续在项目内保存。
    MMM::ProjectSettings settings;
    settings.m_editorOverride.emplace();
    // 保留项目编辑器覆盖对象本身，只排除应归软件级管理的显示字段。
    auto& projectEditor             = *settings.m_editorOverride;
    projectEditor.showToolLabels    = true;
    projectEditor.fixedToolWindow   = false;
    projectEditor.showManagerLabels = false;
    projectEditor.toolbarVisibility.stateTools.colorBrush          = true;
    projectEditor.toolbarVisibility.independentButtons.notePalette = true;
    // 设置与默认不同的值，确保旧字段被忽略后的默认回退确实可观察。

    const nlohmann::json encoded    = settings;
    const auto&          editorJson = encoded.at("m_editorOverride");
    // 对新数据检查字段不存在，而非仅写出默认值；前者才不会重新制造项目级覆盖。
    if ( editorJson.contains("showToolLabels") ||
         editorJson.contains("fixedToolWindow") ||
         editorJson.contains("showManagerLabels") ||
         editorJson.contains("toolbarVisibility") ) {
        // 整个嵌套可见性子树必须缺席，不是只清掉几个当前已知的子字段。
        XERROR("Project settings persisted global toolbar display fields");
        return false;
    }

    nlohmann::json legacyEditor = projectEditor;
    // 单独序列化 EditorSettings 可保留完整显示设置，再嵌回项目 JSON
    // 模拟旧格式。
    nlohmann::json legacyProject      = encoded;
    legacyProject["m_editorOverride"] = std::move(legacyEditor);
    // 移入后不再读取 legacyEditor，后续解码只消费完整的 legacyProject。
    // 只替换编辑器覆盖子对象，其他项目设置保留当前有效形状以隔离旧显示字段。
    const auto restored = legacyProject.get<MMM::ProjectSettings>();
    // 先确认覆盖对象没有被整体丢弃，避免“删除所有项目覆盖”误通过字段重置检查。
    if ( !restored.m_editorOverride ) {
        // 删除整个覆盖对象不是合法的字段迁移，也会使下面的解引用失去前提。
        XERROR("Project editor override was not restored");
        return false;
    }

    const auto& restoredEditor = *restored.m_editorOverride;
    // 顶层布尔值和嵌套按钮可见性都要恢复默认，避免仅过滤浅层字段的实现通过。
    // 分别检查标签、停靠模式和两个嵌套可见性字段，不能只过滤顶层简单布尔值。
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
/// @note 当前还覆盖工作区分拍、备份继承及全局显示字段边界，不只测试音频资源。
int main()
{
    // 无命令行参数或外部夹具路径，显式运行测试目标即可验证本文件全部内存场景。
    // 每个用例都创建独立项目或设置对象，不需要在场景之间重置全局资源目录。
    // 部分数量检查只返回 false；非零退出是失败依据，不以日志是否出现推断通过。
    // 各场景独立构造输入，短路失败不会留下需要下一场景清理的项目文件。
    // 只有退出零表示全部场景都已执行；非零时后续未运行场景不计作已验证。
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
