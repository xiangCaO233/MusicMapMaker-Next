#pragma once

#include "config/EditorSettings.h"
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace MMM::Config
{
/// @brief 客户端听音配置，与谱面资源和导出数据相互独立。
/// @note 音量采用线性倍率；界面统一按整数百分数显示。
struct AudioPreset {
    /// @brief 用户自定义名称，在当前库内唯一。
    std::string m_name{ "默认" };
    /// @brief 最终输出音量与静音。
    float m_volume{ 0.25F };
    bool  m_muted{ false };
    /// @brief 音乐总线音量与静音。
    float m_music{ 1.0F };
    bool  m_musicMuted{ false };
    /// @brief 效果音总线音量与静音。
    float m_effect{ 1.0F };
    bool  m_effectMuted{ false };
    /// @brief 界面反馈音量与静音。
    float m_interaction{ 1.0F };
    bool  m_interactionMuted{ false };
    /// @brief 打击音、节拍器及皮肤常驻音效的个别偏好。
    SfxConfig m_sfx;
    /// @brief 客户端预览均衡器类型：关闭、十段、十五段。
    int m_eqPreset{ 0 };
    /// @brief 均衡器各段增益，单位为 dB，不能换算成音量百分比。
    std::vector<float> m_eqGains;
    /// @brief 均衡器各段 Q，保留其无量纲物理含义。
    std::vector<float> m_eqQs;
    /// @brief 主时间线试听速度，不修改谱面时间坐标。
    double m_speed{ 1.0 };
    /// @brief 主时间线试听音高，单位为半音。
    double m_pitch{ 0.0 };
    /// @brief 拉伸质量，稳定顺序为快速、平衡、精细、最佳。
    int m_quality{ 2 };
};
/// @brief 仅存于 user_config 的个人预设库，不提供文件导入导出。
struct AudioPresets {
    /// @brief 至少保留一个可编辑预设。
    std::vector<AudioPreset> m_items{ AudioPreset{} };
    /// @brief 当前选择同时作为下次启动和打开项目的默认配置。
    std::string m_selected{ "默认" };
    /// @brief 返回当前项；仅在加载或用户修改预设时查找。
    AudioPreset& current();
    /// @brief 返回当前项的只读访问。
    const AudioPreset& current() const;
    /// @brief 切换前收取旧预设的最终音量，再应用新预设。
    bool select(const std::string& name, EditorSettings& settings);
    /// @brief 复制当前预设并选中，空名和重名不改变现有数据。
    bool duplicate(const std::string& name, EditorSettings& settings);
    /// @brief 重命名当前项，保留所有音量和效果器参数。
    bool rename(const std::string& name);
    /// @brief 删除当前项并选中首项，禁止删除最后一个预设。
    bool remove(EditorSettings& settings);
};
/// @brief 捕获软件混音字段，不覆盖预设已保存的效果器参数。
void captureAudioPreset(AudioPreset& preset, const EditorSettings& settings);
/// @brief 应用混音字段，保留与听音无关的编辑器偏好。
void applyAudioPreset(const AudioPreset& preset, EditorSettings& settings);
/// @brief 保留 AppConfig 直接修改的四个总线值，避免旧引擎快照回写。
void preserveAudioMixerSettings(EditorSettings&       target,
                                const EditorSettings& source);
/// @brief 序列化个人预设库。
void to_json(nlohmann::json& json, const AudioPresets& presets);
/// @brief 读取并清洗预设库；损坏、空库和失效选择回退到可编辑默认项。
void from_json(const nlohmann::json& json, AudioPresets& presets);
}  // namespace MMM::Config
