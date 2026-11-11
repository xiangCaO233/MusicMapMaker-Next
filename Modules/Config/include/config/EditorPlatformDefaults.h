#pragma once

namespace MMM::Config
{
/// @brief 获取当前构建平台首次使用时的磁吸默认值。
/// @details 安卓默认开启滚动和物件放置吸附，桌面继续使用原默认值。
/// 平台选择只封装在实现文件，公共配置头不传播 Android SDK 或平台宏。
/// 该值只用于缺省初始化，不得在 UI 更新时覆盖用户保存的显式开关。
/// @return 当前平台是否默认启用磁吸。
/// @warning 仅返回构建期确定的标量，不读取设备、配置或文件系统。
bool defaultMagnetEnabled() noexcept;
}  // namespace MMM::Config
