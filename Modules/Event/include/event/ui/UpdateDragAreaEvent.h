#pragma once

#include "event/EventDef.h"
#include "event/ui/UIEvent.h"
#include <vector>

namespace MMM::Event
{

/**
 * @brief 拖拽区域描述结构体
 * 记录无边框窗口中允许进行原生拖拽的矩形区域
 */
struct DragArea {
    float x;  ///< 区域左上角 X 坐标
    float y;  ///< 区域左上角 Y 坐标
    float w;  ///< 区域宽度
    float h;  ///< 区域高度
};

/**
 * @brief 更新拖拽区域事件
 * 当 UI 布局发生变化时，由 UI 系统向底层窗口系统发送新的拖拽区域列表
 */
struct UpdateDragAreaEvent : public UIEvent {
    std::vector<DragArea> areas;  ///< 允许拖拽的矩形区域集合
};

}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(UpdateDragAreaEvent, UIEvent)