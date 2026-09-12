#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace MMM::Event
{

/// @brief 描述 GLFW 窗口接收到的文件拖放操作。
/// @note 路径在回调返回前复制，避免依赖 GLFW 提供的临时字符串。
struct GLFWDropEvent : public BaseEvent {
    /// @brief 本次拖放携带的 UTF-8 文件路径列表。
    std::vector<std::string> paths;
    /// @brief 文件释放时鼠标在窗口坐标系中的位置。
    glm::vec2 pos;
};

}  // namespace MMM::Event

// 基础事件订阅者可借此统一记录文件拖放的发生时间。
// 具体导入与路径校验留给业务层处理，事件层不访问文件系统。
EVENT_REGISTER_PARENTS(MMM::Event::GLFWDropEvent, MMM::Event::BaseEvent)
