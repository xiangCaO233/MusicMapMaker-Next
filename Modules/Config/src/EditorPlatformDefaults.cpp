#include "config/EditorPlatformDefaults.h"

namespace MMM::Config
{
/// @brief 桌面与移动共用首次使用策略，界面和项目工作区不参与默认值选择。
/// @details 只影响新配置和缺失字段；已有配置的 false 必须原样恢复。
/// 两个吸附总开关都属于应用偏好，不能随输入设备或项目切换反复重置。
/// @warning 无分配、无运行期平台查询，构造配置时即可调用。
bool defaultMagnetEnabled() noexcept
{
    // 默认值统一在此定义，避免桌面与安卓的首次安装行为再次分叉。
    return true;
}
}  // namespace MMM::Config
