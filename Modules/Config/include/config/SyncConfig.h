#pragma once

/// @note 同步间隔以秒保存，不随 UI 分拍或谱面 BPM 自动换算。
#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 音画同步参数的独立值定义，供播放时钟与编辑器配置共同使用。
/// @details 不依赖完整界面、调色盘和布局设置，时钟算法只消费本组参数。
/// @brief 视觉时钟按音频观测修正位置的策略。
enum class SyncMode {
    None,      ///< 直接同步音频时间 (可能抖动)
    Integral,  ///< 积分制同步 (平滑追踪)
    WaterTank  ///< 水箱制同步 (固定延迟)
};

/// @brief 将同步模式序列化为稳定文本。
void to_json(nlohmann::json& json, const SyncMode& mode);
/// @brief 从稳定文本读取同步模式。
void from_json(const nlohmann::json& json, SyncMode& mode);

/// @brief 长期保存的同步校准参数，字段和默认值保持兼容。
struct SyncConfig {
    SyncMode mode{ SyncMode::Integral };
    float    integralFactor{ 0.1f };    ///< 积分追踪系数 (0.0~1.0)
    float    waterTankBuffer{ 0.05f };  ///< 水箱缓冲时间 (秒)
    double   syncInterval{ 10.0 };      ///< 强制同步周期 (秒)
};

/// @brief 将同步配置序列化为 JSON。
void to_json(nlohmann::json& json, const SyncConfig& config);
/// @brief 从 JSON 读取同步配置。
void from_json(const nlohmann::json& json, SyncConfig& config);

}  // namespace MMM::Config
