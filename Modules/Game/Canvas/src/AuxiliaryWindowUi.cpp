#include "canvas/AuxiliaryWindowUi.h"

#include "canvas/AuxiliaryWindowState.h"

#include <algorithm>
#include <imgui.h>

namespace MMM::Canvas
{
namespace
{
/// @brief 辅助窗口标题栏至少保留在显示器工作区内的逻辑像素宽度。
constexpr float AUXILIARY_WINDOW_MINIMUM_VISIBLE_TITLE_WIDTH = 64.0F;

/// @brief 从屏幕外恢复辅助窗口时保留的工作区边距。
constexpr float AUXILIARY_WINDOW_RECOVERY_MARGIN = 24.0F;

/// @brief 将 ImGui 坐标转换为辅助窗口矩形。
/// @param position 左上角屏幕坐标。
/// @param size 矩形尺寸。
/// @return 可供纯布局逻辑检查的矩形。
AuxiliaryWindowRect makeAuxiliaryWindowRect(const ImVec2& position,
                                            const ImVec2& size)
{
    // 纯值对象隔离 ImGui 类型，使几何规则能够在无 UI 上下文的测试中验证。
    return { position.x, position.y, size.x, size.y };
}
}  // namespace

/// @brief 检查当前 ImGui 窗口的标题栏是否仍在任一显示器工作区内。
/// @param dpiScale 当前窗口内容缩放。
/// @return 至少一个工作区保留足够标题栏操作区域时返回 true。
/// @warning UI 热路径：辅助窗口打开时调用，仅遍历平台显示器列表。
bool isCurrentAuxiliaryWindowReachable(float dpiScale)
{
    // Begin 之后读取实际恢复出的屏幕矩形，包含 imgui.ini 中的持久布局。
    const auto window =
        makeAuxiliaryWindowRect(ImGui::GetWindowPos(), ImGui::GetWindowSize());
    const float titleBarHeight = ImGui::GetFrameHeight();
    const float minimumVisibleWidth =
        // 缩放下限为 1，避免异常的小比例把可点击宽度压缩到不可操作。
        AUXILIARY_WINDOW_MINIMUM_VISIBLE_TITLE_WIDTH * std::max(dpiScale, 1.0F);

    const ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    // 多视口环境逐一检查各显示器工作区，窗口无需位于主显示器。
    for ( int index = 0; index < platformIo.Monitors.Size; ++index ) {
        const ImGuiPlatformMonitor& monitor = platformIo.Monitors[index];
        const auto                  workArea =
            makeAuxiliaryWindowRect(monitor.WorkPos, monitor.WorkSize);
        if ( isAuxiliaryWindowReachable(
                 window, workArea, titleBarHeight, minimumVisibleWidth) ) {
            // 找到一个可操作工作区即可提前返回，通常只需检查当前显示器。
            return true;
        }
    }

    // 平台后端未填充显示器列表时，以 ImGui 主视口作为兼容回退。
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if ( !mainViewport ) return false;
    const auto mainWorkArea =
        makeAuxiliaryWindowRect(mainViewport->WorkPos, mainViewport->WorkSize);
    // 回退仍复用相同纯几何规则，避免多显示器与单视口路径产生不同判据。
    return isAuxiliaryWindowReachable(
        window, mainWorkArea, titleBarHeight, minimumVisibleWidth);
}

/// @brief 按需把当前不可访问的 ImGui 辅助窗口恢复到主工作区。
/// @param requested 是否收到一次性恢复请求。
/// @param dpiScale 当前窗口内容缩放。
/// @warning 仅在打开或重新激活窗口时执行，不得持续覆盖用户拖动位置。
void recoverCurrentAuxiliaryWindow(bool requested, float dpiScale)
{
    // 未请求恢复或窗口本就可达时保持用户保存的位置和尺寸。
    if ( !requested || isCurrentAuxiliaryWindowReachable(dpiScale) ) return;

    // 没有主视口时无法确定安全工作区，保留原布局等待后续帧重试。
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if ( !mainViewport ) return;

    const auto window =
        makeAuxiliaryWindowRect(ImGui::GetWindowPos(), ImGui::GetWindowSize());
    const auto workArea =
        makeAuxiliaryWindowRect(mainViewport->WorkPos, mainViewport->WorkSize);
    const auto recovered = recoverAuxiliaryWindowRect(
        window,
        workArea,
        AUXILIARY_WINDOW_RECOVERY_MARGIN * std::max(dpiScale, 1.0F));

    // Begin 后才能读取项目布局实际恢复出的矩形；这里只在屏幕外恢复时写入一次。
    // 先设置尺寸再设置位置，确保居中坐标与最终受限尺寸保持一致。
    ImGui::SetWindowSize(ImVec2(recovered.width, recovered.height),
                         ImGuiCond_Always);
    ImGui::SetWindowPos(ImVec2(recovered.x, recovered.y), ImGuiCond_Always);
}

}  // namespace MMM::Canvas
