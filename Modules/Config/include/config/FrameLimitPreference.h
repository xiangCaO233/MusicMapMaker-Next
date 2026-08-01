#pragma once

#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 编辑器与渲染循环共享的帧率限制偏好。
enum class FrameLimitPreference {
    VSync,
    Refresh2x,
    Refresh4x,
    Refresh8x,
    Unlimited
};

/// @brief 将帧率限制偏好序列化为稳定文本。
void to_json(nlohmann::json& json, const FrameLimitPreference& preference);
/// @brief 从稳定文本读取帧率限制偏好。
void from_json(const nlohmann::json& json, FrameLimitPreference& preference);

}  // namespace MMM::Config
