#pragma once
#include "event/ui/UIEvent.h"

namespace MMM::Event
{

/// @brief 设置窗口支持切换的配置页签。
/// @note 枚举值与设置窗口的标签模型对应，不承诺持久化数值稳定。
enum class SettingsTab {
    Software,       ///< 软件配置。
    Collaboration,  ///< 多人协作配置。
    Visual,         ///< 视觉配置。
    Project,        ///< 项目配置。
    Beatmap,        ///< 谱面配置。
    Editor,         ///< 编辑器配置。
    Shortcut,       ///< 快捷键配置。
    Debug           ///< 调试配置。
};

/// @brief 请求设置窗口切换到指定配置页签。
/// @note 事件只描述目标页签，窗口显示与焦点策略由 UI 层决定。
/// @note 发布者无需持有设置窗口，事件总线负责将请求送至订阅者。
struct UISettingsTabEvent : public UIEvent {
    /// @brief 要切换到的设置标签。
    SettingsTab tab;
};

}  // namespace MMM::Event

// 注册到 UIEvent 分发链，便于设置窗口通过统一 UI 入口处理。
// 通用 UI 监听器仍可使用继承字段识别请求来源。
// 具体页签的可见性和可用性由设置窗口在消费事件时校验。
EVENT_REGISTER_PARENTS(MMM::Event::UISettingsTabEvent, MMM::Event::UIEvent);
