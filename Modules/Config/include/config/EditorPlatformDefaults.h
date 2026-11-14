#pragma once

namespace MMM::Config
{
/// @brief 获取应用首次使用时的磁吸默认值。
/// @details 桌面与安卓统一默认开启滚动和物件放置吸附。
/// 默认策略封装在配置实现中，公共配置头不传播平台 SDK 或条件宏。
/// 该值只用于缺省初始化，不得在 UI 更新时覆盖用户保存的显式开关。
/// @return 首次使用是否默认启用磁吸。
/// @warning 仅返回构建期确定的标量，不读取设备、配置或文件系统。
bool defaultMagnetEnabled() noexcept;
}  // namespace MMM::Config
