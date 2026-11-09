#include "presentation/CanvasInteractionPresentation.h"
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
}  // namespace MMM::Canvas
