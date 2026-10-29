#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/workspace/IWorkspacePresentation.h"

namespace MMM::UI
{
namespace
{
/// @brief 桌面外壳完整保留原有菜单、状态栏、侧栏与停靠逻辑。
/// @details 默认窗口和输入规则直接继承公共接口，不增加设备判断。
/// 该实现与移动实现由构建目标二选一，运行时没有双外壳共存。
/// 生命周期属于 UIManager，所有更新发生在原有 UI 线程。
class DesktopWorkspacePresentation final : public IWorkspacePresentation
{
public:
    /// @brief 只转发既有桌面组合，不承载移动端状态或手势判断。
    void renderWorkspace(MainDockSpaceUI& dock, UIManager& manager) override
    {
        // 原有桌面几何在单独文件维护，公共更新循环不再承担平台布局。
        dock.renderDesktopWorkspace(&manager);
    }
};
}  // namespace

/// @brief 创建唯一桌面策略，工厂签名与移动目标保持一致。
/// @return 独占对象；不依赖全局析构顺序或引用计数。
std::unique_ptr<IWorkspacePresentation> createWorkspacePresentation()
{
    // 仅管理器初始化时分配一次，每帧复用同一策略实例。
    return std::make_unique<DesktopWorkspacePresentation>();
}
}  // namespace MMM::UI
