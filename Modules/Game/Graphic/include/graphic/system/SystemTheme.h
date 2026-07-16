#pragma once

#include <cstdint>

namespace MMM::Graphic
{
/// @brief 操作系统为应用界面提供的亮暗外观偏好。
enum class SystemTheme : std::uint8_t {
    Unknown,  ///< 系统未提供明确偏好。
    Light,    ///< 系统偏好亮色界面。
    Dark      ///< 系统偏好暗色界面。
};

/// @brief 获取当前缓存或首次查询到的系统外观偏好。
/// @return 当前系统主题；平台无法识别时返回 Unknown。
/// @warning 主线程低频路径：Linux 首次调用会连接 XDG Settings Portal，禁止在
/// 每帧热路径中直接反复调用。
[[nodiscard]] SystemTheme getSystemTheme();

/// @brief 刷新平台主题状态并返回最新外观偏好。
/// @return 刷新后的系统主题；平台无法识别时返回 Unknown。
/// @warning 主线程低频路径：应由外层节流后调用；Linux 仅非阻塞派发专用
/// GMainContext 中已到达的 Portal 信号。
[[nodiscard]] SystemTheme refreshSystemTheme();
}  // namespace MMM::Graphic
