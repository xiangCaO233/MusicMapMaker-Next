#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/AudioPlaybackConfig.h"
#include "config/FontPreferenceValidator.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/theme/ImGuiThemeRegistry.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <filesystem>
#include <nfd.h>
#include <system_error>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 默认皮肤目录名。
///
/// 该名称同时用于候选排序、无加载路径时的回退和恢复默认皮肤入口。
constexpr const char* kDefaultSkinDirectoryName = "mmm-default";

/// @brief 获取指定皮肤目录的入口脚本路径。
/// @param skinDirectoryName skins 根目录下的皮肤目录名。
/// @return 皮肤入口脚本完整路径。
///
/// 函数只做词法路径拼接，不检查目录名是否合法或文件是否存在。存在性验证由
/// `skinLuaFileExists` 在实际选择与扫描阶段统一完成。
std::filesystem::path skinLuaPathForDirectory(
    const std::string& skinDirectoryName)
{
    // 以应用配置的 skins 根目录为唯一基准，避免依赖当前工作目录。
    std::filesystem::path path = Config::AppPaths::skinsRootPath();
    // UTF-8 目录名通过平台路径转换 helper 加入。
    path /= Config::utf8ToPath(skinDirectoryName);
    // 每个可加载皮肤必须以 skin.lua 作为固定入口。
    path /= "skin.lua";
    return path;
}

/// @brief 获取当前实际加载的皮肤目录名。
/// @param settings 编辑器设置。
/// @return 当前皮肤目录名。
///
/// 解析顺序优先采用 SkinManager 已实际加载的路径，其次采用持久化选择，最后回退
/// 内置默认目录。这样加载失败时 UI 仍能展示可恢复的配置值。
std::string currentSkinDirectoryName(const Config::EditorSettings& settings)
{
    // 实际加载路径是运行态真值，优先级高于可能过期的 AppConfig。
    auto loadedName = Config::pathToUtf8(
        Config::SkinManager::instance().getData().skinPath.filename());
    if ( !loadedName.empty() ) {
        return loadedName;
    }
    if ( !settings.selectedSkinDirectory.empty() ) {
        // 尚无已加载路径时保留用户的持久化选择供下拉框显示。
        return settings.selectedSkinDirectory;
    }
    return kDefaultSkinDirectoryName;
}

/// @brief 检查皮肤入口脚本是否存在。
/// @param skinLuaPath 皮肤入口脚本路径。
/// @return 文件存在且是普通文件时返回 true。
///
/// 使用 error_code 接口避免权限错误或路径竞态通过异常中断设置页。
bool skinLuaFileExists(const std::filesystem::path& skinLuaPath)
{
    // 同一 error_code 承接 exists 与文件类型查询，任一失败都会返回 false。
    std::error_code ec;
    return std::filesystem::exists(skinLuaPath, ec) &&
           std::filesystem::is_regular_file(skinLuaPath, ec);
}

/// @brief 按当前皮肤配置预加载所有皮肤音效。
///
/// 每个 audioPaths 条目按相同 key 查询可选 lead-in；缺失时使用零秒。预加载只
/// 填充 AudioManager 音效池，不触发实际播放。
/// @warning 低频资源重载路径：皮肤热切换后调用，会触发音频资源加载。
void preloadCurrentSkinSoundEffects()
{
    // SkinData 在皮肤切换流程内保持稳定，本函数只借用只读引用。
    const auto& skinData = Config::SkinManager::instance().getData();
    for ( const auto& [key, path] : skinData.audioPaths ) {
        // lead-in 表与路径表允许不完全对应，按 key 独立查找。
        const auto   leadInIt = skinData.audioLeadInSeconds.find(key);
        const double leadInSeconds =
            leadInIt != skinData.audioLeadInSeconds.end() ? leadInIt->second
                                                          : 0.0;
        // 路径转换为音频层使用的 UTF-8，并保持皮肤定义的起播偏移。
        Audio::AudioManager::instance().preloadSoundEffect(
            key, Config::pathToUtf8(path), 1.0f, leadInSeconds);
    }
}
}  // namespace

/// @brief 刷新可选皮肤目录名缓存。
///
/// 只有包含普通 `skin.lua` 的直接子目录才进入候选。默认皮肤在字典序排序后再次
/// 稳定移到首位，重复名称最终去除。
///
/// 扫描容错约定：
/// - skins 根目录缺失视为没有候选；
/// - 根目录不是文件夹时同样返回空候选；
/// - 无权限子目录由 directory_iterator 选项跳过；
/// - 单个条目状态查询失败不会中止其余候选；
/// - `skin.lua` 必须存在并是普通文件；
/// - 扫描不解析 Lua，也不验证皮肤内部资源完整性；
/// - 缓存只保存目录名，不保留迭代器或 filesystem entry。
/// @warning 低频文件系统路径：只在设置窗口打开或缓存标脏时扫描
/// AppPaths::skinsRootPath()，禁止每帧无条件调用。
void SettingsView::refreshAvailableSkinDirectories()
{
    // 先清空旧快照，扫描失败时不会继续展示已经删除的目录。
    m_availableSkinDirectories.clear();

    // 根目录缺失或不可访问时视为空候选，并消费 dirty 标志。
    std::error_code ec;
    const auto      skinsRoot = Config::AppPaths::skinsRootPath();
    if ( !std::filesystem::exists(skinsRoot, ec) ||
         !std::filesystem::is_directory(skinsRoot, ec) ) {
        m_availableSkinDirectoriesDirty = false;
        return;
    }

    // 只遍历 skins 根目录一层，皮肤内部资源目录不作为独立皮肤。
    std::filesystem::directory_iterator it(
        skinsRoot,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::directory_iterator end;
    while ( it != end ) {
        // 每个条目使用独立错误码，单项失败不会污染迭代器推进状态。
        std::error_code itemEc;
        if ( it->is_directory(itemEc) ) {
            const auto skinLuaPath = it->path() / "skin.lua";
            if ( skinLuaFileExists(skinLuaPath) ) {
                // 缓存只保存 UTF-8 目录名，完整路径在选择时重新构造。
                m_availableSkinDirectories.push_back(
                    Config::pathToUtf8(it->path().filename()));
            }
        }
        it.increment(ec);
        if ( ec ) {
            // 跳过本次推进错误，继续尝试后续可访问条目。
            ec.clear();
        }
    }

    // 首轮字典序排序为确定性展示和 unique 去重提供前提。
    std::sort(m_availableSkinDirectories.begin(),
              m_availableSkinDirectories.end());
    // 稳定二次排序仅把内置默认皮肤移动到最前，其他顺序保持不变。
    std::stable_sort(
        m_availableSkinDirectories.begin(),
        m_availableSkinDirectories.end(),
        [](const std::string& lhs, const std::string& rhs) {
            // 相等元素不建立额外顺序，满足稳定排序比较器要求。
            if ( lhs == rhs ) return false;
            const bool lhsDefault = lhs == kDefaultSkinDirectoryName;
            const bool rhsDefault = rhs == kDefaultSkinDirectoryName;
            if ( lhsDefault != rhsDefault ) return lhsDefault;
            return lhs < rhs;
        });
    // 排序后的相邻重复名称一次性移除。
    m_availableSkinDirectories.erase(
        std::unique(m_availableSkinDirectories.begin(),
                    m_availableSkinDirectories.end()),
        m_availableSkinDirectories.end());
    // 无论候选数量如何，本轮扫描已经完成。
    m_availableSkinDirectoriesDirty = false;
}

/// @brief 应用皮肤选择并请求图形/音频资源热重载。
/// @param skinDirectoryName skins 根目录下的皮肤目录名。
/// @param skinLuaPath 皮肤入口脚本路径。
/// @return 切换成功时返回 true。
///
/// 成功切换需要依次完成 Lua 配置加载、字体偏好校验、调色板刷新、音效池重建、
/// 图形主题应用和 UI 资源重载请求。函数不保存 AppConfig，调用方通过返回值并入
/// 当前设置页的统一保存路径。
///
/// 失败发生在验证或 SkinManager 加载阶段时保持原运行态皮肤，并通过中心通知告知
/// 用户。后续资源重载只在新皮肤已经成为 SkinManager 当前数据后执行。
///
/// 成功后的刷新顺序：
/// - 校验新皮肤是否仍包含用户偏好的 ASCII/CJK 字体；
/// - 保存新皮肤稳定目录名；
/// - 失效设置窗口活动和预备布局缓存；
/// - 通知主 Dock 刷新调色板解析；
/// - 清空旧皮肤音效池；
/// - 预加载新皮肤定义的所有音效和 lead-in；
/// - 重新登记当前工程覆盖的效果音；
/// - 重载已经打开的效果音轨引用；
/// - 应用主题并请求字体图集重建；
/// - 请求 UIManager 重载其余皮肤资源。
///
/// 该流程不持有 session 锁，也不直接保存配置文件。调用者必须位于用户明确选择
/// 皮肤的低频路径，并在 true 返回值后完成统一持久化。
///
/// 失败语义：
/// - 空目录名直接失败；
/// - 入口缺失或不是普通文件直接失败；
/// - Lua 加载失败保留旧运行态数据；
/// - 没有 VKContext 时仍可完成非图形资源切换；
/// - 没有 UIManager 时跳过视图级刷新，但不撤销已加载皮肤；
/// - 后续异步重建请求不改变本函数的成功结果。
///
/// 成功返回只表示新皮肤配置已被接受并已安排相关刷新，不保证所有 GPU 资源已在
/// 当前调用栈完成上传。消费者必须尊重各自的安全帧边界。
/// @warning 低频资源重载路径：会加载 Lua、清理音效池并请求 Vulkan
/// 资源重建，只能由设置页皮肤选择触发。
bool SettingsView::applySkinSelection(const std::string& skinDirectoryName,
                                      const std::filesystem::path& skinLuaPath)
{
    // 目录名和入口文件都必须有效，避免把空选择写入持久化设置。
    if ( skinDirectoryName.empty() || !skinLuaFileExists(skinLuaPath) ) {
        // VKContext 可暂时不可用；通知是可选反馈，不影响失败返回值。
        if ( auto ctx = Graphic::VKContext::get() ) {
            ctx->get().showCenterNotification("皮肤入口不存在");
        }
        return false;
    }

    // SkinManager 负责解析 skin.lua 并建立新的运行态 SkinData。
    if ( !Config::SkinManager::instance().loadSkin(
             Config::pathToUtf8(skinLuaPath)) ) {
        if ( auto ctx = Graphic::VKContext::get() ) {
            // 加载失败保持原皮肤，不执行后续资源清理。
            ctx->get().showCenterNotification("皮肤加载失败");
        }
        return false;
    }

    // 加载成功后再调整与新皮肤字体清单相关的持久化偏好。
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    if ( Config::resetUnavailableFontPreferences(
             settings, Config::SkinManager::instance()) ) {
        // 字体回退属于兼容处理，记录警告便于诊断用户偏好变化。
        XWARN("Unavailable font preference reset after skin switch");
    }
    // 保存稳定目录名，而不是机器相关的绝对 skin.lua 路径。
    settings.selectedSkinDirectory = skinDirectoryName;
    // 翻译、字体与布局可能全部变化，活动和预备尺寸缓存同时失效。
    m_layoutMetricsCache.valid = false;
    m_hasPreparedLayoutMetrics = false;

    if ( m_sourceManager ) {
        // 主停靠区存在时立即刷新依赖皮肤的调色板可见状态。
        if ( auto* mainDock = m_sourceManager->getView<MainDockSpaceUI>(
                 "MainDockSpaceUI") ) {
            mainDock->refreshPaletteAfterSkinChange();
        }
    }

    // 旧皮肤音效不能与新 key/path 映射混用，先清池再预加载。
    auto& audio = Audio::AudioManager::instance();
    audio.clearSoundEffects();
    preloadCurrentSkinSoundEffects();
    // 皮肤音效之后重新叠加当前工程自定义特效资源。
    Logic::EditorEngine::instance().registerCurrentProjectEffectSoundEffects();
    if ( m_sourceManager ) {
        // 已打开的效果音轨需重新绑定清池后的音频资源。
        m_sourceManager->reloadOpenEffectAudioTracks();
    }

    if ( auto ctx = Graphic::VKContext::get() ) {
        // 主题立即应用，字体图集通过重建请求在安全帧边界更新。
        ctx->get().applyTheme();
        ctx->get().requestFontRebuild();
        // 成功通知使用稳定目录名，避免暴露冗长绝对路径。
        ctx->get().showCenterNotification("皮肤已切换: " + skinDirectoryName);
    }

    if ( m_sourceManager ) {
        // 其余纹理、图标和脚本资源由 UIManager 统一安排重载。
        m_sourceManager->requestSkinResourceReload();
    }
    // true 表示调用方应把设置页标记为已修改并保存选择。
    return true;
}

/// @brief 渲染软件设置页。
///
/// 本页编辑全局
/// `EditorSettings`，覆盖语言、身份、帧率、音频、皮肤、字体、光标、
/// 外观、自动保存、备份、时间格式和同步策略。普通字段在本帧末尾统一保存并发布
/// `CmdUpdateEditorConfig`。
///
/// 部分设置具有即时副作用：语言切换更新 Translator，音频模式与后端通知
/// AudioManager，皮肤选择执行资源热重载，字体与 UI 缩放请求图形资源重建。这些
/// 副作用只在控件实际提交时运行。
///
/// 数值拖动中使用局部临时值的控件会在 `IsItemDeactivatedAfterEdit` 后提交高成本
/// 更新，使拖动视觉保持流畅。普通轻量配置可以随控件变化直接写入内存草稿。
///
/// Clay 负责设置行矩形，ImGui 负责控件交互。所有 Lambda 捕获仅在当前帧布局执行
/// 期间有效，不得保存配置引用、字体指针或局部数组到后续帧。
///
/// 折叠状态只存入 ImGui StateStorage，不持久化到软件配置。隐藏分组不登记内部
/// 控件，因此不会产生副作用或占用布局高度。
///
/// 文件系统与资源重载只允许位于明确的低频交互分支：皮肤缓存标脏、用户导入导出
/// 或主动切换皮肤。不得把目录扫描和皮肤加载移入每帧无条件路径。
///
/// 状态所有权约定：
/// - `EditorSettings` 是所有普通偏好的持久化真值；
/// - `SkinManager` 持有当前已加载皮肤、字体和翻译资源；
/// - `AudioManager` 持有解码偏好、播放后端和空间音频运行态；
/// - `VKContext` 持有主题注册表以及字体/主题重建请求；
/// - `UIManager` 负责已打开视图的皮肤资源和效果音轨刷新；
/// - `EditorEngine` 负责重新登记当前工程的自定义效果音。
///
/// 普通控件的提交约定：
/// - 复选框、单选与轻量组合框可直接修改设置并设置 `changed`；
/// - 需要外部系统确认的后端切换只有成功后才写设置；
/// - 高成本主题更新在连续拖动结束后执行；
/// - 文件对话框取消不视为配置变化；
/// - 折叠、悬浮、Tooltip 和弹窗打开状态不触发保存。
///
/// 字体偏好有三种表示：`Default`、皮肤字体稳定名称、外部字体文件路径。切换皮肤
/// 后会校验名称是否仍可用；无效偏好重置为默认，并请求字体图集在安全帧边界重建。
/// ASCII 与 CJK 偏好相互独立，共同组成最终字体回退链。
///
/// 皮肤切换的顺序具有依赖关系：先让 SkinManager 建立新数据，再刷新调色板；清空
/// 旧音效池后预加载皮肤音效，再叠加工程音效并重载已打开音轨；最后应用主题、
/// 请求字体与 UI 资源重载。调整顺序可能造成旧资源 key 残留或工程音效丢失。
///
/// 原生文件选择器返回的 UTF-8 路径由 NFD 分配，成功分支必须释放；ImGui 文件
/// 对话框由单例持有，显示完成后无论确认或取消都必须 Close。两种实现使用相同的
/// `.ttf/.otf` 过滤范围。
///
/// 自动保存与自动备份共享模式结构但职责不同。自动保存写回当前谱面，自动备份
/// 生成可轮换副本并额外具有保留数量上限；两组触发器不能互相代替或共享字段。
///
/// 同步配置只描述算法参数，不在 UI 线程等待网络或逻辑确认。连续修改应由消费者
/// 覆盖旧状态并继续推进本地交互，不能引入固定时长阻塞。
///
/// 所有 ImGui 内部 ID 必须与翻译文本分离。动态字体和主题候选以稳定名称或资源
/// 路径参与隐藏 ID，避免同名可见项共享状态。
///
/// 单位约定：
/// - UI 缩放和字体缩放是无量纲倍率；
/// - 光标、轨迹、烟雾和圆角、间距、padding 使用逻辑像素；
/// - 轨迹、烟雾、动画过渡与同步间隔使用秒；
/// - OpenAL 方向分量是无量纲向量值；
/// - OpenAL 距离字段使用音频系统约定的世界单位；
/// - 自动保存/备份间隔由数值和 Seconds/Minutes 枚举共同解释。
///
/// 运行态刷新边界：
/// - Translator 可在当前帧立即切换语言；
/// - Theme 可通过 VKContext 立即应用颜色和尺寸；
/// - Font rebuild 只提交请求，不在本函数同步上传图集；
/// - Skin resource reload 只提交给 UIManager，在安全阶段完成；
/// - Audio backend 返回成功后才改变持久化选择；
/// - OpenAL 参数在连续拖动结束后一次性提交；
/// - EditorConfig 事件在本帧末尾汇总发布。
///
/// 临时滑块值采用函数内 static，以跨帧维持 ImGui 拖动状态。控件非活动时必须从
/// EditorSettings 回填，确保配置重载、皮肤切换或其他入口修改后不会保留陈旧值。
/// static 只保存轻量 float，不持有外部对象引用。
///
/// 皮肤目录缓存的 dirty 标志在打开设置窗口和导入资源后设置。扫描失败也会消费
/// 当前 dirty 状态并返回空列表，防止每帧重复访问一个不可用目录；后续显式刷新
/// 可以再次标脏。
///
/// UI 缩放与字体倍率的可见通知使用“需要重启”翻译文本，但 UI 缩放仍会立即调用
/// applyTheme
/// 更新可安全热应用的样式部分。字体图集和其他启动期资源按现有生命周期
/// 在后续重建或重启时完成。
///
/// CursorStyle 为 System 时隐藏软件光标粒子参数，但不清空这些字段。重新选择
/// Software 后应恢复用户此前的尺寸与生命周期偏好。
///
/// 动画过渡持续时间允许在拖动期间写入内存以预览效果，但只有交互结束才把页面
/// 标记为 changed。AlwaysClamp 与显式 clamp 共同覆盖拖动和文本输入路径。
///
/// 自动任务的隐藏字段始终保留：切换到 Disabled 不清空间隔和触发器，Timed 与
/// EventTriggered 之间切换也不重置另一模式的参数。用户返回原模式时应恢复配置。
///
/// 同步算法参数根据模式条件显示。切换模式不清空积分因子或 WaterTank 缓冲值，
/// syncInterval 对所有模式保持可编辑，由消费者决定关闭模式下是否忽略。
///
/// 配置迁移边界：
/// - 本页不修正旧版本未知枚举；
/// - 未知主题 ID 原样显示；
/// - 空字体偏好兼容显示为 Default；
/// - 已删除皮肤字体由切换皮肤流程统一重置；
/// - 旧数值范围的迁移应在 AppConfig 加载时完成；
/// - 控件范围只限制本次用户输入；
/// - 页面不会在纯展示帧静默覆盖持久化值。
///
/// ImGui 作用域约定：
/// - `FeedbackBeginCombo` 成功后必须配对 `FeedbackEndCombo`；
/// - NativeFileDialog 成功路径必须 `NFD_FreePathU8`；
/// - ImGuiFileDialog 显示完成后必须 `Close`；
/// - 动作按钮的 `PushID` 与 `PopID` 必须逐项配对；
/// - 紧凑按钮样式必须在同一回调内恢复；
/// - OpenAL 条件控件不能让禁用或样式状态逃逸；
/// - 折叠标题会临时修改 WorkRect，离开回调前必须恢复。
///
/// 错误反馈约定：
/// - 皮肤入口缺失和加载失败通过中心通知显示；
/// - 原生文件选择器错误写入项目日志；
/// - 用户取消对话框不记录错误；
/// - VKContext 或 UIManager 暂不可用时跳过对应可选刷新；
/// - AudioManager 后端切换失败时保留旧偏好；
/// - 本页不抛出异常，也不使用对话框阻塞主循环。
///
/// 性能约定：
/// - 皮肤目录只在 dirty 时扫描；
/// - 皮肤 Lua 只在用户选择新目录时加载；
/// - 字体只提交重建请求；
/// - 主题只在选择或拖动结束时应用；
/// - 音频空间配置只在编辑结束时提交；
/// - 未展开分组不构造内部控件；
/// - 无 changed 帧不发布事件或保存配置。
///
/// 可见通知与日志不是持久化成功确认。中心通知用于说明资源切换结果或重启要求，
/// 配置文件写入仍由 AppConfig::save 统一负责并使用其自身错误处理。
///
/// 页面打开期间外部系统可能修改部分设置。临时拖动值在控件空闲时回读配置；普通
/// 控件每帧直接引用 EditorSettings，因此自然显示最新内存状态。
///
/// 新增即时副作用时，应先明确所属系统、失败回滚语义和调用频率，并仍把持久化
/// 合并到函数末尾；不能在普通悬浮或布局测量路径执行。
///
/// 新增文件或资源选择时，必须保持选择器只在用户动作后打开，取消不改变配置，
/// 成功结果的外部分配由对应库要求释放。
///
/// 新增条件设置行时，隐藏状态应保留已有值，除非配置规范明确要求模式切换重置。
/// @warning UI 热路径：软件设置页可见时每帧执行；无修改帧不得保存配置、发布命令
/// 或重建图形与音频资源。
void SettingsView::drawSoftwareSettings()
{
    // EditorSettings 由 AppConfig 持有，本函数只借用当前帧引用。
    // 函数结束后不缓存该引用，也不转移配置对象所有权。
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    // changed 汇总需要统一发布和持久化的普通配置变化。
    // 即时副作用成功但字段未变化时也不应触发额外保存。
    bool changed = false;

    // 根 VBox 每帧重建父子关系，行与 section 对象由 SettingsView 缓存复用。
    // clear 不影响已经编辑的 EditorSettings 数据。
    m_contentVBox.clear();
    // 统一间距和内边距维持各设置标签页一致外观。
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    // 行索引包含标题和控件，section 索引只覆盖本帧展开分组。
    // 两个索引每帧从零开始，稳定复用同一缓存槽。
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 使用布局缓存中的统一标签列宽，避免设置页每帧重复测量全部标签。
    // 当前内容缩放作为缓存键，DPI 变化后值列仍保持对齐。
    const float maxLabelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());
    // 所有设置行共享该宽度，动态显示参数时不会横向跳动。

    /// 创建一个可折叠软件设置分组并返回当前帧内容 section。
    /// @param label 本地化标题，也参与页面内稳定 ID 构造。
    /// @param defaultOpen StateStorage 无记录时使用的首次展开状态。
    /// @return 展开时返回非拥有 section 指针，折叠时返回空指针。
    ///
    /// 调用方只在非空返回值下登记控件。section 由 SettingsView 缓存所有，返回
    /// 指针不能跨帧保存。
    ///
    /// 折叠状态改变只影响下一帧内容树，不设置页面 changed，也不保存配置。
    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        // 页面前缀、节、行和标题共同隔离不同折叠状态。
        // 元素与布局继续使用不同后缀，避免 Clay ID 冲突。
        std::string baseIdStr = "SW_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        // 在登记回调前读取状态，以决定本帧是否创建内容区。
        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        // 标题独占一行，使用当前 ImGui frame 高度适配主题与 DPI。
        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                // Clay 给出绝对矩形，ImGui 游标必须移动到标题起点。
                ImGui::SetCursorScreenPos({ r.x, r.y });
                // Header 三态颜色从当前主题基础色逐级增亮。
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                // 三次颜色压栈与回调末尾 PopStyleColor(3) 配对。
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                // 将 TreeNodeEx 约束到 Clay 边界：临时将 WindowPadding 设为 0
                // 以消除外扩，并把 WorkRect.Max.x 调整到 Clay 宽度。
                // WorkRect 属于 ImGui 内部窗口状态，必须在回调结束前恢复。
                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                // 数值 ID 借用指针重载传入，不表示可解引用对象地址。
                // CollapsingHeader 不建立树嵌套，因此无需 TreePop。
                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                // 恢复临时 padding 与 WorkRect，再写回新的折叠状态。
                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        // 标题始终加入根 VBox，内容 section 仅在展开时加入。
        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            // 装饰 section 统一提供背景、行距和内边距。
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            // Fit 高度紧贴当前组实际控件。
            return &sec;
        }
        // 折叠时不登记隐藏控件，也不触发任何即时副作用。
        // 标题自身仍保留在根 VBox 中供用户重新展开。
        return nullptr;
    };

    // 常规组配置边界：
    // - language 控制 Translator 当前语言和下次启动偏好；
    // - defaultCreator 经过统一身份规范化后保存；
    // - frameLimit 只表达渲染调度偏好；
    // - PGO 上传开关同时记录用户已明确回答授权；
    // - audioDecodingMode 只影响新加载资源的缓存策略；
    // - audioPlaybackBackend 必须由 AudioManager 成功切换后才提交；
    // - OpenAL 空间参数仅在 OpenAL 后端且功能启用时显示；
    // - selectedSkinDirectory 保存目录名而非绝对路径；
    // - theme 保存稳定注册表 ID，Auto 使用保留常量；
    // - 字体偏好可保存 Default、皮肤名称或外部路径；
    // - UI 与字体缩放在拖动结束时提交。
    //
    // 该组包含多个即时运行态更新，但所有持久化字段仍通过函数末尾的一次 save
    // 提交。任何提前返回或失败分支都不得把 changed 设为 true。
    if ( auto* sec = addHeader(TR_CACHE("ui.settings.software.general").data(),
                               true) ) {
        // 常规组包含程序级身份、帧调度、音频、皮肤、字体与光标设置。
        // 采用统一标签宽度，使动态 OpenAL 参数和字体行保持同一值列起点。

        // 语言选择使用固定自描述名称，选中后立即切换 Translator。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.language").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 可见名称与持久化语言 ID 通过相同索引严格配对。
                const char* langs[] = { "简体中文 (zh_cn)", "English (en_us)" };
                const char* langIDs[] = { "zh_cn", "en_us" };
                int currentLang       = (settings.language == "en_us") ? 1 : 0;
                // 未知语言 ID 在界面中回退中文索引，不主动覆盖原值。
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackCombo("##LangCombo",
                                              &currentLang,
                                              langs,
                                              IM_ARRAYSIZE(langs)) ) {
                    settings.language = langIDs[currentLang];
                    // Translator 即时切换使本页后续帧采用新语言并失效尺寸缓存。
                    Config::SkinManager::instance().getTranslator().switchLang(
                        settings.language);
                    changed = true;
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.default_creator").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 固定缓冲在打开窗口时由持久化 Creator 初始化。
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::InputTextWithHint(
                         "##DefaultCreator",
                         TR_CACHE("ui.settings.software.default_creator.hint")
                             .data(),
                         m_defaultCreatorInputBuffer.data(),
                         m_defaultCreatorInputBuffer.size()) ) {
                    settings.defaultCreator = Config::normalizeCreatorIdentity(
                        m_defaultCreatorInputBuffer.data());
                    // 每次编辑都把规范化值写入设置，最终统一保存。
                    changed = true;
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 编辑结束后用规范化结果刷新缓冲，移除被截断或非法的尾部。
                    refreshDefaultCreatorInputBuffer();
                }
            });

        // 帧率偏好是离散枚举，具体等待与 VSync 由渲染循环解释。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.framelimit").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 临时整数与选项数组顺序必须对应 FrameLimitPreference 枚举。
                int         limit    = (int)settings.frameLimit;
                const char* limits[] = {
                    TR_CACHE("ui.settings.software.framelimit.vsync").data(),
                    TR_CACHE("ui.settings.software.framelimit.2x").data(),
                    TR_CACHE("ui.settings.software.framelimit.4x").data(),
                    TR_CACHE("ui.settings.software.framelimit.8x").data(),
                    TR_CACHE("ui.settings.software.framelimit.unlimited").data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackCombo("##FrameLimitCombo",
                                              &limit,
                                              limits,
                                              IM_ARRAYSIZE(limits)) ) {
                    // 只更新偏好，不在设置回调中直接 sleep 或重建交换链。
                    settings.frameLimit = (Config::FrameLimitPreference)limit;
                    changed             = true;
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.auto_upload_pgo_profiles").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 复选框垂直居中到 Clay 行，保持与相邻组合框一致。
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                if ( ::MMM::UI::FeedbackCheckbox(
                         "##AutoUploadPgoProfiles",
                         &settings.autoUploadPgoProfiles) ) {
                    // 用户主动切换即视为已询问授权，避免再次弹出同意流程。
                    settings.pgoProfileUploadConsentAsked = true;
                    changed                               = true;
                }
            });

        // 偏好仅影响新加载资源，避免在设置回调中同步重建当前播放图。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.audio_decoding").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // UI 整数只映射 Cached 与 Streaming 两个稳定模式。
                int         mode = settings.audioDecodingMode ==
                                           Config::AudioDecodingMode::Streaming
                                       ? 1
                                       : 0;
                const char* modes[] = {
                    TR_CACHE("ui.settings.software.audio_decoding.cached")
                        .data(),
                    TR_CACHE("ui.settings.software.audio_decoding.streaming")
                        .data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackCombo(
                         "##AudioDecodingMode", &mode, modes, 2) ) {
                    settings.audioDecodingMode =
                        mode == 1 ? Config::AudioDecodingMode::Streaming
                                  : Config::AudioDecodingMode::Cached;
                    // AudioManager
                    // 记录新加载资源的默认策略，当前播放对象不重建。
                    Audio::AudioManager::instance().setDecodingMode(
                        settings.audioDecodingMode);
                    changed = true;
                }
                ImGui::SetItemTooltip(
                    // 提示说明内存与磁盘读取权衡，不占常驻布局宽度。
                    "%s",
                    TR_CACHE("ui.settings.software.audio_decoding.help")
                        .data());
            });

        // 音频后端切换需要 AudioManager 成功建立目标设备后才持久化。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.audio_backend").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 零映射 SDL，一映射 OpenAL；未知配置在此回退 SDL 预览。
                int backend = settings.audioPlaybackBackend ==
                                      Config::AudioPlaybackBackend::OpenAL
                                  ? 1
                                  : 0;
                const char* backends[] = {
                    TR_CACHE("ui.settings.software.audio_backend.sdl").data(),
                    TR_CACHE("ui.settings.software.audio_backend.openal").data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackCombo("##AudioBackendCombo",
                                              &backend,
                                              backends,
                                              IM_ARRAYSIZE(backends)) ) {
                    // 先构造强类型目标，再让 AudioManager 执行实际后端切换。
                    auto target = backend == 1
                                      ? Config::AudioPlaybackBackend::OpenAL
                                      : Config::AudioPlaybackBackend::SDL;
                    if ( Audio::AudioManager::instance().setPlaybackBackend(
                             target) ) {
                        // 只有运行态切换成功才保存偏好，失败时保留原设置。
                        settings.audioPlaybackBackend = target;
                        changed                       = true;
                    }
                }
            });

        // OpenAL 专用配置边界：
        // - enabled 控制空间声像处理总开关；
        // - directionX/Y/Z 构成声源方向向量；
        // - distance 表示当前声源距离；
        // - referenceDistance 定义开始衰减的参考尺度；
        // - maxDistance 定义衰减计算的远端边界；
        // - rolloffFactor 控制随距离衰减强度；
        // - 所有滑块在释放时一次性提交完整结构。
        if ( settings.audioPlaybackBackend ==
             Config::AudioPlaybackBackend::OpenAL ) {
            // 空间音频设置只对 OpenAL 有意义，SDL 模式隐藏但保留已有值。
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.software.openal_spatial").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    // 复选框垂直居中，切换后立即提交完整空间配置。
                    ImGui::SetCursorScreenPos(
                        { r.x,
                          r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                    if ( ::MMM::UI::FeedbackCheckbox(
                             "##OpenALSpatial",
                             &settings.openALSpatialConfig.enabled) ) {
                        // AudioManager 即时更新运行态，最终 AppConfig
                        // 仍统一保存。
                        Audio::AudioManager::instance().setOpenALSpatialConfig(
                            settings.openALSpatialConfig);
                        changed = true;
                    }
                });

            if ( settings.openALSpatialConfig.enabled ) {
                /// 登记一个 OpenAL 空间参数滑块。
                /// @param label 设置行可见标签。
                /// @param id 页面内唯一 ImGui ID。
                /// @param value 目标配置字段地址。
                /// @param minValue 最小交互值。
                /// @param maxValue 最大交互值。
                /// @param format 数值显示格式。
                ///
                /// 拖动期间仅编辑内存字段，释放控件后一次性通知
                /// AudioManager，避免 每个中间值重建或同步空间音频状态。
                auto addSpatialSlider = [&](const char* label,
                                            const char* id,
                                            float*      value,
                                            float       minValue,
                                            float       maxValue,
                                            const char* format) {
                    addSettingItem(
                        *sec,
                        rowIndex,
                        label,
                        maxLabelW,
                        [&, id, value, minValue, maxValue, format](
                            Clay_BoundingBox r, bool) {
                            // 滑块占满当前值列，参数范围由调用点明确给出。
                            ImGui::SetNextItemWidth(r.width);
                            ::MMM::UI::FeedbackSliderFloat(
                                id, value, minValue, maxValue, format);
                            if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                                // 编辑结束后提交完整结构，保证各字段组成一致快照。
                                Audio::AudioManager::instance()
                                    .setOpenALSpatialConfig(
                                        settings.openALSpatialConfig);
                                changed = true;
                            }
                        });
                };

                // 方向向量三个分量允许 -1 至 1，由音频层负责规范化解释。
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_direction_x").data(),
                    "##OpenALDirX",
                    &settings.openALSpatialConfig.directionX,
                    -1.0f,
                    1.0f,
                    "%.4f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_direction_y").data(),
                    "##OpenALDirY",
                    &settings.openALSpatialConfig.directionY,
                    -1.0f,
                    1.0f,
                    "%.4f");
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_direction_z").data(),
                    "##OpenALDirZ",
                    &settings.openALSpatialConfig.directionZ,
                    -1.0f,
                    1.0f,
                    "%.4f");
                // 声源距离允许从监听器原点到 100 个配置单位。
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_distance").data(),
                    "##OpenALDistance",
                    &settings.openALSpatialConfig.distance,
                    0.0f,
                    100.0f,
                    "%.4f");
                // 参考距离保持正数，避免衰减公式出现退化分母。
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_reference_distance")
                        .data(),
                    "##OpenALReferenceDistance",
                    &settings.openALSpatialConfig.referenceDistance,
                    0.01f,
                    100.0f,
                    "%.4f");
                // 最大距离上限高于参考距离交互范围，支持大场景衰减。
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_max_distance").data(),
                    "##OpenALMaxDistance",
                    &settings.openALSpatialConfig.maxDistance,
                    0.01f,
                    1000.0f,
                    "%.4f");
                // rolloff 为非负衰减系数，零值表示不随距离衰减。
                addSpatialSlider(
                    TR_CACHE("ui.settings.software.openal_rolloff").data(),
                    "##OpenALRolloff",
                    &settings.openALSpatialConfig.rolloffFactor,
                    0.0f,
                    10.0f,
                    "%.4f");
            }
        }

        // 皮肤行由一个候选组合框和打开目录、导入、导出三个动作按钮组成。
        // 候选缓存只保存合法入口目录名；高成本选择流程由 applySkinSelection
        // 封装。
        if ( m_availableSkinDirectoriesDirty ) {
            // 扫描只在显式标脏后发生，完成后缓存供后续帧复用。
            refreshAvailableSkinDirectories();
        }
        // 活动皮肤名称按运行态、持久化值、默认目录的顺序解析。
        const std::string activeSkinDirectory =
            currentSkinDirectoryName(settings);
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.skin").data(),
            maxLabelW,
            [this, &changed, activeSkinDirectory](Clay_BoundingBox r, bool) {
                // 三个动作按钮固定宽度，剩余空间全部分给皮肤组合框。
                const ImGuiStyle& style         = ImGui::GetStyle();
                constexpr float   actionButtonW = 35.0f;
                const float       comboWidth =
                    std::max(1.0f,
                             r.width - actionButtonW * 3.0f -
                                 style.ItemSpacing.x * 3.0f);
                // std::max 保证极窄窗口下组合框仍有合法正宽度。
                ImGui::SetNextItemWidth(comboWidth);
                if ( m_availableSkinDirectories.empty() ) {
                    // 没有合法入口时展示禁用组合框，而不是空弹窗。
                    ImGui::BeginDisabled();
                    if ( ::MMM::UI::FeedbackBeginCombo(
                             "##SkinCombo",
                             TR_CACHE("ui.settings.software.skin.none")
                                 .data()) ) {
                        ::MMM::UI::FeedbackEndCombo();
                    }
                    ImGui::EndDisabled();
                } else if ( ::MMM::UI::FeedbackBeginCombo(
                                "##SkinCombo", activeSkinDirectory.c_str()) ) {
                    // 候选来自低频目录缓存，不在打开下拉框时再次扫描。
                    for ( const auto& directoryName :
                          m_availableSkinDirectories ) {
                        const bool selected =
                            directoryName == activeSkinDirectory;
                        if ( ::MMM::UI::FeedbackSelectable(
                                 directoryName.c_str(), selected) ) {
                            // 重选当前目录不执行高成本资源重载。
                            if ( directoryName != activeSkinDirectory &&
                                 applySkinSelection(
                                     directoryName,
                                     skinLuaPathForDirectory(directoryName)) ) {
                                // 切换成功才并入统一 AppConfig 保存路径。
                                changed = true;
                            }
                        }
                        if ( selected ) {
                            // 弹窗打开后键盘焦点定位到实际活动皮肤。
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }

                // 动作按钮与组合框保持同一行，并使用紧凑横向 padding。
                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(0.0f, style.FramePadding.y));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.5f, 0.5f));

                // 打开按钮仅访问 skins 根目录，不修改配置。
                ImGui::PushID("OpenSkinDirectory");
                if ( ::MMM::UI::FeedbackButton(ICON_MMM_FOLDER_OPEN,
                                               { actionButtonW, 0.0f }) ) {
                    openSkinDirectory();
                }
                Utils::renderTooltip(
                    TR_CACHE("ui.settings.software.skin.open_directory")
                        .data());
                ImGui::PopID();

                ImGui::SameLine();
                // 导入按钮打开文件选择器，成功导入可能切换并保存皮肤。
                ImGui::PushID("ImportSkinPackage");
                if ( ::MMM::UI::FeedbackButton(ICON_MMM_DOWNLOAD,
                                               { actionButtonW, 0.0f }) ) {
                    changed |= openSkinImportFilePicker();
                }
                Utils::renderTooltip(
                    TR_CACHE("ui.settings.software.skin.import").data());
                ImGui::PopID();

                ImGui::SameLine();
                // 导出按钮不修改设置，因此不并入 changed。
                ImGui::PushID("ExportSkinPackage");
                if ( ::MMM::UI::FeedbackButton(ICON_MMM_PACK,
                                               { actionButtonW, 0.0f }) ) {
                    openSkinExportFilePicker();
                }
                Utils::renderTooltip(
                    TR_CACHE("ui.settings.software.skin.export").data());
                ImGui::PopID();

                // 恢复三个动作按钮共享的 padding 与文字对齐样式。
                ImGui::PopStyleVar(2);
            });

        // 主题选择契约：
        // - Auto 始终作为第一个固定候选；
        // - 注册表主题使用稳定 id 持久化、displayName 展示；
        // - 未知持久化 id 原样显示，不静默覆盖；
        // - VKContext 缺失时保持只读式回退显示；
        // - 选择成功后立即 applyTheme 并统一保存。
        // UI 主题列表由当前 Vulkan 上下文的注册表提供，Auto 是固定入口。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.theme").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // VKContext 缺失时仍显示持久化主题 ID，但无法枚举或应用主题。
                auto context = Graphic::VKContext::get();
                const Graphic::ImGuiThemeRegistry* themeRegistry =
                    context ? &context->get().getThemeRegistry() : nullptr;
                const Graphic::ImGuiTheme* selectedTheme =
                    themeRegistry ? themeRegistry->findTheme(settings.theme)
                                  : nullptr;
                // 预览优先本地化 Auto，再用注册主题名称，最后保留未知 ID。
                const char* preview =
                    settings.theme == Config::UI_THEME_AUTO_ID
                        ? TR_CACHE("ui.settings.software.theme.auto").data()
                    : selectedTheme ? selectedTheme->displayName().data()
                                    : settings.theme.c_str();

                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo("##ThemeCombo", preview) ) {
                    // Auto 由平台或系统主题选择实际配色。
                    const bool automaticSelected =
                        settings.theme == Config::UI_THEME_AUTO_ID;
                    if ( ::MMM::UI::FeedbackSelectable(
                             TR_CACHE("ui.settings.software.theme.auto").data(),
                             automaticSelected) ) {
                        settings.theme = Config::UI_THEME_AUTO_ID;
                        if ( context ) {
                            // 上下文存在时立即应用，不等待设置页关闭。
                            context->get().applyTheme();
                        }
                        changed = true;
                    }
                    if ( automaticSelected ) {
                        ImGui::SetItemDefaultFocus();
                    }

                    if ( themeRegistry ) {
                        // 注册表可能包含空主题条目，枚举时显式跳过。
                        for ( const auto& theme : themeRegistry->themes() ) {
                            if ( !theme ) continue;
                            const bool selected = settings.theme == theme->id();
                            // 稳定主题 ID 隔离可能重复的可见 displayName。
                            ImGui::PushID(theme->id().data());
                            if ( ::MMM::UI::FeedbackSelectable(
                                     theme->displayName().data(), selected) ) {
                                settings.theme = theme->id();
                                // 当前 registry 来自有效
                                // context，可安全立即应用。
                                context->get().applyTheme();
                                changed = true;
                            }
                            ImGui::PopID();
                            if ( selected ) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
            });

        // 字体选择契约：
        // - ASCII 与 CJK 各自维护独立偏好和文件选择器 key；
        // - Default 是稳定保留值，不依赖皮肤资源列表；
        // - 皮肤字体保存 name，path 只用于消除 ImGui ID 冲突；
        // - 外部字体保存文件选择器返回的完整路径；
        // - 任一确认选择都只请求异步字体重建；
        // - 取消文件对话框不修改偏好；
        // - 文件选择器错误写日志但不抛出异常。
        //
        // ASCII 字体组合框生命周期：
        // - 每帧从 EditorSettings 解析当前可见选择；
        // - 空字符串兼容映射为 Default；
        // - 固定 Default 项始终位于皮肤候选之前；
        // - 皮肤候选来自 SkinManager 当前 SkinData；
        // - 选择候选只保存 name，不复制或打开字体文件；
        // - 浏览按钮依据 FilePickerStyle 打开一种对话框；
        // - 原生结果立即保存并释放 NFD 缓冲；
        // - 内置结果由布局后模态处理块消费；
        // - 确认后仅请求字体图集重建。
        // ASCII 字体负责拉丁字符与符号，可选择皮肤字体或外部文件。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.font.ascii").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 字体清单由当前皮肤提供，引用只在本帧使用。
                auto& skinMgr    = Config::SkinManager::instance();
                auto& asciiFonts = skinMgr.getAsciiFonts();
                // 空偏好兼容为 Default，可见标签使用本地化文本。
                std::string currentAscii = settings.preferredAsciiFont.empty()
                                               ? "Default"
                                               : settings.preferredAsciiFont;

                std::string label =
                    (currentAscii == "Default")
                        ? TR_CACHE("ui.settings.software.font.default").data()
                        : currentAscii;

                // 右侧浏览按钮固定宽度，其余区域留给字体组合框。
                const ImGuiStyle& style         = ImGui::GetStyle();
                const float       browseButtonW = 35.0f;
                const float       comboW        = std::max(
                    1.0f, r.width - browseButtonW - style.ItemSpacing.x);
                ImGui::SetNextItemWidth(comboW);
                if ( ::MMM::UI::FeedbackBeginCombo("##AsciiFontCombo",
                                                   label.c_str()) ) {
                    // 固定默认选项不依赖皮肤字体清单。
                    {
                        bool isSelected = (currentAscii == "Default");
                        if ( ::MMM::UI::FeedbackSelectable(
                                 TR_CACHE("ui.settings.software.font.default")
                                     .data(),
                                 isSelected) ) {
                            settings.preferredAsciiFont = "Default";
                            // 字体选择只发出安全帧边界重建请求。
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                        if ( isSelected ) ImGui::SetItemDefaultFocus();
                    }

                    // 皮肤字体按名称显示，并把资源路径附在隐藏 ID
                    // 中消除重名冲突。
                    for ( const auto& [name, path] : asciiFonts ) {
                        bool        isSelected = (currentAscii == name);
                        std::string lbl =
                            name + "##" + Config::pathToUtf8(path);
                        if ( ::MMM::UI::FeedbackSelectable(lbl.c_str(),
                                                           isSelected) ) {
                            settings.preferredAsciiFont = name;
                            // 只保存皮肤内稳定名称，不保存展开后的资源路径。
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                // 浏览按钮与组合框同排，使用紧凑样式避免省略号被裁剪。
                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(0.0f, style.FramePadding.y));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.5f, 0.5f));
                const bool browseAsciiClicked = ::MMM::UI::FeedbackButton(
                    "...##BrowseAscii", { browseButtonW, 0 });
                ImGui::PopStyleVar(2);
                if ( browseAsciiClicked ) {
                    // 文件选择器实现由软件偏好决定，两个分支使用相同扩展名白名单。
                    if ( settings.filePickerStyle ==
                         Config::FilePickerStyle::Native ) {
                        // 原生对话框打开前播放统一弹窗反馈。
                        ::MMM::UI::PlayPopupOpenFeedback();
                        nfdu8char_t*      outPath    = nullptr;
                        nfdu8filteritem_t filters[1] = { { "Font Files",
                                                           "ttf,otf" } };
                        nfdresult_t       result = NativeFileDialog::openFile(
                            &outPath, filters, 1, nullptr);

                        if ( result == NFD_OKAY ) {
                            // 外部字体按用户选择路径保存，并请求字体图集重建。
                            settings.preferredAsciiFont = outPath;
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                            // NFD 返回缓冲由库分配，成功使用后必须释放。
                            NFD_FreePathU8(outPath);
                        } else if ( result == NFD_ERROR ) {
                            // 错误只记录日志，取消选择不视为失败或修改。
                            XERROR("NFD Error: {}", NFD_GetError());
                        }
                    } else {
                        // 内置 ImGui 对话框使用只读文件名栏和字体扩展名过滤。
                        IGFD::FileDialogConfig config;
                        config.path     = ".";
                        config.fileName = "";
                        config.flags =
                            ImGuiFileDialogFlags_Modal |
                            ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_ReadOnlyFileNameField;
                        // 只在从关闭变为打开时播放弹窗反馈，避免每帧重复声音。
                        const bool wasOpen =
                            ImGuiFileDialog::Instance()->IsOpened(
                                "AsciiFontPicker");
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "AsciiFontPicker",
                            TR_CACHE("ui.settings.software.font.browse").data(),
                            ".ttf,.otf",
                            config);
                        if ( !wasOpen && ImGuiFileDialog::Instance()->IsOpened(
                                             "AsciiFontPicker") ) {
                            ::MMM::UI::PlayPopupOpenFeedback();
                        }
                    }
                }
            });

        // CJK 字体组合框生命周期：
        // - 使用独立 preferredCjkFont 字段；
        // - 空字符串同样兼容映射为 Default；
        // - 候选来自 SkinManager 的 CJK 字体清单；
        // - 名称与路径共同构造唯一 ImGui 项；
        // - 浏览按钮使用 CjkFontPicker 独立对话框 key；
        // - 与 ASCII 选择器可以分别保持打开状态检查；
        // - 成功选择不会覆盖 ASCII 字体偏好；
        // - 字体重建请求由 VKContext 在安全阶段执行；
        // - 取消对话框保持当前偏好不变。
        // CJK 字体负责中日韩字形，流程与 ASCII 字体独立以支持字体回退组合。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.font.cjk").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // CJK 字体清单由当前皮肤提供，选择名称可与 ASCII 清单不同。
                auto& skinMgr  = Config::SkinManager::instance();
                auto& cjkFonts = skinMgr.getCjkFonts();
                // 空偏好兼容为 Default，显示时使用本地化默认名称。
                std::string currentCjk = settings.preferredCjkFont.empty()
                                             ? "Default"
                                             : settings.preferredCjkFont;
                std::string label =
                    (currentCjk == "Default")
                        ? TR_CACHE("ui.settings.software.font.default").data()
                        : currentCjk;

                // 固定浏览按钮宽度后，把剩余值列交给组合框。
                const ImGuiStyle& style         = ImGui::GetStyle();
                const float       browseButtonW = 35.0f;
                const float       comboW        = std::max(
                    1.0f, r.width - browseButtonW - style.ItemSpacing.x);
                ImGui::SetNextItemWidth(comboW);
                if ( ::MMM::UI::FeedbackBeginCombo("##CjkFontCombo",
                                                   label.c_str()) ) {
                    // 默认选项交回字体系统自动选择 CJK 字体。
                    {
                        bool isSelected = (currentCjk == "Default");
                        if ( ::MMM::UI::FeedbackSelectable(
                                 TR_CACHE("ui.settings.software.font.default")
                                     .data(),
                                 isSelected) ) {
                            settings.preferredCjkFont = "Default";
                            // 重建请求延迟到图形安全点，不在回调中同步上传字体。
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                        if ( isSelected ) ImGui::SetItemDefaultFocus();
                    }

                    // 皮肤额外字体使用名称显示、路径参与隐藏 ID。
                    for ( const auto& [name, path] : cjkFonts ) {
                        bool        isSelected = (currentCjk == name);
                        std::string lbl =
                            name + "##" + Config::pathToUtf8(path);
                        if ( ::MMM::UI::FeedbackSelectable(lbl.c_str(),
                                                           isSelected) ) {
                            settings.preferredCjkFont = name;
                            // 保存稳定皮肤名称并请求字体图集重建。
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                // 浏览按钮采用与 ASCII 行一致的紧凑省略号样式。
                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(0.0f, style.FramePadding.y));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.5f, 0.5f));
                const bool browseCjkClicked = ::MMM::UI::FeedbackButton(
                    "...##BrowseCjk", { browseButtonW, 0 });
                ImGui::PopStyleVar(2);
                if ( browseCjkClicked ) {
                    // 原生与内置文件选择器都只接受 ttf/otf 字体文件。
                    if ( settings.filePickerStyle ==
                         Config::FilePickerStyle::Native ) {
                        // 原生对话框打开动作播放统一弹窗音效。
                        ::MMM::UI::PlayPopupOpenFeedback();
                        nfdu8char_t*      outPath    = nullptr;
                        nfdu8filteritem_t filters[1] = { { "Font Files",
                                                           "ttf,otf" } };
                        nfdresult_t       result = NativeFileDialog::openFile(
                            &outPath, filters, 1, nullptr);

                        if ( result == NFD_OKAY ) {
                            // 外部 CJK 字体保存选择路径，并安排字体图集重建。
                            settings.preferredCjkFont = outPath;
                            if ( auto ctx = Graphic::VKContext::get() )
                                ctx->get().requestFontRebuild();
                            changed = true;
                            // 成功结果路径由 NFD 分配，使用后释放。
                            NFD_FreePathU8(outPath);
                        } else if ( result == NFD_ERROR ) {
                            // 取消不记录错误，真正错误写入项目日志。
                            XERROR("NFD Error: {}", NFD_GetError());
                        }
                    } else {
                        // 内置对话框使用独立 key，避免与 ASCII 选择器状态冲突。
                        IGFD::FileDialogConfig config;
                        config.path     = ".";
                        config.fileName = "";
                        config.flags =
                            ImGuiFileDialogFlags_Modal |
                            ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_ReadOnlyFileNameField;
                        // 记录打开前状态，使反馈音只在状态边沿播放一次。
                        const bool wasOpen =
                            ImGuiFileDialog::Instance()->IsOpened(
                                "CjkFontPicker");
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "CjkFontPicker",
                            TR_CACHE("ui.settings.software.font.browse").data(),
                            ".ttf,.otf",
                            config);
                        if ( !wasOpen && ImGuiFileDialog::Instance()->IsOpened(
                                             "CjkFontPicker") ) {
                            ::MMM::UI::PlayPopupOpenFeedback();
                        }
                    }
                }
            });

        // 界面全局缩放影响主题尺寸，拖动结束后再应用以避免每帧重算样式。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.ui_scale.multiplier").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 静态临时值在控件活动期间保持连续拖动状态。
                static float tmpUIScale = settings.uiScaleMultiplier;
                ImGui::SetNextItemWidth(r.width);
                ::MMM::UI::FeedbackSliderFloat(
                    "##UIScale", &tmpUIScale, 0.5f, 2.0f, "%.4f");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 释放控件时把最终值写入配置并重新应用缩放主题。
                    settings.uiScaleMultiplier = tmpUIScale;
                    changed                    = true;
                    if ( auto ctx = Graphic::VKContext::get() ) {
                        // 主题应用更新运行态尺寸，通知提示部分资源需重启完成。
                        ctx->get().applyTheme();
                        ctx->get().showCenterNotification(
                            TR_CACHE("ui.settings.software.font.restart")
                                .data());
                    }
                } else if ( !ImGui::IsItemActive() ) {
                    // 非活动帧从配置刷新临时值，接受其他入口的外部修改。
                    tmpUIScale = settings.uiScaleMultiplier;
                }
            });

        // 字体大小倍率独立于 UI 几何缩放，提交后提示重启完成字体变化。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.font.multiplier").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 静态临时值避免拖动过程中反复写配置和触发通知。
                static float tmpFontScale = settings.fontSizeMultiplier;
                ImGui::SetNextItemWidth(r.width);
                ::MMM::UI::FeedbackSliderFloat(
                    "##FontScale", &tmpFontScale, 0.5f, 2.0f, "%.4f");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 只在编辑结束边沿提交最终倍率。
                    settings.fontSizeMultiplier = tmpFontScale;
                    changed                     = true;
                    if ( auto ctx = Graphic::VKContext::get() ) {
                        // 当前流程不立即重建字体，中心通知明确需要重启。
                        ctx->get().showCenterNotification(
                            TR_CACHE("ui.settings.software.font.restart")
                                .data());
                    }
                } else if ( !ImGui::IsItemActive() ) {
                    // 控件空闲时同步可能来自配置重载的实际值。
                    tmpFontScale = settings.fontSizeMultiplier;
                }
            });

        // 内置 ASCII 文件选择器需要作为独立模态窗口在设置控件之后处理结果。
        {
            // 模态样式和居中位置按当前 DPI 统一管理。
            const float dpiScale =
                Config::AppConfig::instance().getWindowContentScale();
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("AsciiFontPicker") ) {
                // 只在对话框已打开时准备固定居中尺寸。
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "AsciiFontPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    // 确认后保存完整文件路径，并安排字体图集重建。
                    settings.preferredAsciiFont =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().requestFontRebuild();
                    changed = true;
                }
                // 确认和取消都关闭实例，避免下一帧重复处理结果。
                ImGuiFileDialog::Instance()->Close();
            }
        }

        // CJK 对话框使用独立 key，但遵循相同模态结果生命周期。
        {
            const float dpiScale =
                Config::AppConfig::instance().getWindowContentScale();
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("CjkFontPicker") ) {
                // 仅在实际打开时覆盖下一窗口居中尺寸。
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "CjkFontPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    // 确认路径写入 CJK 偏好，与 ASCII 字段完全分离。
                    settings.preferredCjkFont =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().requestFontRebuild();
                    changed = true;
                }
                // 消费本次结果后立即关闭模态实例。
                ImGuiFileDialog::Instance()->Close();
            }
        }
    }

    // 光标配置契约：
    // - CursorStyle::System 把指针绘制交给操作系统；
    // - CursorStyle::Software 启用自定义光标、轨迹与烟雾；
    // - cursorSize、trailSize、smokeSize 使用逻辑像素；
    // - trailLifeTime 和 smokeLifeTime 使用秒；
    // - enableBpmSyncSmokeLife 让烟雾寿命改由音乐节拍推导；
    // - BPM 同步启用时固定 smokeLifeTime 仍保留但不可编辑；
    // - 所有字段只改变后续帧绘制，不创建或销毁持久资源。
    // 光标组选择系统光标或软件绘制光标，并展示后者的粒子参数。
    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.software.cursor_params").data(), true) ) {

        // 采用统一标签宽度，使动态软件光标参数保持对齐。

        // CursorStyle 枚举通过通用单选 helper 在 Software/System 间切换。
        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.editor.cursor_style").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.editor.cursor_software").data(),
                (int)Config::CursorStyle::Software },
              { TR_CACHE("ui.settings.editor.cursor_system").data(),
                (int)Config::CursorStyle::System } },
            (int&)settings.cursorStyle,
            changed);

        if ( settings.cursorStyle == Config::CursorStyle::Software ) {
            // 软件光标参数仅在对应模式可见，切换到系统光标时保留配置。
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.cursor_size").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               // 主光标直径以屏幕像素配置。
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##CursorSize",
                                   &settings.softwareCursorConfig.cursorSize,
                                   4.0f,
                                   512.0f,
                                   "%.4f px");
                           });
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.trail_size").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               // 轨迹粒子尺寸独立于主光标尺寸。
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##TrailSize",
                                   &settings.softwareCursorConfig.trailSize,
                                   4.0f,
                                   512.0f,
                                   "%.4f px");
                           });
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.trail_life").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               // 轨迹生命周期以秒保存，决定残影持续时间。
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##TrailLife",
                                   &settings.softwareCursorConfig.trailLifeTime,
                                   0.05f,
                                   5.0f,
                                   "%.4f s");
                           });
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.smoke_size").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               // 烟雾粒子尺寸使用像素单位。
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##SmokeSize",
                                   &settings.softwareCursorConfig.smokeSize,
                                   4.0f,
                                   512.0f,
                                   "%.4f px");
                           });
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.software.cursor_bpm_sync").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    // BPM 同步让烟雾寿命由音乐节拍驱动，而非固定秒数。
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##BpmSync",
                        &settings.softwareCursorConfig.enableBpmSyncSmokeLife);
                });
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.software.smoke_life").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    // BPM 同步启用时保留固定寿命值但禁止编辑，供关闭后恢复。
                    if ( settings.softwareCursorConfig.enableBpmSyncSmokeLife )
                        ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ::MMM::UI::FeedbackSliderFloat(
                        "##SmokeLife",
                        &settings.softwareCursorConfig.smokeLifeTime,
                        0.05f,
                        10.0f,
                        "%.4f s");
                    // 禁用作用域仅覆盖当前滑块。
                    if ( settings.softwareCursorConfig.enableBpmSyncSmokeLife )
                        ImGui::EndDisabled();
                });
        }
    }

    // 外观配置契约：
    // - windowRounding 作用于独立窗口轮廓；
    // - frameRounding 作用于按钮、输入框等 Frame；
    // - windowGap 控制窗口与布局区域之间的间隙；
    // - itemSpacing 控制相邻 ImGui Item 间距；
    // - windowPadding 控制窗口内容内边距并影响最小尺寸缓存；
    // - animationTransitionDuration 控制 UI 动画过渡时间；
    // - 五个几何字段使用逻辑像素，动画字段使用秒；
    // - 需要主题刷新的字段只在拖动结束后 applyTheme；
    // - 临时值在控件空闲时始终回读实际配置。
    // 外观组编辑窗口、控件、间距和动画参数；高成本主题应用延迟到拖动结束。
    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.software.aesthetics").data(), true) ) {
        // 采用统一标签宽度，让所有像素滑块和持续时间控件对齐。

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.window_rounding").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 静态临时值承接窗口圆角的连续拖动，避免每帧应用主题。
                static float tmpRounding = settings.aesthetics.windowRounding;
                ImGui::SetNextItemWidth(r.width);
                ::MMM::UI::FeedbackSliderFloat(
                    "##WinRounding", &tmpRounding, 0.0f, 32.0f, "%.4f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 释放滑块后提交最终像素值并刷新运行态主题。
                    settings.aesthetics.windowRounding = tmpRounding;
                    changed                            = true;
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().applyTheme();
                } else if ( !ImGui::IsItemActive() ) {
                    // 控件空闲时同步外部配置变化。
                    tmpRounding = settings.aesthetics.windowRounding;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.frame_rounding").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // frameRounding 作用于按钮、输入框等 frame，不等同窗口圆角。
                static float tmpFrame = settings.aesthetics.frameRounding;
                ImGui::SetNextItemWidth(r.width);
                ::MMM::UI::FeedbackSliderFloat(
                    "##FrameRounding", &tmpFrame, 0.0f, 32.0f, "%.4f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 最终值提交后立即应用主题，使控件圆角可见。
                    settings.aesthetics.frameRounding = tmpFrame;
                    changed                           = true;
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().applyTheme();
                } else if ( !ImGui::IsItemActive() ) {
                    // 非活动帧以持久化内存值校正临时滑块。
                    tmpFrame = settings.aesthetics.frameRounding;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.window_gap").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // windowGap 控制窗口间布局距离，不要求调用 applyTheme。
                static float tmpGap = settings.aesthetics.windowGap;
                // 约束下一弹窗/控件宽度与 Clay 值列保持一致。
                ImGui::SetNextWindowSizeConstraints(ImVec2(r.width, -1),
                                                    ImVec2(r.width, -1));
                ImGui::SetNextItemWidth(r.width);
                ::MMM::UI::FeedbackSliderFloat(
                    "##WinGap", &tmpGap, 0.0f, 32.0f, "%.4f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 拖动完成后保存最终间隔值。
                    settings.aesthetics.windowGap = tmpGap;
                    changed                       = true;
                } else if ( !ImGui::IsItemActive() ) {
                    // 控件空闲时接受外部更新。
                    tmpGap = settings.aesthetics.windowGap;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.item_spacing").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // itemSpacing 会影响大量 ImGui 元素，延迟到释放后统一应用。
                static float tmpSpacing = settings.aesthetics.itemSpacing;
                ImGui::SetNextItemWidth(r.width);
                ::MMM::UI::FeedbackSliderFloat(
                    "##ItemSpacing", &tmpSpacing, 0.0f, 32.0f, "%.4f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 保存并应用主题，让后续帧使用新的控件间距。
                    settings.aesthetics.itemSpacing = tmpSpacing;
                    changed                         = true;
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().applyTheme();
                } else if ( !ImGui::IsItemActive() ) {
                    // 非拖动帧重新对齐实际设置。
                    tmpSpacing = settings.aesthetics.itemSpacing;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.window_padding").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // windowPadding 同时影响最小窗口尺寸缓存和实际主题布局。
                static float tmpPadding = settings.aesthetics.windowPadding;
                ImGui::SetNextItemWidth(r.width);
                ::MMM::UI::FeedbackSliderFloat(
                    "##WinPadding", &tmpPadding, 0.0f, 32.0f, "%.4f px");
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 释放后提交并应用主题，缓存键会在下一帧检测变化。
                    settings.aesthetics.windowPadding = tmpPadding;
                    changed                           = true;
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().applyTheme();
                } else if ( !ImGui::IsItemActive() ) {
                    // 空闲时从配置刷新，防止静态临时值跨重载陈旧。
                    tmpPadding = settings.aesthetics.windowPadding;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.aesthetics.animation_transition")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 动画过渡时长使用秒，临时值支持连续精细拖动。
                static float tmpDuration =
                    settings.aesthetics.animationTransitionDuration;
                // 最大一秒限制交互响应不会因配置过大而显著滞后。
                constexpr float maxDuration = 1.0f;
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat(
                         "##AnimationTransitionDuration",
                         &tmpDuration,
                         0.005f,
                         Config::UIAestheticsConfig::
                             MIN_ANIMATION_TRANSITION_DURATION,
                         maxDuration,
                         "%.2f s",
                         ImGuiSliderFlags_AlwaysClamp) ) {
                    // 文本输入等路径同样显式钳制到配置最小值和页面最大值。
                    tmpDuration =
                        std::clamp(tmpDuration,
                                   Config::UIAestheticsConfig::
                                       MIN_ANIMATION_TRANSITION_DURATION,
                                   maxDuration);
                    settings.aesthetics.animationTransitionDuration =
                        // 拖动期间更新内存值，让动画预览可即时响应。
                        tmpDuration;
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 交互结束时标记持久化修改。
                    settings.aesthetics.animationTransitionDuration =
                        tmpDuration;
                    changed = true;
                } else if ( !ImGui::IsItemActive() ) {
                    // 未编辑时同步外部设置值。
                    tmpDuration =
                        settings.aesthetics.animationTransitionDuration;
                }
            });
    }

    // 偏好与同步配置契约：
    // - FilePickerStyle 只影响未来打开的文件对话框实现；
    // - SaveFormatPreference 只影响未来保存的输出格式；
    // - AutoSaveConfig 保存当前文档，不维护轮换副本；
    // - autoBackup 使用相同模式枚举并额外限制 maxBackupCount；
    // - 两种自动任务都支持定时或四类事件触发；
    // - TimeFormatPreference 只影响时间文本显示；
    // - recentProjectsLimit 限制历史列表容量，不删除工程；
    // - SyncMode 决定显示积分因子或 WaterTank 缓冲参数；
    // - syncInterval 使用 double 秒值并对所有模式保留。
    // 偏好与同步组包含文件选择器、保存格式、自动保存/备份和联机同步参数。
    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.software.sync").data(), true) ) {

        // 采用统一标签宽度，让动态模式参数不会改变值列起点。

        // 文件选择器偏好决定后续打开操作使用 ImGui 统一界面还是系统原生对话框。
        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.software.picker_style").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.software.picker_unified").data(),
                (int)Config::FilePickerStyle::Unified },
              { TR_CACHE("ui.settings.software.picker_native").data(),
                (int)Config::FilePickerStyle::Native } },
            (int&)settings.filePickerStyle,
            changed);

        // 保存格式偏好只作用于后续保存；不会立即转换当前已打开的谱面文件。
        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.software.save_format").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.software.save_format.original").data(),
                (int)Config::SaveFormatPreference::Original },
              { TR_CACHE("ui.settings.software.save_format.force_mmm").data(),
                (int)Config::SaveFormatPreference::ForceMMM } },
            (int&)settings.saveFormatPreference,
            changed);

        // 自动保存使用独立子组，模式行与条件参数共享装饰边界。
        // 自动保存模式状态机：
        //
        // Disabled：
        // - 不展示间隔或触发器控件；
        // - 保留 intervalUnit、intervalValue 和全部触发器值；
        // - 不代表关闭手动保存；
        // - 重新启用时恢复此前模式参数。
        //
        // Timed：
        // - 展示 Seconds/Minutes 单位单选；
        // - 展示 5 至 60 的整数间隔；
        // - 隐藏但保留事件触发器；
        // - 计时与实际保存由逻辑服务负责。
        //
        // EventTriggered：
        // - 展示对象修改、换谱、ImGui 失焦和原生窗口失焦；
        // - 四项可以任意组合，包括全部关闭；
        // - 隐藏但保留定时间隔；
        // - UI 不合并、消抖或执行实际保存操作。
        auto& autoSaveGroup =
            addSettingGroup(*sec, sectionIndex, "AutoSaveSettingsGroup");
        // Disabled、Timed 与 EventTriggered 三种模式互斥，枚举值显式映射。
        addRadioSetting(
            autoSaveGroup,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.software.auto_save.mode").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.software.auto_save.mode.disabled").data(),
                (int)Config::AutoSaveMode::Disabled },
              { TR_CACHE("ui.settings.software.auto_save.mode.timed").data(),
                (int)Config::AutoSaveMode::Timed },
              { TR_CACHE("ui.settings.software.auto_save.mode.event").data(),
                (int)Config::AutoSaveMode::EventTriggered } },
            (int&)settings.autoSave.mode,
            changed,
            false);

        if ( settings.autoSave.mode == Config::AutoSaveMode::Timed ) {
            // 定时模式通过秒/分钟单位与整数间隔共同表达周期。
            addRadioSetting(
                autoSaveGroup,
                rowIndex,
                sectionIndex,
                TR_CACHE("ui.settings.software.auto_save.interval_unit").data(),
                maxLabelW,
                { { TR_CACHE(
                        "ui.settings.software.auto_save.interval_unit.seconds")
                        .data(),
                    (int)Config::AutoSaveIntervalUnit::Seconds },
                  { TR_CACHE(
                        "ui.settings.software.auto_save.interval_unit.minutes")
                        .data(),
                    (int)Config::AutoSaveIntervalUnit::Minutes } },
                (int&)settings.autoSave.intervalUnit,
                changed,
                false);
            // 间隔值范围 5 至 60，实际时间尺度由 intervalUnit 决定。
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.interval").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    // 滑块占满子组值列，修改直接并入 changed。
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ::MMM::UI::FeedbackSliderInt(
                        "##AutoSaveInterval",
                        &settings.autoSave.intervalValue,
                        5,
                        60);
                },
                false,
                false);
        } else if ( settings.autoSave.mode ==
                    Config::AutoSaveMode::EventTriggered ) {
            // 事件模式允许任意组合多个触发器，不强制至少启用一个。
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.on_object_modified")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    // 对象修改触发由编辑命令层判断，本页只保存许可开关。
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoSaveObjectModified",
                        &settings.autoSave.onObjectModified);
                },
                false,
                false);
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.on_beatmap_switch")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    // 换谱触发在离开当前谱面前执行保存。
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoSaveBeatmapSwitch",
                        &settings.autoSave.onBeatmapSwitch);
                },
                false,
                false);
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.on_imgui_focus_lost")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    // ImGui 窗口失焦与原生应用失焦属于不同事件源。
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoSaveImGuiFocusLost",
                        &settings.autoSave.onImGuiWindowFocusLost);
                },
                false,
                false);
            addSettingItem(
                autoSaveGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_save.on_native_focus_lost")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    // 原生窗口失焦覆盖切换到其他桌面应用的场景。
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoSaveNativeFocusLost",
                        &settings.autoSave.onNativeWindowFocusLost);
                },
                false,
                false);
        }

        // 自动备份与自动保存使用相同模式结构，但产生独立副本并管理保留数量。
        // 自动备份模式状态机：
        //
        // Disabled：
        // - 不生成新备份副本；
        // - 隐藏间隔、触发器和最大保留数量；
        // - 保留全部隐藏值供重新启用；
        // - 不影响普通自动保存状态。
        //
        // Timed：
        // - 单位和间隔范围与自动保存保持一致；
        // - 生成独立备份而不是覆盖当前谱面文件；
        // - 展示最大保留数量；
        // - 轮换和删除由备份服务完成。
        //
        // EventTriggered：
        // - 四类触发器与自动保存字段彼此独立；
        // - 同一事件可同时触发保存和备份；
        // - 展示最大保留数量；
        // - 本页只持久化策略，不创建文件。
        auto& autoBackupGroup =
            addSettingGroup(*sec, sectionIndex, "AutoBackupSettingsGroup");
        // 模式枚举与自动保存一致，但字段位于独立 autoBackup 配置中。
        addRadioSetting(
            autoBackupGroup,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.software.auto_backup.mode").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.software.auto_backup.mode.disabled")
                    .data(),
                (int)Config::AutoSaveMode::Disabled },
              { TR_CACHE("ui.settings.software.auto_backup.mode.timed").data(),
                (int)Config::AutoSaveMode::Timed },
              { TR_CACHE("ui.settings.software.auto_backup.mode.event").data(),
                (int)Config::AutoSaveMode::EventTriggered } },
            (int&)settings.autoBackup.mode,
            changed,
            false);

        if ( settings.autoBackup.mode == Config::AutoSaveMode::Timed ) {
            // 定时备份同样以单位和间隔值组合周期。
            addRadioSetting(
                autoBackupGroup,
                rowIndex,
                sectionIndex,
                TR_CACHE("ui.settings.software.auto_backup.interval_unit")
                    .data(),
                maxLabelW,
                { { TR_CACHE("ui.settings.software.auto_backup.interval_unit."
                             "seconds")
                        .data(),
                    (int)Config::AutoSaveIntervalUnit::Seconds },
                  { TR_CACHE("ui.settings.software.auto_backup.interval_unit."
                             "minutes")
                        .data(),
                    (int)Config::AutoSaveIntervalUnit::Minutes } },
                (int&)settings.autoBackup.intervalUnit,
                changed,
                false);
            // 隐藏事件触发字段时保留其值，来回切换模式不会丢失偏好。
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.interval").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    // 间隔范围与自动保存保持一致，便于理解和维护。
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ::MMM::UI::FeedbackSliderInt(
                        "##AutoBackupInterval",
                        &settings.autoBackup.intervalValue,
                        5,
                        60);
                },
                false,
                false);
        } else if ( settings.autoBackup.mode ==
                    Config::AutoSaveMode::EventTriggered ) {
            // 事件备份允许四种触发条件独立组合。
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.on_object_modified")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    // 对象修改可生成备份而不改变普通自动保存策略。
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoBackupObjectModified",
                        &settings.autoBackup.onObjectModified);
                },
                false,
                false);
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.on_beatmap_switch")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    // 换谱备份在会话切换边界生成独立副本。
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoBackupBeatmapSwitch",
                        &settings.autoBackup.onBeatmapSwitch);
                },
                false,
                false);
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.on_imgui_focus_lost")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    // ImGui 子窗口失焦触发与系统窗口焦点分开配置。
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoBackupImGuiFocusLost",
                        &settings.autoBackup.onImGuiWindowFocusLost);
                },
                false,
                false);
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE(
                    "ui.settings.software.auto_backup.on_native_focus_lost")
                    .data(),
                maxLabelW,
                [&](Clay_BoundingBox, bool) {
                    // 原生焦点丢失覆盖应用级切换场景。
                    changed |= ::MMM::UI::FeedbackCheckbox(
                        "##AutoBackupNativeFocusLost",
                        &settings.autoBackup.onNativeWindowFocusLost);
                },
                false,
                false);
        }

        if ( settings.autoBackup.mode != Config::AutoSaveMode::Disabled ) {
            // 只有会产生备份的模式才展示保留数量，禁用时仍保留已有值。
            addSettingItem(
                autoBackupGroup,
                rowIndex,
                TR_CACHE("ui.settings.software.auto_backup.max_count").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    // 上下限来自公共 Config 常量，避免 UI 与备份服务约束漂移。
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ::MMM::UI::FeedbackSliderInt(
                        "##AutoBackupMaxCount",
                        &settings.autoBackup.maxBackupCount,
                        Config::AUTO_BACKUP_COUNT_MIN,
                        Config::AUTO_BACKUP_COUNT_MAX);
                },
                false,
                false);
        }

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.time_format").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 时间格式使用局部整数映射四种稳定显示偏好。
                int         timeFormat    = (int)settings.timeFormatPreference;
                const char* timeFormats[] = {
                    TR_CACHE("ui.settings.software.time_format.clock").data(),
                    TR_CACHE("ui.settings.software.time_format.seconds").data(),
                    TR_CACHE("ui.settings.software.time_format.milliseconds")
                        .data(),
                    TR_CACHE("ui.settings.software.time_format.beat").data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackCombo("##TimeFormat",
                                              &timeFormat,
                                              timeFormats,
                                              IM_ARRAYSIZE(timeFormats)) ) {
                    // 格式变化只影响后续时间文本呈现，不修改底层时间值。
                    settings.timeFormatPreference =
                        (Config::TimeFormatPreference)timeFormat;
                    changed = true;
                }
            });

        // 最近项目上限控制持久化历史容量，不会删除磁盘上的工程。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.recent_limit").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 一至五十保证列表至少保留一个入口且避免无限增长。
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackSliderInt("##RecentLimit",
                                                  &settings.recentProjectsLimit,
                                                  1,
                                                  50) ) {
                    changed = true;
                }
            });

        // 同步配置契约：
        // - None 关闭校正算法，但继续保留参数；
        // - Integral 使用零至一的积分权重；
        // - WaterTank 使用零至 0.5 秒的缓冲目标；
        // - syncInterval 为 double 秒值；
        // - 模式切换不等待远端确认；
        // - 页面不创建网络连接或重启协作会话；
        // - 完整配置通过 CmdUpdateEditorConfig 交给消费者；
        // - 消费者应以最新配置覆盖旧值。
        // 联机同步模式选择关闭、积分校正或 WaterTank 缓冲算法。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.software.sync_mode").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 组合框索引与 SyncMode 枚举值显式对应。
                int         syncMode    = (int)settings.syncConfig.mode;
                const char* syncModes[] = {
                    TR_CACHE("ui.settings.software.sync_mode.none").data(),
                    TR_CACHE("ui.settings.software.sync_mode.integral").data(),
                    TR_CACHE("ui.settings.software.sync_mode.watertank").data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackCombo("##SyncMode",
                                              &syncMode,
                                              syncModes,
                                              IM_ARRAYSIZE(syncModes)) ) {
                    // 切换只更新配置，网络消费者通过最终 EditorConfig
                    // 命令应用。
                    settings.syncConfig.mode = (Config::SyncMode)syncMode;
                    changed                  = true;
                }
            });

        if ( settings.syncConfig.mode == Config::SyncMode::Integral ) {
            // 积分模式只展示零至一的校正系数。
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.sync_factor").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               // 系数是无量纲权重，由同步算法解释。
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##IntegralFactor",
                                   &settings.syncConfig.integralFactor,
                                   0.0f,
                                   1.0f);
                           });
        } else if ( settings.syncConfig.mode == Config::SyncMode::WaterTank ) {
            // WaterTank 模式展示以秒为语义的缓冲目标。
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.software.sync_buffer").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               // 半秒上限限制同步缓冲不会显著拖慢交互反馈。
                               ImGui::SetNextItemWidth(r.width);
                               changed |= ::MMM::UI::FeedbackSliderFloat(
                                   "##WaterTankBuffer",
                                   &settings.syncConfig.waterTankBuffer,
                                   0.0f,
                                   0.5f);
                           });
        }

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.software.sync_interval").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // 同步间隔使用 double 存储，DragScalar 避免精度降为
                           // float。
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ::MMM::UI::FeedbackDragScalar(
                               "##SyncInterval",
                               ImGuiDataType_Double,
                               &settings.syncConfig.syncInterval,
                               0.1f,
                               nullptr,
                               nullptr,
                               "%.1f s");
                       });
    }

    // 所有展开分组登记完成后统一执行 Clay 布局与 ImGui 回调。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    // 根布局使用当前剩余宽度，高度由标题和设置行自动计算。
    ImVec2 sz = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    // 推进游标，使父窗口正确计算内容高度与滚动范围。
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    // 皮肤导入/导出模态在根布局后渲染，成功导入可标记配置变化。
    changed |= renderSkinPackageFileDialogs(
        Config::AppConfig::instance().getWindowContentScale());

    if ( changed ) {
        // 一帧内全部普通设置变化合并为一次逻辑命令和一次磁盘保存。
        // 即时副作用已经执行，完整配置命令保证其他消费者最终一致。
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        // AppConfig 负责序列化路径和失败日志，本页不直接写文件。
        Config::AppConfig::instance().save();
    }
}

}  // namespace MMM::UI
