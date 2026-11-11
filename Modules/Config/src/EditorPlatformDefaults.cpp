#include "config/EditorPlatformDefaults.h"

namespace MMM::Config
{
/// @brief 平台默认策略集中在配置实现边界，桌面与移动 UI 不参与判断。
/// @details 只影响新配置和缺失字段；已有配置的 false 必须原样恢复。
/// 安卓精细触摸编辑依赖分拍磁吸，默认开启两个吸附入口。
/// 桌面沿用已有行为，不借本次移动适配改变键鼠编辑偏好。
/// @warning 无分配、无运行期平台查询，构造配置时即可调用。
bool defaultMagnetEnabled() noexcept
{
#if defined(__ANDROID__)
    // 编译目标决定默认值，不根据当前输入设备反复切换用户偏好。
    return true;
#else
    // 桌面构建继续保持历史默认，已有测试和用户配置无需迁移。
    return false;
#endif
}
}  // namespace MMM::Config
