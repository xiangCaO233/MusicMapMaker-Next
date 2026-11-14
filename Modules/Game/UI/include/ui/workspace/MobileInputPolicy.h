#pragma once
#include <string>
#include <string_view>

namespace MMM::UI
{
/// @brief 移动端连接边沿策略，不查询平台、不持有窗口或手势对象。
/// @details 连接鼠标时选择经典方案；连接期间保留用户主动选择。
/// 拔出后强制触控方案，不能把键鼠方案留在无指针设备的界面中。
class MobileInputPolicy
{
public:
    /// @brief 仅初始化和设备状态变化时写入默认方案，无鼠标时拒绝其他方案。
    /// @return 请求发生变化时返回 true；调用方仍通过安全帧协议实际切换。
    /// @warning UI 每帧仅比较标量和短字符串，不保存配置或调用系统服务。
    bool update(bool mouse, std::string& requested)
    {
        // 插入边沿不恢复先前键鼠偏好，明确采用用户要求的经典默认方案。
        // 同一次连接期间不反复覆盖，用户可继续切换到纯鼠标等其他方案。
        const bool edge = !m_initialized || mouse != m_mouse;
        m_initialized   = true;
        m_mouse         = mouse;
        if ( !edge && mouse ) return false;
        const std::string_view next =
            mouse ? "classic-desktop" : "touch-gestures";
        if ( requested == next ) return false;
        requested = next;
        return true;
    }
    /// @brief 设置页借用已经发布的能力，不自行枚举设备。
    /// @warning UI 每帧只返回一个标量，不拥有输入设备或配置引用。
    bool hasMouse() const { return m_mouse; }

private:
    /// @brief 首帧也必须应用默认方案，不能依赖历史用户配置碰巧正确。
    bool m_initialized{};
    /// @brief 最近一次能力快照，仅由移动外壳的 UI 线程推进。
    bool m_mouse{};
};
/// @brief 检索与正文共用的鼠标专属行身份，隐藏时不清除用户保存值。
/// @warning 固定文本比较，无鼠标时搜索也不能导航到这些不存在的控件。
inline bool isPointerOnlySetting(std::string_view key)
{
    return key.starts_with("ui.settings.shortcut.") ||
           key == "ui.settings.editor.toolbar_value_wheel_adjustment" ||
           key == "ui.settings.editor.scroll_multiplier";
}
}  // namespace MMM::UI
