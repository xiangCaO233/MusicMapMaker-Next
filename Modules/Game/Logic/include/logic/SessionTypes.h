#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/// @file SessionTypes.h
/// @brief 会话列表及其已发布快照的公共值类型，不包含注册表实现。
/// @details 保持已有快照的所有权和字段布局，锁与发布算法继续由注册表负责。

namespace MMM::Logic
{
class BeatmapSession;

/// @brief 多画布 Session 条目，绑定 Session 与其对应画布的 cameraId。
struct SessionEntry {
    /// @brief 逻辑会话。
    std::shared_ptr<BeatmapSession> session;

    /// @brief 该画布对应的唯一 cameraId，例如 "Canvas_0"。
    std::string cameraId;

    /// @brief 显示名称，例如谱面名或默认标签。
    std::string displayName;

    /// @brief 当前会话谱面文件的稳定路径键，用于阻止重复打开同一谱面。
    std::string beatmapPathKey;

    /// @brief 当前会话完整复合音频时间线的稳定指纹。
    std::string audioTimelineFingerprint;

    /// @brief 当前会话 Main 资源序列和起播位置的画布同步指纹。
    std::string mainAudioSyncFingerprint;

    /// @brief 是否为初始 Logo 占位画布，尚未加载谱面时为 true。
    bool isLogoPlaceholder{ false };

    /// @brief 是否应使用项目工作区 ini 中保存的停靠状态。
    bool restoreDockFromWorkspace{ false };

    /// @brief 主画布窗口当前是否在 ImGui 中可见。
    bool isCanvasVisible{ true };
};

/// @brief 逻辑线程使用的 Session 指针快照，保留原始列表索引用于匹配活跃项。
struct SessionSnapshotEntry {
    /// @brief Session 在注册表中的原始索引。
    int32_t index{ -1 };

    /// @brief 主画布稳定身份，供鼠标位置命令在锁外选择目标。
    std::string cameraId;

    /// @brief 逻辑会话共享引用，保证锁外 update 期间生命周期有效。
    std::shared_ptr<BeatmapSession> session;

    /// @brief 该 Session 对应主画布本帧是否可见。
    bool isCanvasVisible{ true };

    /// @brief 当前会话完整复合音频时间线指纹。
    std::string audioTimelineFingerprint;

    /// @brief 当前会话 Main 资源序列和起播位置的画布同步指纹。
    std::string mainAudioSyncFingerprint;

    /// @brief 该条目是否为 Logo 占位画布。
    bool isLogoPlaceholder{ false };
};

/// @brief 发布给逻辑热路径的不可变 Session 列表快照。
struct PublishedSessionSnapshot {
    /// @brief 当前所有有效 Session 的稳定快照。
    std::vector<SessionSnapshotEntry> sessions;
};

/// @brief 只含 UI 标签元数据的不可变条目，不延长 BeatmapSession 生命周期。
struct SessionUiSnapshotEntry {
    /// @brief 与注册表索引保持一致的画布位置。
    int32_t index{ -1 };
    /// @brief 画布注册与命令路由共用的稳定相机 ID。
    std::string cameraId;
    /// @brief 当前条目是否仍为欢迎 Logo 占位会话。
    bool isLogoPlaceholder{ false };
    /// @brief 是否从项目工作区恢复原有停靠布局。
    bool restoreDockFromWorkspace{ false };
};

/// @brief UI 每帧读取的轻量会话列表，由结构变更低频发布。
struct PublishedSessionUiSnapshot {
    /// @brief 与会话注册表同索引的画布元数据。
    std::vector<SessionUiSnapshotEntry> entries;
};

}  // namespace MMM::Logic
