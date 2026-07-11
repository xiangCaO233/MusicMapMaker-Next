#pragma once
#include "event/ui/UIEvent.h"

namespace MMM::Event
{

enum class SettingsTab {
    Software,  // 软件配置
    Visual,    // 视觉配置
    Project,   // 项目配置
    Beatmap,   // 谱面配置
    Editor,    // 编辑器配置
    Shortcut,  // 快捷键配置
    Debug      // 调试配置
};

struct UISettingsTabEvent : public UIEvent {
    /// @brief 要切换到的设置标签
    SettingsTab tab;
};

}  // namespace MMM::Event

// 注册父类关系
EVENT_REGISTER_PARENTS(MMM::Event::UISettingsTabEvent, MMM::Event::UIEvent);
