#pragma once

#include "config/VisualConfig.h"

namespace MMM::UI
{

/// @brief 记录最近使用的两个分拍线显示模式，供快捷键在二者间切换。
/// @details 历史只保留最近两个不同枚举值；首次观察时以传统“始终显示/隐藏”
/// 组合补足前一模式，使快捷键在启动后立即有确定目标。
class BeatLineDisplayModeHistory
{
public:
    /// @brief 记录当前生效的分拍线显示模式。
    /// @param mode 当前显示模式。
    /// @warning UI 热路径：每帧仅比较和写入枚举值，禁止引入分配或阻塞操作。
    constexpr void observe(Config::BeatLineDisplayMode mode)
    {
        if ( !m_initialized ) {
            // 首次观察同时建立当前值和兼容旧行为的默认另一值。
            m_currentMode  = mode;
            m_previousMode = defaultPreviousMode(mode);
            m_initialized  = true;
            return;
        }
        if ( mode == m_currentMode ) {
            // 重复同步同一配置不能挤掉真正的上一个模式。
            return;
        }
        // 新的不同模式到来时，将原当前值推进为前一值。
        m_previousMode = m_currentMode;
        m_currentMode  = mode;
    }

    /// @brief 获取最近另一个模式，并将本次切换写入历史。
    /// @param currentMode 当前生效的显示模式。
    /// @return 最近使用的另一个显示模式。
    constexpr Config::BeatLineDisplayMode toggleTarget(
        Config::BeatLineDisplayMode currentMode)
    {
        // 先同步外部当前值，兼容设置面板绕过快捷键直接改动配置。
        observe(currentMode);
        // 捕获目标后再作为新当前值写回，下一次调用即可反向切换。
        const Config::BeatLineDisplayMode target = m_previousMode;
        observe(target);
        return target;
    }

private:
    /// @brief 在尚无历史时选择兼容传统显隐切换的另一个模式。
    /// @param mode 当前显示模式。
    /// @return 当前为隐藏时返回始终显示，否则返回隐藏。
    static constexpr Config::BeatLineDisplayMode defaultPreviousMode(
        Config::BeatLineDisplayMode mode)
    {
        // 只有 Hidden 的默认配对是 Always，其余可见模式都配对 Hidden。
        return mode == Config::BeatLineDisplayMode::Hidden
                   ? Config::BeatLineDisplayMode::Always
                   : Config::BeatLineDisplayMode::Hidden;
    }

    /// @brief 是否已经观察到首个显示模式。
    bool m_initialized{ false };
    /// @brief 最近一次观察到的当前模式。
    Config::BeatLineDisplayMode m_currentMode{
        Config::BeatLineDisplayMode::Always
    };
    /// @brief 当前模式之前最近使用的模式。
    Config::BeatLineDisplayMode m_previousMode{
        Config::BeatLineDisplayMode::Hidden
    };
};

}  // namespace MMM::UI
