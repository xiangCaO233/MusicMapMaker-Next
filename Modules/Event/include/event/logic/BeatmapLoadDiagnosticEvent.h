#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

#include <cstdint>
#include <string>

namespace MMM::Event
{

/// @brief UI 可稳定识别的谱面加载诊断类型。
/// @note 枚举用于选择交互操作，面向用户的说明仍由消息字段提供。
enum class BeatmapLoadDiagnosticKind : std::uint8_t {
    /// @brief 旧 MMM 旁存在可用于恢复已丢失 SOUND 信息的原始 Malody 文件。
    LegacyMmmOriginalMalodyAvailable,

    /// @brief 自动采样的非法绝对轨道已迁移到首条可用 BGM 轨。
    AudioSampleTrackRelocated,
};

/// @brief 谱面加载诊断的用户提示级别。
/// @note 提示级别只影响呈现方式，不改变诊断处理流程。
enum class BeatmapLoadDiagnosticSeverity : std::uint8_t {
    Info,
    Warning,
    Error,
};

/// @brief 谱面载入完成后由逻辑线程发送给 UI 的结构化诊断事件。
/// @note 所有路径和消息均按值保存，不依赖 Loader 的局部缓冲区生命周期。
struct BeatmapLoadDiagnosticEvent : public BaseEvent {
    /// @brief 本条诊断的稳定类型。
    BeatmapLoadDiagnosticKind m_kind{
        BeatmapLoadDiagnosticKind::LegacyMmmOriginalMalodyAvailable
    };

    /// @brief 本条诊断的提示级别。
    BeatmapLoadDiagnosticSeverity m_severity{
        BeatmapLoadDiagnosticSeverity::Warning
    };

    /// @brief 产生诊断的谱面路径，使用 UTF-8 字符串。
    std::string m_beatmapPath;

    /// @brief 可供用户采取修复操作的关联文件路径，使用 UTF-8 字符串。
    std::string m_relatedPath;

    /// @brief Loader 提供的诊断详情。
    std::string m_message;
};

}  // namespace MMM::Event

// 注册基础事件关系，便于通用诊断监听器记录加载阶段问题。
EVENT_REGISTER_PARENTS(MMM::Event::BeatmapLoadDiagnosticEvent,
                       MMM::Event::BaseEvent);
