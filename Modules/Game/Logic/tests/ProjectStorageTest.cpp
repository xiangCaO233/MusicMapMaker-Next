/// @file
/// @brief 分片存储、兼容迁移与项目打开流程的回归夹具。
/// 每次运行创建独立临时根，文件写入不触及仓库资源或用户项目。
/// 直接往返与控制器打开后的保存分别检查，避免混淆持久化和项目生命周期两层责任。
#include "logic/ProjectStorage.h"

#include "log/colorful-log.h"
#include "logic/ProjectController.h"
#include "logic/ProjectDirectoryScanner.h"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <string_view>
#include <system_error>

namespace
{

/// @brief 校验测试条件并记录失败原因。
/// @param condition 待验证的不变量，失败后保留可定位的场景描述。
/// @param message 不参与判定的诊断文本。
/// @return 原始条件值，供调用处短路组织多项断言。
/// 不使用 assert，发布构建定义 NDEBUG 后仍执行全部到达的检查。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) {
        // 成功不写日志，批量检查时保留首个失败条件的清晰定位信息。
        XERROR("Project storage test failed: {}", message);
    }
    return condition;
}

/// @brief 创建仅供本测试使用的唯一临时目录。
/// @return 创建失败时返回空路径，主入口据此停止而不尝试清理未知目录。
/// 临时路径只作为夹具容器，项目内资源引用始终保持相对路径形式。
std::filesystem::path createTestRoot()
{
    const auto suffix =
        // 使用单调时钟生成本次夹具名称，不依赖项目标题或用户配置路径。
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("mmm_project_storage_test_" + std::to_string(suffix));
    std::error_code filesystemError;
    std::filesystem::create_directories(root, filesystemError);
    // 场景子目录全部从此根派生，最后的清理无需猜测哪些外部路径属于本次运行。
    // 空返回值是启动失败信号，不能把它传给后面的文件写入与递归清理。
    return filesystemError ? std::filesystem::path{} : root;
}

/// @brief 构造覆盖全部分片职责的项目数据。
/// @return 内存项目值，不创建对应音频或谱面媒体文件。
/// 数据刻意使用非默认值，使字段被遗漏时不会因默认初始化而误通过。
/// 样本包含两个音频资源、一张谱面与一个草稿组，区分各集合的序列化归属。
/// 标题、显示名称和资源路径使用不同文本，避免字段混用仍产生相同结果。
MMM::Project makeProject()
{
    MMM::Project project;
    // 元数据与设置分属不同文件，标题和最近打开项都提供可识别的样本值。
    project.m_metadata.m_title                            = "Split Project";
    project.m_metadata.m_artist                           = "Artist";
    project.m_settings.m_lastOpenedBeatmap                = "Hard";
    project.m_settings.m_workspace.m_activeBeatmapPath    = "hard.mmm";
    project.m_settings.m_workspace.m_projectAudioToolOpen = true;
    // 工具打开状态与主画布打开列表独立，存储拆分不能把两个窗口状态混为一项。
    // 工作区保留稳定画布 ID 和相对谱面路径，不依赖本次临时目录的绝对名称。
    project.m_settings.m_workspace.m_openBeatmaps = {
        // 窗口列表不同于项目谱面列表，关闭的谱面可以仍保留在项目入口中。
        MMM::ProjectWorkspaceBeatmapState{
            .m_filePath                    = "hard.mmm",
            .m_cameraId                    = "Canvas_7",
            .m_displayName                 = "Hard",
            .m_playbackTime                = 12.5,
            .m_canvasHorizontalOffsetRatio = 0.125F,
        },
    };
    // 工具选择指向下方真实定义的音效资源，避免只测试没有关联对象的空状态。
    project.m_settings.m_workspace.m_projectAudioToolSelectedResourceId = "hit";
    project.m_settings.m_workspace.m_projectAudioToolBrushVolume        = 0.65F;
    project.m_settings.m_workspace.m_projectAudioToolPreviewEffectOnSelection =
        // 默认值是关闭，用开启状态观察该偏好是否确实写入独立分片。
        true;
    project.m_settings.m_workspace.m_projectAudioToolPlacements = {
        // 自定义尺寸与非零位置覆盖工具布局分片，而不是只保存资源引用。
        MMM::ProjectAudioToolItemPlacement{
            // 用资源 ID 关联音效，不能在保存后只剩坐标而失去对应资源。
            .m_audioResourceId = "hit",
            .m_x               = 12.0F,
            .m_y               = 34.0F,
            .m_width           = 128.0F,
            .m_height          = 96.0F,
            .m_zOrder          = 2,
        },
    };
    project.m_audioResources = {
        // 主音轨与音效各一项，使加载结果同时覆盖两种资源角色。
        MMM::AudioResource{
            .m_id   = "main",
            .m_path = "bgm.ogg",
            .m_type = MMM::AudioTrackType::Main,
        },
        MMM::AudioResource{
            .m_id   = "hit",
            .m_path = "hit.wav",
            .m_type = MMM::AudioTrackType::Effect,
        },
    };
    project.m_beatmaps = {
        // 入口只记录谱面路径，存储测试不需要构造实际谱面内容。
        MMM::Project::BeatmapEntry{
            .m_name     = "Hard",
            .m_filePath = "hard.mmm",
        },
    };
    project.m_draftLaneGroups = {
        // 草稿载荷与显式轨道数需要持久化，运行时修订号则必须丢弃。
        MMM::ProjectDraftLaneGroup{
            // 谱面路径关联上方唯一入口，不能再用共同主音轨串联多个难度。
            .m_beatmapFilePath = "hard.mmm",
            .m_notePayload     = "draft-payload",
            .m_trackCount      = 7,
            .m_runtimeRevision = 17U,
        },
    };
    project.m_excludedAudioPaths = { "unused.wav" };
    // 保持相对路径形式，临时目录的运行期后缀不应进入可迁移的项目数据。
    // 该列表按用户选择保留，不能因为对应文件没创建就从序列化输入中删掉。
    // 排除项随项目分片序列化，不要求磁盘上存在对应资源文件。
    return project;
}

/// @brief 将旧版项目顶层 JSON 写入根目录。
/// @param root 该兼容场景独占的临时项目目录。
/// @param project 通过领域序列化器写成旧版单文件结构的样本。
/// @return 流打开与写出均成功时为 true，调用方不继续使用失败夹具。
/// 通过领域序列化生成合法旧结构，避免手写 JSON 漏字段干扰回退检查。
bool writeLegacyProjectFile(const std::filesystem::path& root,
                            const MMM::Project&          project)
{
    std::ofstream file(root / "mmm_project.json", std::ios::trunc);
    // 不调用新版 storage.save，确保测试输入确实是旧布局而不是新分片。
    if ( !file.is_open() ) return false;
    file << std::setw(4) << nlohmann::json(project) << '\n';
    // helper 只负责准备输入，不执行迁移或删除旧文件。
    return file.good();
}

/// @brief 验证分片保存、职责拆分和完整往返。
/// @param root 本次夹具根目录，保存时允许内部 .mmm 尚不存在。
/// @return 文件布局、字段归属及主要领域值均符合约定时为 true。
/// 先检查磁盘分片再执行领域回读，防止读写两端共同的字段放错位置被掩盖。
/// 只比较具有明确持久化职责的字段，不要求运行时缓存与输入对象逐字节相等。
/// 文件存在性、字段归属、读取来源与领域值分别断言，不能互相替代。
bool testSplitRoundTrip(const std::filesystem::path& root)
{
    MMM::Logic::ProjectStorage storage;
    std::string                errorMessage;
    const auto                 project = makeProject();
    // 使用同一份输入完成保存与回读比较，后续不修改样本填补缺失字段。
    if ( !storage.save(project, root, errorMessage) ) {
        // 保存失败后停止读取，避免成串的文件缺失诊断遮盖最初原因。
        XERROR("Failed to save split project: {}", errorMessage);
        return false;
    }

    const auto directory = root / ".mmm";
    // 直接使用存储约定的相对目录，验证保存没有把分片散落在项目根。
    const std::array expectedFiles{
        // 包括入口及所有职责分片；只写 manifest 或遗漏可选新分片均会失败。
        "manifest.json",  "project.json",
        "settings.json",  "audio_resources.json",
        "beatmaps.json",  "draft_lanes.json",
        "workspace.json", "project_audio_tool.json",
    };
    for ( const auto* filename : expectedFiles ) {
        // 要求常规文件而非仅存在同名路径，目录不能冒充已经保存的 JSON。
        if ( !check(std::filesystem::is_regular_file(directory / filename),
                    "all split files should exist") ) {
            return false;
        }
    }

    nlohmann::json settingsJson;
    // 原始对象专门用于验证物理字段归属，不能用合并后的 Project 替代。
    nlohmann::json workspaceJson;
    nlohmann::json audioToolJson;
    {
        // 直接读取三个相关文件，不经过合并函数，观察工作区与工具的物理拆分。
        std::ifstream settingsFile(directory / "settings.json");
        std::ifstream workspaceFile(directory / "workspace.json");
        std::ifstream audioToolFile(directory / "project_audio_tool.json");
        settingsJson  = nlohmann::json::parse(settingsFile, nullptr, false);
        workspaceJson = nlohmann::json::parse(workspaceFile, nullptr, false);
        audioToolJson = nlohmann::json::parse(audioToolFile, nullptr, false);
    }
    // 旧容器中不能残留另一分片负责的字段，否则手工修改会形成两个来源。
    if ( !check(!settingsJson.contains("m_workspace"),
                "workspace should not remain in general settings") ||
         // 布局和试听偏好都检查移出，防止拆分时只处理了部分工具字段。
         !check(!workspaceJson.contains("m_projectAudioToolPlacements"),
                "audio tool layout should not remain in general workspace") ||
         !check(!workspaceJson.contains(
                    "m_projectAudioToolPreviewEffectOnSelection"),
                "audio tool preferences should not remain in general "
                "workspace") ||
         !check(std::abs(
                    // 用非默认音量和容差检查 JSON
                    // 往返，不要求浮点十进制文本逐字相同。
                    audioToolJson.value("m_projectAudioToolBrushVolume", 0.0F) -
                    0.65F) < 1e-6F,
                "audio tool brush volume should use its own file") ||
         !check(audioToolJson.value(
                    "m_projectAudioToolPreviewEffectOnSelection", false),
                "audio tool selection preview should use its own file") ||
         // 布局至少有一个完整条目，空数组不能满足工具状态拆分要求。
         !check(audioToolJson["m_projectAudioToolPlacements"].size() == 1,
                "audio tool layout should use its own file") ) {
        // 文件职责不正确时不继续领域回读，否则合并逻辑可能掩盖冗余字段。
        return false;
    }

    const auto loaded = storage.load(root);
    // load 返回的来源必须是 Split，不能靠遗留单文件暗中完成往返。
    // 以下短路顺序先验证集合数量，再对 front 访问其内容。
    return check(loaded.m_success, "split project should load") &&
           // 成功来源必须明确，不能把默认对象或旧文件当作这次分片往返结果。
           check(loaded.m_source == MMM::Logic::ProjectStorage::Source::Split,
                 "split project should report split source") &&
           check(loaded.m_project.m_metadata.m_title == "Split Project",
                 "metadata should round trip") &&
           check(loaded.m_project.m_audioResources.size() == 2,
                 // 两种资源都保留，不能只恢复主音轨而丢掉采样资源。
                 "audio resources should round trip") &&
           check(loaded.m_project.m_beatmaps.size() == 1,
                 "beatmaps should round trip") &&
           check(loaded.m_project.m_draftLaneGroups.size() == 1,
                 "draft lane groups should round trip") &&
           // 联合检查谱面归属、载荷与轨道数，排除只重建空分组的实现。
           check(
               loaded.m_project.m_draftLaneGroups.front().m_beatmapFilePath ==
                       "hard.mmm" &&
                   loaded.m_project.m_draftLaneGroups.front().m_notePayload ==
                       "draft-payload" &&
                   loaded.m_project.m_draftLaneGroups.front().m_trackCount == 7,
               "draft lane group identity, payload and count should round "
               "trip") &&
           check(loaded.m_project.m_draftLaneGroups.front().m_runtimeRevision ==
                     0U,
                 // 非零输入应归零；保留它会把上一个运行周期的缓存版本带回来。
                 // 修订号是当前进程的缓存失效信息，不能把旧运行状态带入新会话。
                 "draft lane runtime revision should not persist") &&
           check(
               loaded.m_project.m_settings.m_workspace.m_openBeatmaps.size() ==
                   1,
               "open beatmaps should round trip") &&
           // 水平偏移应属于每个已打开谱面的工作区状态，而不是丢回统一默认值。
           check(std::abs(loaded.m_project.m_settings.m_workspace.m_openBeatmaps
                              .front()
                              .m_canvasHorizontalOffsetRatio -
                          0.125F) < 1e-6F,
                 "canvas horizontal offset should round trip") &&
           check(loaded.m_project.m_settings.m_workspace
                         .m_projectAudioToolPlacements.size() == 1,
                 "audio tool layout should round trip") &&
           // 先证明布局存在，后面的宽度读取才有有效的首元素。
           // 领域回读还需检查工具偏好，物理分片写入正确不代表合并路径正确。
           check(std::abs(loaded.m_project.m_settings.m_workspace
                              .m_projectAudioToolBrushVolume -
                          0.65F) < 1e-6F,
                 "audio tool brush volume should round trip") &&
           check(loaded.m_project.m_settings.m_workspace
                     .m_projectAudioToolPreviewEffectOnSelection,
                 "audio tool selection preview should round trip") &&
           // 宽度用非默认数值验证，不能以重新生成一个默认布局块代替回读。
           check(std::abs(loaded.m_project.m_settings.m_workspace
                              .m_projectAudioToolPlacements.front()
                              .m_width -
                          128.0F) < 1e-6F,
                 "custom block size should round trip");
}

/// @brief 验证早期分片项目缺少草稿文件时仍按空草稿组载入。
/// @param root 与完整往返场景分开的目录，删除文件不影响其他用例。
/// @return 缺失可选分片时加载成功且草稿集合为空。
/// 与对象字段缺省不同，此例移除整个文件，验证存储布局层的兼容路径。
/// 其余必需分片保持有效，不能因缺少草稿文件就退回不存在的旧布局。
bool testSplitWithoutDraftFile(const std::filesystem::path& root)
{
    MMM::Logic::ProjectStorage storage;
    std::string                errorMessage;
    if ( !storage.save(makeProject(), root, errorMessage) ) {
        // 先准备其余有效分片，避免把多个损坏原因混入兼容性检查。
        XERROR("Failed to prepare legacy split project: {}", errorMessage);
        return false;
    }

    std::error_code filesystemError;
    std::filesystem::remove(root / ".mmm" / "draft_lanes.json",
                            filesystemError);
    // 删除失败要中止测试，否则仍会加载原先的非空草稿，无法检验缺省路径。
    // 只移除后加的草稿分片，模拟旧版本布局，而非写一个空 JSON 替代它。
    if ( filesystemError ) return false;

    const auto loaded = storage.load(root);
    // 原样本含非空草稿，回读为空说明确实使用了缺失分片的默认路径。
    return check(loaded.m_success,
                 "split project without draft file should load") &&
           // 不是丢弃整个项目，只对缺失的可选草稿集合使用空默认值。
           check(loaded.m_project.m_draftLaneGroups.empty(),
                 "missing draft file should produce an empty group list");
}

/// @brief 验证旧主音频草稿组可保留迁移键和未声明轨道数。
/// @return 反序列化及再次序列化均保留旧迁移信息时返回 true。
/// 直接验证领域对象的旧字段兼容，不需要写入磁盘或经过项目打开流程。
/// 未声明轨道数由后续业务解释，测试不在这里替换成某个默认轨道数。
bool testLegacyDraftGroupWithoutTrackCount()
{
    const auto legacyJson = nlohmann::json{
        // 刻意不写 m_trackCount，零值表达“旧数据未声明”，不是显式零轨谱面。
        { "m_mainAudioResourceId", "main" },
        { "m_notePayload", "legacy-draft" },
    };
    const auto group = legacyJson.get<MMM::ProjectDraftLaneGroup>();
    // 立即重新编码模拟项目打开后的自动保存，不经过草稿服务认领流程。
    const auto saved = nlohmann::json(group);
    // 输入没有 m_trackCount，测试不先人为补零，以实际覆盖反序列化默认值。
    // 尚未被谱面认领前再次保存仍写旧键，避免项目打开时自动保存丢掉迁移入口。
    return check(
        group.m_beatmapFilePath.empty() &&
            group.m_legacyMainAudioResourceId == "main" &&
            group.m_notePayload == "legacy-draft" && group.m_trackCount == 0 &&
            saved.value("m_mainAudioResourceId", std::string{}) == "main" &&
            // 新谱面键尚不存在，不能写一个空字段遮蔽旧音频迁移键。
            !saved.contains("m_beatmapFilePath"),
        "legacy draft group should leave track count undeclared");
}

/// @brief 验证资源扫描不会把内部配置目录中的文件识别为谱面。
/// @param root 已由完整往返场景创建 .mmm 的项目根目录。
/// @return 只有根目录的 visible.mmm 被收集时为 true。
/// 此场景验证路径过滤，不把文件内容是否为合法谱面混入扫描行为。
/// 隐藏目录同时含有真实分片和伪造谱面候选，排除不能依赖几个固定文件名。
bool testInternalDirectoryIsNotScanned(const std::filesystem::path& root)
{
    {
        // 两个空文件使用相同扩展名，仅位置不同，确保差异来自内部目录排除。
        std::ofstream visibleBeatmap(root / "visible.mmm");
        std::ofstream internalBeatmap(root / ".mmm" / "internal.mmm");
        // 伪造内部谱面不是配置分片名称，要求扫描器排除的是目录范围。
        if ( !visibleBeatmap.is_open() || !internalBeatmap.is_open() ) {
            // 文件未能准备好时退出，防止把缺失内部文件误认为过滤成功。
            return false;
        }
    }

    const MMM::Logic::ProjectDirectoryScanner scanner;
    // 文件流已关闭，扫描不会依赖平台对未关闭输出句柄的可见性行为。
    const auto result = scanner.scan(root);
    // 一个候选同时约束两种错误：漏掉可见谱面或额外导入隐藏候选。
    return check(result.m_success, "project scan should complete") &&
           check(result.m_beatmapFiles.size() == 1,
                 "internal .mmm files should not be scanned") &&
           check(
               result.m_beatmapFiles.front().filename() ==
                   // 数量检查在前；即使扫描返回空列表，也不会访问不存在的首项。
                   std::filesystem::path("visible.mmm"),
               "visible project beatmap should remain discoverable");
}

/// @brief 验证新分片损坏时仍可回退旧文件并在成功迁移后移除旧文件。
/// @param root 旧格式兼容场景的独立目录。
/// @return 先回退 Legacy，迁移后删除旧入口并能选择 Split 时为 true。
/// 使用不完整分片而非非法 JSON，让错误发生在分片读取阶段。
/// 此场景与完整往返目录分开，防止已有分片意外补足测试故意缺失的文件。
bool testLegacyFallbackAndRemoval(const std::filesystem::path& root)
{
    MMM::Logic::ProjectStorage storage;
    const auto                 project = makeProject();
    // 旧单文件必须先写成有效项目，后面才能区分回退失败与新布局不完整。
    if ( !writeLegacyProjectFile(root, project) ) return false;
    // 后续创建的新入口故意不配齐文件，旧入口仍保留为唯一可完整加载的版本。

    std::error_code filesystemError;
    std::filesystem::create_directories(root / ".mmm", filesystemError);
    if ( filesystemError ) return false;
    {
        std::ofstream manifest(root / ".mmm" / "manifest.json");
        // 只建立合法入口但不建立数据分片，复现可识别新布局却无法完整读取的情况。
        manifest << R"({"format_version":1,"storage":"split"})";
        // 结束作用域先关闭写入流，再触发加载，让输入状态稳定可见。
    }

    const auto loaded = storage.load(root);
    // 来源断言不可省略：只看成功标志无法证明实际走了旧文件回退。
    if ( !check(loaded.m_success,
                "legacy project should load when split files are incomplete") ||
         !check(loaded.m_source == MMM::Logic::ProjectStorage::Source::Legacy,
                "incomplete split project should fall back to legacy") ) {
        // 回退结果无效时不能继续写入默认项目，否则会掩盖读取失败。
        return false;
    }

    std::string errorMessage;
    // 使用已加载项目生成全部新分片，模拟实际迁移，而非复制预制测试文件。
    if ( !storage.save(loaded.m_project, root, errorMessage) ) {
        // 写入失败保留旧文件，测试严格遵守“先保存新布局、再清理旧入口”。
        XERROR("Failed to migrate legacy project: {}", errorMessage);
        return false;
    }
    if ( !storage.removeLegacyProjectFile(root, errorMessage) ) {
        // 只有保存成功后才清理旧入口，保持迁移顺序与正常项目打开流程一致。
        XERROR("Failed to remove legacy project: {}", errorMessage);
        return false;
    }
    return check(!std::filesystem::exists(root / "mmm_project.json"),
                 "legacy project file should be removed after migration") &&
           // 再次 load 必须识别新分片，不能只以“旧文件已删”证明迁移可用。
           check(storage.load(root).m_source ==
                     MMM::Logic::ProjectStorage::Source::Split,
                 "migrated project should load from split storage");
}

/// @brief 验证打开项目时不会在自动保存阶段清空已持久化的草稿轨组。
/// @param root 独立的测试项目目录。
/// @return 内存项目和重新读取的分片均保留草稿轨组时返回 true。
/// 覆盖控制器打开后的自动持久化，不仅是 ProjectStorage 自身的直接读写。
/// 独立根不包含内部扫描场景生成的空谱面，避免谱面解析干扰草稿保存检查。
bool testOpenProjectPreservesDraftLaneGroups(const std::filesystem::path& root)
{
    MMM::Logic::ProjectStorage storage;
    std::string                errorMessage;
    if ( !storage.save(makeProject(), root, errorMessage) ) {
        // 初始化磁盘内容后再进入控制器，避免内存默认空项目成为测试起点。
        XERROR("Failed to prepare draft lane project: {}", errorMessage);
        return false;
    }

    auto&       controller    = MMM::Logic::ProjectController::instance();
    const auto  openResult    = controller.openProject(root);
    const auto* openedProject = controller.currentProject();
    // 先缓存打开后内存状态的判定，关闭项目后不能继续借用该指针。
    const bool memoryPreserved =
        // 短路先确认打开成功及指针有效，失败状态下不访问草稿容器。
        openResult.m_opened && openedProject &&
        openedProject->m_draftLaneGroups.size() == 1U &&
        openedProject->m_draftLaneGroups.front().m_notePayload ==
            "draft-payload";
    const auto reloaded = storage.load(root);
    // 在 close 之前回读，专门检查打开阶段已经发生的保存是否保留草稿。
    const auto closed = controller.closeProject();
    // 无论后续断言是否成功，先释放单例控制器持有的当前项目，隔离下一个场景。

    return check(memoryPreserved,
                 "opening a project should retain draft lane groups") &&
           // 内存保留不代表磁盘正确，两层都校验同一载荷，避免误保存空草稿。
           check(
               reloaded.m_success &&
                   reloaded.m_project.m_draftLaneGroups.size() == 1U &&
                   reloaded.m_project.m_draftLaneGroups.front().m_notePayload ==
                       "draft-payload",
               "opening a project should not overwrite persisted drafts") &&
           check(closed.m_closed,
                 // 关闭也是隔离条件，避免单例控制器状态污染后续无配置项目。
                 "draft lane project should close after the test");
}

/// @brief 验证无项目配置的目录首次打开和保存后均继承软件默认配色。
/// @param root 独立的测试项目目录。
/// @return 打开、持久化和关闭均符合预期时返回 true。
/// 空方案名是继承软件配色的存储约定，不应自动固化为某个当前方案名称。
/// 不比较具体 RGBA 值，使断言独立于测试机器使用的皮肤和软件默认方案。
/// 已有项目的显式配色覆盖不在本例输入中，避免与首次打开的继承语义混淆。
bool testOpenWithoutConfigurationInheritsPalette(
    const std::filesystem::path& root)
{
    std::error_code error;
    std::filesystem::create_directories(root, error);
    // 不写任何配置入口，强制走控制器首次建立配置的分支。
    // 只建空目录，不写 manifest，确保进入“首次打开无配置项目”的分支。
    if ( error ) return false;
    auto&       controller = MMM::Logic::ProjectController::instance();
    const auto  opened     = controller.openProject(root);
    const auto* project    = controller.currentProject();
    // 检查字段语义而非具体颜色，软件当前皮肤不会影响本例预期。
    const bool inherits = opened.m_opened && project &&
                          project->m_settings.m_colorPaletteSchemeName.empty();
    // 判定保存为值后才关闭项目，避免在断言中借用已经失效的领域对象。
    const auto reloaded = MMM::Logic::ProjectStorage{}.load(root);
    // 控制器首次保存后仍应保留继承标记，不能在序列化时替换成局部默认方案。
    const auto closed = controller.closeProject();
    return check(inherits,
                 "unconfigured projects should inherit the software palette") &&
           // 再看持久化结果，内存中的正确默认值不代表首次保存也正确。
           check(reloaded.m_success && reloaded.m_project.m_settings
                                           .m_colorPaletteSchemeName.empty(),
                 "saving should preserve software palette inheritance") &&
           check(closed.m_closed, "palette test project should close");
}

}  // namespace

/// @brief 运行项目分片存储与旧格式迁移测试。
/// @return 所有场景通过时为零，任一准备或断言失败时为一。
/// 临时根的子目录按场景分开，唯一复用是往返后对同一根执行内部目录扫描。
/// 控制器场景显式关闭项目，递归清理只处理已不再由活动项目使用的目录。
int main()
{
    const auto root = createTestRoot();
    // 运行环境不需要预先准备项目样本，所有兼容布局都在这一临时根内生成。
    // 创建失败立即退出，不能把空路径交给最后的递归清理。
    if ( root.empty() ) return 1;
    const auto fallbackRoot    = root / "legacy";
    const auto oldSplitRoot    = root / "old_split";
    const auto openProjectRoot = root / "open_project";
    // 旧文件迁移不与完整新布局共用目录，防止互相提供意外的回退输入。
    std::error_code filesystemError;
    std::filesystem::create_directories(fallbackRoot, filesystemError);
    // 旧文件写入 helper 不建父目录，必须先完成场景目录准备。

    const bool success =
        // 用短路链在首个失败后停止；共享根的后续场景依赖前置准备成功。
        !filesystemError && testSplitRoundTrip(root) &&
        testSplitWithoutDraftFile(oldSplitRoot) &&
        testLegacyDraftGroupWithoutTrackCount() &&
        testInternalDirectoryIsNotScanned(root) &&
        testLegacyFallbackAndRemoval(fallbackRoot) &&
        testOpenProjectPreservesDraftLaneGroups(openProjectRoot) &&
        testOpenWithoutConfigurationInheritsPalette(root / "no_configuration");
    std::filesystem::remove_all(root, filesystemError);
    // 清理只使用本次生成的具体路径，不根据项目资源字段扩展删除范围。
    // 只清理本次生成的根；测试成功与否由前面的业务断言决定。
    return success ? 0 : 1;
}
