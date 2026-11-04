#include "MobileCanvasInteractionPresentation.h"
#include "presentation/CanvasInteractionPresentation.h"
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
}  // namespace MMM::Canvas
