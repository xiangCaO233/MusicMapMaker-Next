#include "game/EditorApplicationService.h"

#include "audio/AudioManager.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"

namespace MMM::Game
{

/// @brief 复制当前项目供 UI 展示和恢复所需的只读快照。
/// @param snapshot 成功时接收项目根目录、工作区和音频资源列表。
/// @return 当前存在项目时返回 true，否则保持输出对象不变并返回 false。
///
/// 快照按值隔离项目生命周期，UI 不持有 Project 指针或内部容器引用。
bool EditorApplicationService::currentProjectUiSnapshot(
    UI::EditorProjectUiSnapshot& snapshot) const
{
    const auto* project = Logic::ProjectController::instance().currentProject();
    if ( !project ) {
        // 无项目是启动页和项目关闭后的正常状态，不记录错误。
        return false;
    }

    // 三个字段来自同一项目观察点，保证工作区和资源列表身份一致。
    snapshot.projectRoot    = project->m_projectRoot;
    snapshot.workspace      = project->m_settings.m_workspace;
    snapshot.audioResources = project->m_audioResources;
    return true;
}

/// @brief 在项目身份仍匹配时返回当前工作区的可写入口。
/// @param expectedProjectRoot UI 快照记录的预期项目根目录。
/// @return 匹配时返回项目持有的工作区；项目缺失或已切换时返回 nullptr。
///
/// 路径先做词法规范化，允许等价的点段表示，同时不访问文件系统。
ProjectWorkspaceState* EditorApplicationService::mutableCurrentWorkspace(
    const std::filesystem::path& expectedProjectRoot)
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.lexically_normal() !=
                         expectedProjectRoot.lexically_normal() ) {
        // 身份保护防止异步 UI 回调把旧项目布局写入新打开项目。
        return nullptr;
    }
    return &project->m_settings.m_workspace;
}

/// @brief 标记当前项目的音频工具为打开并立即保存项目。
///
/// 无项目时安全无操作；保存由 EditorEngine 处理路径、序列化与错误反馈。
/// @warning 低频 UI 动作路径：会触发项目文件写入，不得每帧无条件调用。
void EditorApplicationService::markProjectAudioToolOpenAndSave()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project ) {
        // 项目可能在点击与回调执行之间关闭。
        return;
    }
    // 只修改工作区中的窗口打开标志，其他项目设置保持原值。
    project->m_settings.m_workspace.m_projectAudioToolOpen = true;
    engine.saveProject();
}

/// @brief 查询逻辑引擎当前编辑工具。
/// @return 本帧可供 UI 显示的 EditTool 快照。
/// @warning UI 热路径：工具栏每帧读取，不得在此增加锁等待或分配。
Logic::EditTool EditorApplicationService::currentTool() const
{
    return Logic::EditorEngine::instance().getCurrentTool();
}

/// @brief 获取逻辑引擎当前编辑器配置副本。
/// @return 隔离调用方修改的完整 EditorConfig 值。
/// @warning UI 热路径：多个设置视图可读取，禁止引入文件 I/O。
Config::EditorConfig EditorApplicationService::editorConfig() const
{
    return Logic::EditorEngine::instance().getEditorConfig();
}

/// @brief 把 UI 修改后的完整编辑器配置交给逻辑引擎。
/// @param config 已完成单字段或一组字段修改的配置快照。
///
/// 服务不直接保存 AppConfig，EditorEngine 负责运行时传播与既有持久化约定。
void EditorApplicationService::updateEditorConfig(
    const Config::EditorConfig& config)
{
    Logic::EditorEngine::instance().setEditorConfig(config);
}

/// @brief 查询当前播放状态。
/// @return 活动会话正在播放时返回 true。
/// @warning UI 热路径：工具栏每帧读取，只允许轻量状态查询。
bool EditorApplicationService::isPlaybackPlaying() const
{
    return Logic::EditorEngine::instance().isPlaybackPlaying();
}

/// @brief 查询活动会话是否正在执行框选交互。
/// @return 框选状态机处于活动阶段时返回 true。
/// @warning UI 热路径：用于快捷键隔离，不得遍历实体。
bool EditorApplicationService::isSelectingMarquee() const
{
    return Logic::EditorEngine::instance().isActiveSessionSelectingMarquee();
}

/// @brief 查询活动会话是否正在拖动音符。
/// @return 拖动状态机处于活动阶段时返回 true。
/// @warning UI 热路径：用于输入路由判定，只读取已有状态。
bool EditorApplicationService::isDraggingNote() const
{
    return Logic::EditorEngine::instance().isActiveSessionDraggingNote();
}

/// @brief 查询活动会话是否正在使用画笔绘制。
/// @return 画笔状态机处于活动阶段时返回 true。
/// @warning UI 热路径：不得在查询中创建命令或修改画笔。
bool EditorApplicationService::isDrawingBrush() const
{
    return Logic::EditorEngine::instance().isActiveSessionDrawingBrush();
}

/// @brief 把 UI 层自动保存原因映射为逻辑层触发类型并排队请求。
/// @param reason ImGui 窗口或原生窗口失焦原因。
///
/// 映射显式覆盖接口定义的两种来源，实际保存由活动会话低频流程完成。
void EditorApplicationService::requestAutoSave(UI::EditorAutoSaveReason reason)
{
    // UI 枚举与逻辑枚举职责分离，在适配层完成一一转换。
    const auto trigger =
        reason == UI::EditorAutoSaveReason::ImGuiWindowFocusLost
            ? Logic::AutoSaveTrigger::ImGuiWindowFocusLost
            : Logic::AutoSaveTrigger::NativeWindowFocusLost;
    Logic::EditorEngine::instance().requestAutoSaveForActiveSession(trigger);
}

/// @brief 向逻辑引擎发布最近测得的渲染帧率。
/// @param fps UI 渲染循环计算的每秒帧数。
/// @warning UI 热路径：只转发数值，消费者负责无阻塞保存快照。
void EditorApplicationService::publishRenderFps(float fps)
{
    Logic::EditorEngine::instance().publishRenderFps(fps);
}

/// @brief 确保指定音效轨道已注册并可立即由反馈系统使用。
/// @param trackId 项目音频资源 ID 或皮肤音效键。
/// @return 音效最终可用时返回 true。
///
/// 查找优先级为已加载缓存、当前项目 Effect 资源、当前皮肤音频。项目路径由
/// 根目录和 UTF-8 相对路径组合；皮肤回退沿用音频池现有增益配置。
/// @warning 低频资源路径：缓存未命中时可能注册和加载音频，不得每帧调用。
bool EditorApplicationService::ensureEffectAudioTrackLoaded(
    const std::string& trackId)
{
    auto& audio = Audio::AudioManager::instance();
    if ( audio.isSoundEffectLoaded(trackId) ) {
        // 已加载命中不读取项目或皮肤配置。
        return true;
    }

    const auto* project = Logic::ProjectController::instance().currentProject();
    if ( project ) {
        // 项目同 ID Effect 资源优先于皮肤，允许工程覆盖交互音效。
        for ( const auto& resource : project->m_audioResources ) {
            if ( resource.m_id != trackId ||
                 resource.m_type != AudioTrackType::Effect ) {
                // 同名非 Effect 音轨不能注册到 SFX 池。
                continue;
            }

            // 项目资源路径按项目根目录解析，不依赖进程当前工作目录。
            const auto absolutePath =
                project->m_projectRoot / Config::utf8ToPath(resource.m_path);
            audio.registerSoundEffect(
                trackId, Config::pathToUtf8(absolutePath), resource.m_config);
            // 注册后同步确认解码/缓存结果，失败不继续用同 ID 皮肤覆盖。
            return audio.ensureSoundEffectLoaded(trackId);
        }
    }

    // 项目没有匹配资源时查询当前皮肤的标准音效表。
    const auto& skinData = Config::SkinManager::instance().getData();
    if ( auto path = skinData.audioPaths.find(trackId);
         path != skinData.audioPaths.end() ) {
        // 皮肤路径已由 SkinLoader 解析，注册时保留该 ID 的现有池音量。
        audio.registerSoundEffect(trackId,
                                  Config::pathToUtf8(path->second),
                                  audio.getSFXPoolVolume(trackId));
    }
    // 未找到任何来源时由 AudioManager 返回 false，不记录重复 UI 错误。
    return audio.ensureSoundEffectLoaded(trackId);
}

}  // namespace MMM::Game
