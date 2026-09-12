#pragma once

#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 编辑器与渲染循环共享的帧率限制偏好。
enum class FrameLimitPreference {
    /// 跟随显示设备垂直同步频率。
    VSync,
    /// 以刷新率的二、四或八倍驱动逻辑与渲染循环。
    Refresh2x,
    Refresh4x,
    Refresh8x,
    /// 不设置目标频率，由循环尽可能快地推进。
    Unlimited
};

/// @brief 将帧率限制偏好序列化为稳定文本。
void to_json(nlohmann::json& json, const FrameLimitPreference& preference);
/// @brief 从稳定文本读取帧率限制偏好。
void from_json(const nlohmann::json& json, FrameLimitPreference& preference);

}  // namespace MMM::Config
