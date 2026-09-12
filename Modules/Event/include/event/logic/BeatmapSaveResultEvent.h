#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <cstdint>
#include <string>

namespace MMM::Event
{

/// @brief 保存成功后应采用的界面反馈形式。
/// @note 该枚举只控制成功反馈展示，失败结果始终需要显式呈现。
enum class BeatmapSavePresentation : std::uint8_t {
    Transient,                  ///< 在鼠标附近显示常规保存反馈。
    TimedAutoSaveStatus,        ///< 在状态栏显示定时自动保存结果。
    TriggeredAutoSaveStatus,    ///< 在状态栏显示事件自动保存结果。
    TimedAutoBackupStatus,      ///< 在状态栏显示定时自动备份结果。
    TriggeredAutoBackupStatus,  ///< 在状态栏显示事件自动备份结果。
    Silent,                     ///< 成功时不显示界面反馈。
};

/// @brief 谱面保存或导出完成后的结果事件。
/// @note 事件按值携带路径和错误文本，避免依赖后台任务的临时缓冲区。
struct BeatmapSaveResultEvent : public BaseEvent {
    /// @brief 目标文件路径，使用 UTF-8 字符串。
    std::string path;
    /// @brief 是否成功写出文件。
    bool success{ false };
    /// @brief 是否来自另存为/导出流程。
    bool isExport{ false };
    /// @brief 失败时优先展示的具体原因；为空时由界面生成通用提示。
    std::string errorMessage;
    /// @brief 保存成功时采用的界面反馈形式。
    /// @note success 为 false 时订阅者忽略静默策略并显示错误反馈。
    BeatmapSavePresentation presentation{ BeatmapSavePresentation::Transient };
};

}  // namespace MMM::Event

// 注册基础事件关系，允许统一诊断保存任务的完成时刻。
EVENT_REGISTER_PARENTS(MMM::Event::BeatmapSaveResultEvent,
                       MMM::Event::BaseEvent)
