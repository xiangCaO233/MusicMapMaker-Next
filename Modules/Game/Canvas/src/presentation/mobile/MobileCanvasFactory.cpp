#include "MobileCanvasInteractionPresentation.h"
#include "MobileTimelineInteractionPresentation.h"
#include "presentation/AnnotationTablePresentation.h"
#include "presentation/CanvasInteractionPresentation.h"
#include "presentation/PreviewInteractionPresentation.h"
#include "presentation/TimelineInteractionPresentation.h"
namespace MMM::Canvas
{
/// @brief 安卓编译目标的唯一工厂；桌面目标选择另一个编译单元。
/// 桌面构建不链接该文件，避免出现两个同名平台工厂。
/// @details 分派发生在画布构造时，更新路径无需重复平台判断。
std::unique_ptr<CanvasInteractionPresentation>
createCanvasInteractionPresentation()
{
    // 测试可以显式构造移动策略，无需替换生产桌面工厂。
    // 所有触摸菜单与手势状态都由移动实现独占。
    return createMobileCanvasInteractionPresentation();
}
/// @brief 时间线与主画布采用相同的构造期平台隔离，不在公共头嵌入平台宏。
/// @details 返回中立基类，调用方不需要包含具体平台头。
/// Timeline 与主画布拥有不同实例，不能共享待定触点或菜单释放状态。
std::unique_ptr<TimelineInteractionPresentation>
createTimelineInteractionPresentation()
{
    // 只在画布创建时分配，热路径不重复创建状态机。
    return createMobileTimelineInteractionPresentation();
}
/// @brief 批注表与画布分别构造平台对象，不共享未提交编辑或触点。
std::unique_ptr<AnnotationTablePresentation> createAnnotationTablePresentation()
{
    return createMobileAnnotationTablePresentation();
}
/// @brief 预览导航拥有独立平台状态，不借用主画布手势解释器。
// 构造只发生于视图创建，不在连续输入帧分配对象。
// 预览状态独占，不能把主画布的选择或历史轻触带入导航。
std::unique_ptr<PreviewInteractionPresentation>
createPreviewInteractionPresentation()
{
    return createMobilePreviewInteractionPresentation();
}
}  // namespace MMM::Canvas
