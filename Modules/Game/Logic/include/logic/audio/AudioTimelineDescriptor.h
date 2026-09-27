#pragma once

#include "audio/AudioManager.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace MMM
{
class BeatMap;
class Project;
}  // namespace MMM

namespace MMM::Logic
{

/// @brief 音频时间线描述符构建阶段的诊断类型。
enum class AudioTimelineDescriptorDiagnosticCode : std::uint8_t {
    /// @brief 自动采样引用无法解析为项目音频资源。
    UnresolvedAudioResource,
};

/// @brief 单个音频时间线描述符构建问题及其来源。
struct AudioTimelineDescriptorDiagnostic {
    /// @brief 诊断类型。
    AudioTimelineDescriptorDiagnosticCode m_code{
        AudioTimelineDescriptorDiagnosticCode::UnresolvedAudioResource
    };

    /// @brief 诊断关联的稳定采样事件标识。
    std::uint64_t m_eventId{ 0U };

    /// @brief 谱面中保存的原始音频引用。
    std::string m_audioReference;

    /// @brief 可直接展示给用户的中文诊断说明。
    std::string m_message;
};

/// @brief 从谱面领域对象生成的完整自动采样时间线描述符。
struct AudioTimelineDescriptor {
    /// @brief 按规范音频元组稳定排序的加载事件。
    std::vector<MMM::Audio::AudioTimelineLoadEvent> m_events;

    /// @brief 资源引用解析阶段产生的非致命诊断。
    std::vector<AudioTimelineDescriptorDiagnostic> m_diagnostics;

    /// @brief 覆盖全部听觉语义的稳定双 FNV-1a 指纹。
    std::string m_fingerprint;

    /// @brief 仅覆盖 Main 资源序列及各自有效起播位置的画布同步指纹。
    std::string m_mainAudioSyncFingerprint;

    /// @brief 非自动采样内容决定的谱面结束时间，单位为秒。
    double m_chartEndSeconds{ 0.0 };
};

/// @brief 判断已构建描述符是否引用指定项目音频资源。
/// @param descriptor 待检查的时间线描述符。
/// @param resourceId 项目音频资源的稳定 ID。
/// @return 任一加载事件使用该资源时返回 true。
/// @warning 低频资源配置路径：线性扫描当前谱面的自动采样事件。
[[nodiscard]] bool audioTimelineDescriptorReferencesResource(
    const AudioTimelineDescriptor& descriptor, std::string_view resourceId);

/// @brief 从 BeatMap 自动采样和项目资源构建音频加载描述符。
/// @param beatMap 待读取的谱面；仅遍历 m_audioSamples。
/// @param project 用于解析音频资源 ID、旧路径和完整音轨配置的项目。
/// @param beatmapPath 谱面所在的项目相对或绝对路径。
/// @param chartContentEndSeconds 玩家物件等非采样内容决定的结束时间。
/// @return 规范排序的加载事件、解析诊断、稳定指纹和谱面结束时间。
/// @warning 低频描述符重建路径：会解析文件系统路径并排序完整采样列表，
/// 只能在谱面载入或音频语义变化后调用，禁止放入每帧 update 热路径。
[[nodiscard]] AudioTimelineDescriptor buildAudioTimelineDescriptor(
    const BeatMap& beatMap, const Project& project,
    const std::filesystem::path& beatmapPath, double chartContentEndSeconds);

}  // namespace MMM::Logic
