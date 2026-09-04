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
    return { position.x, position.y, size.x, size.y };
}
}  // namespace

bool isCurrentAuxiliaryWindowReachable(float dpiScale)
{
    const auto window =
        makeAuxiliaryWindowRect(ImGui::GetWindowPos(), ImGui::GetWindowSize());
    const float titleBarHeight = ImGui::GetFrameHeight();
    const float minimumVisibleWidth =
        AUXILIARY_WINDOW_MINIMUM_VISIBLE_TITLE_WIDTH * std::max(dpiScale, 1.0F);

    const ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    for ( int index = 0; index < platformIo.Monitors.Size; ++index ) {
        const ImGuiPlatformMonitor& monitor = platformIo.Monitors[index];
        const auto                  workArea =
            makeAuxiliaryWindowRect(monitor.WorkPos, monitor.WorkSize);
        if ( isAuxiliaryWindowReachable(
                 window, workArea, titleBarHeight, minimumVisibleWidth) ) {
            return true;
        }
    }

    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if ( !mainViewport ) return false;
    const auto mainWorkArea =
        makeAuxiliaryWindowRect(mainViewport->WorkPos, mainViewport->WorkSize);
    return isAuxiliaryWindowReachable(
        window, mainWorkArea, titleBarHeight, minimumVisibleWidth);
}

void recoverCurrentAuxiliaryWindow(bool requested, float dpiScale)
{
    if ( !requested || isCurrentAuxiliaryWindowReachable(dpiScale) ) return;

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
    ImGui::SetWindowSize(ImVec2(recovered.width, recovered.height),
                         ImGuiCond_Always);
    ImGui::SetWindowPos(ImVec2(recovered.x, recovered.y), ImGuiCond_Always);
}

}  // namespace MMM::Canvas
