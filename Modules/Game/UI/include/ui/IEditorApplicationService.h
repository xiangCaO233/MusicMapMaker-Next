#pragma once

#include "mmm/project/AudioResource.h"
#include "mmm/project/ProjectSettings.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MMM::UI
{

/// @brief UI 加载项目工作区所需的轻量应用快照。
struct EditorProjectUiSnapshot {
    /// @brief 项目根目录。
    std::filesystem::path projectRoot;

    /// @brief 项目工作区状态副本。
    ProjectWorkspaceState workspace;

    /// @brief 项目音频资源副本。
    std::vector<AudioResource> audioResources;
};

/// @brief UI 可提交的自动保存触发来源。
enum class EditorAutoSaveReason : std::uint8_t {
    ImGuiWindowFocusLost,
    NativeWindowFocusLost,
};

/// @brief 由 Game 组合根实现的编辑器应用端口，隔离 UIManager 与 Logic 单例。
class IEditorApplicationService
{
public:
    virtual ~IEditorApplicationService() = default;

    /// @brief 获取当前项目的 UI 工作区快照。
    /// @warning 低频项目生命周期路径：复制工作区和音频资源；不得在每帧稳定
    /// 路径调用。
    [[nodiscard]] virtual bool currentProjectUiSnapshot(
        EditorProjectUiSnapshot& snapshot) const = 0;

    /// @brief 使用 UI 捕获结果修改当前项目工作区。
    /// @warning UI 低频周期路径：返回对当前项目工作区的短期观察指针；仅允许在
    /// UI 线程立即写入，不得缓存到下一帧或跨线程持有。
    [[nodiscard]] virtual ProjectWorkspaceState* mutableCurrentWorkspace(
        const std::filesystem::path& expectedProjectRoot) = 0;

    /// @brief 标记项目音频工具已经打开并保存项目。
    virtual void markProjectAudioToolOpenAndSave() = 0;

    /// @brief 请求当前活动谱面自动保存。
    /// @warning UI 热路径低频分支：只设置 Logic 原子事件位，不执行文件 I/O。
    virtual void requestAutoSave(EditorAutoSaveReason reason) = 0;

    /// @brief 发布当前渲染 FPS。
    /// @warning UI 热路径：每帧调用一次；实现只能发布标量状态，不得分配、加锁
    /// 或执行文件 I/O。
    virtual void publishRenderFps(float fps) = 0;

    /// @brief 确保项目或皮肤中的指定音效已加载。
    /// @warning 低频显式交互路径：可能访问文件系统并等待单文件解码；不得在
    /// 每帧 UI 更新中调用。
    [[nodiscard]] virtual bool ensureEffectAudioTrackLoaded(
        const std::string& trackId) = 0;
};

}  // namespace MMM::UI
