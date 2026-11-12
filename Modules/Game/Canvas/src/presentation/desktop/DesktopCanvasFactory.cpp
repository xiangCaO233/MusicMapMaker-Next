#include "presentation/CanvasInteractionPresentation.h"
#include "presentation/TimelineInteractionPresentation.h"
namespace MMM::Canvas
{
/// @brief 桌面平台保持原来的指针解释器，不采用安卓触控仲裁。
/// @details 工厂按 CMake 平台选择，原生桌面只链接这一份平台入口。
/// 具体解释器仍可供安卓的经典和纯鼠标方案借用。
std::unique_ptr<CanvasInteractionPresentation>
createCanvasInteractionPresentation()
{
    // 构造仅发生在画布生命周期起点，不在方案切换时重复申请对象。
    return createDesktopCanvasInteractionPresentation();
}
/// @brief 时间线与主画布采用相同的构造期平台隔离，不在公共头嵌入平台宏。
/// @details 返回中立基类，调用方不需要包含具体平台头。
/// Timeline 与主画布拥有不同实例，不能共享待定触点或菜单释放状态。
std::unique_ptr<TimelineInteractionPresentation>
createTimelineInteractionPresentation()
{
    // 只在画布创建时分配，热路径不重复创建状态机。
    return std::make_unique<TimelineInteractionPresentation>();
}
}  // namespace MMM::Canvas
