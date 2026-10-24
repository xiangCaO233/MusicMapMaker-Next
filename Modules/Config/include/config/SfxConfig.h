#pragma once

#include <map>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace MMM::Config
{

/// @brief 折线节点键音的两种听感策略，不改变物件与视觉特效。
enum class PolylineSfxMode {
    Strict,   ///< 严格逻辑：按各节点实际类型播放，保持历史行为。
    Gameplay  ///< 游玩体感：首节点按实际类型，后续仅播放滑键节点。
};

/// @brief 以稳定文本保存折线音效模式。
void to_json(nlohmann::json& json, const PolylineSfxMode& mode);
/// @brief 读取折线音效模式，未知值回退严格逻辑。
void from_json(const nlohmann::json& json, PolylineSfxMode& mode);

/// @brief 打击音效的独立开关、声像、增益与资源混音设置。
struct SfxConfig {
    /// @brief 折线播放策略，默认保留现有听感而不在升级时自动切换。
    PolylineSfxMode polylineSfxMode{ PolylineSfxMode::Strict };

    /// @brief 是否播放严格位于折线内部的滑键键音，不影响首尾或独立滑键。
    bool enablePolylineInternalFlickSfx{ true };

    /// @brief Flick类型音效的播放是否跟随滑动轨道数量进行增益
    bool enableFlickWidthVolumeScaling{ false };

    /// @brief 每增加一个轨道的增益倍率
    float flickWidthVolumeMultiplier{ 0.1f };

    /// @brief 是否按物件轨道位置分配 HitEffect 左右声道音量。
    bool enableStereoHitEffects{ true };

    /// @brief 皮肤常驻音效的独立音量映射 (Key: 音效ID, Value: 0.0~1.0)
    std::map<std::string, float> permanentSfxVolumes;

    /// @brief 皮肤常驻音效的静音状态 (Key: 音效ID)
    std::map<std::string, bool> permanentSfxMutes;

    /// @brief 皮肤全局打击音效的播放速率是否跟随主音轨
    bool hitSfxSyncSpeed{ true };

    /// @brief 是否启用打击音效
    bool enableHitSfx{ true };

    /// @brief 是否播放未绑定音效文件的物件所使用的默认打击音效。
    bool enableUnboundHitSfx{ true };

    /// @brief 未绑定音效文件的默认打击音效线性增益，范围为 0.0~2.0。
    float unboundHitSfxGain{ 1.0F };

    /// @brief 是否播放已经绑定音效文件的物件打击音效。
    bool enableBoundHitSfx{ true };

    /// @brief 已绑定音效文件的物件打击音效线性增益，范围为 0.0~2.0。
    float boundHitSfxGain{ 1.0F };

    /// @brief 普通谱面编辑器播放时是否启用节拍器；旧配置默认关闭。
    bool enableEditorMetronome{ false };

    /// @brief 编辑器节拍器的线性增益，范围为 0.0~2.0。
    float editorMetronomeGain{ 1.0F };
};

/// @brief 将打击音效线性增益规范到持久化与实时混音的共同范围。
/// @param gain 待规范的线性增益。
/// @return 0.0~2.0 的有限值；非有限输入按静音增益处理。
[[nodiscard]] float sanitizeHitSfxGain(float gain) noexcept;

/// @brief 将音效配置序列化为 JSON。
void to_json(nlohmann::json& json, const SfxConfig& config);
/// @brief 从 JSON 读取音效配置。
void from_json(const nlohmann::json& json, SfxConfig& config);

}  // namespace MMM::Config
