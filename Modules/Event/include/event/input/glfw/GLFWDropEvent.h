#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace MMM::Event
{

/**
 * @brief GLFW 文件拖拽事件
 */
struct GLFWDropEvent : public BaseEvent {
    std::vector<std::string> paths;
    glm::vec2                pos;  // 拖拽放下时的鼠标位置
};

}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(MMM::Event::GLFWDropEvent, MMM::Event::BaseEvent)
