#pragma once
#include <memory>
namespace MMM::Canvas
{
class TimelineInteractionPresentation;
/// @brief 移动工厂只暴露中立返回类型，不传播触点、菜单和导航实现。
/// @details 生产目标在构造期选择；CPU 测试也可以显式创建同一解释器。
/// 这里只声明工厂，具体类和手势成员都留在移动实现文件。
/// 公共 TimelineCanvas 仅持有中立基类，不引入 ImGui 移动布局依赖。
std::unique_ptr<TimelineInteractionPresentation>
createMobileTimelineInteractionPresentation();
}  // namespace MMM::Canvas
