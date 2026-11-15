#include "ui/utils/DownloadRouteSelector.h"
#include "log/colorful-log.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace
{
/// @brief 每帧使用正式控件，返回下拉框在宿主窗口的真实命中位置。
/// @param size 模拟视口尺寸，窄屏也必须能打开并点击全部选项。
/// @param scale 当前内容倍率，控件宽度必须跟随实际可用空间收缩。
/// @warning 仅推进测试 ImGui 帧，不读取文件、请求网络或等待后台线程。
/// @details 宿主标题和位置固定，排除拖动窗口造成的命中坐标变化。
/// 每帧重新调用正式控件，不能用测试副本模拟 BeginCombo 的状态机。
/// 命中位置来自提交后的项目矩形，不依赖特定字体的固定宽度。
/// Render 提交帧后才允许下一次输入，保证按下与抬起严格分离。
ImVec2 frame(ImVec2 size, float scale)
{
    auto& io       = ImGui::GetIO();
    io.DisplaySize = size;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize(size);
    // 固定宿主排除标题拖动和持久布局，输入只作用于被测选择控件。
    ImGui::Begin(
        "RouteHost",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    MMM::UI::drawDownloadRouteSelector(false, scale);
    // 无下载提示时最后一项始终是 Combo，读取实际矩形而非硬编码点击坐标。
    const auto   a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 hit{ (a.x + b.x) * .5f, (a.y + b.y) * .5f };
    ImGui::End();
    ImGui::Render();
    return hit;
}

/// @brief 输入完整的按下和抬起，不能直接写入控件内部的激活状态。
/// @details 分帧事件遵循正常鼠标输入路径，确认控件确实把选择发布到网络层。
/// 点击先提交指针位置与按下，再提交抬起，不直接设置 ActiveId。
/// 这样同时覆盖控件的打开边沿和选项在松手时的正常激活语义。
/// 新帧继续读取同一视口和倍率，不能用切换窗口大小掩盖坐标错误。
void click(ImVec2 point, ImVec2 size, float scale)
{
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(point.x, point.y);
    io.AddMouseButtonEvent(0, true);
    frame(size, scale);
    io.AddMouseButtonEvent(0, false);
    frame(size, scale);
}
}  // namespace

/// @brief 验证默认值、三个手动选择入口与窄屏可达性，不启动真实应用或音频。
/// @return 任一点击没有传递到下载策略，或弹出选项越出视口时返回非零。
/// @details 两个独立上下文分别模拟紧凑视口和桌面，不复用残留弹窗。
/// 先选 R2，再选源站，最后回到自动，验证往返切换而非单向初始化。
/// 断言直接读取 Network 选项，使只有显示文本变化的假接线无法通过。
/// 弹窗矩形必须完全落在视口内，所有选项均应能够通过点击命中。
/// 不使用操作系统鼠标注入，运行测试不会打扰用户当前桌面。
/// 不链接显示后端或创建 Vulkan 设备，窗口状态仍由真实 ImGui 推进。
/// 无外网、音频和皮肤依赖，测试失败不会遗留下载或安装文件。
int main()
{
    bool passed = true;
    for ( const ImVec2 size : { ImVec2{ 384, 320 }, ImVec2{ 1280, 720 } } ) {
        // 独立上下文避免先前桌面布局改变窄屏弹层的初始位置。
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        // 字体只用于布局与命中，测试不创建 GPU 纹理或访问用户字体目录。
        io.IniFilename = nullptr;
        io.DeltaTime   = 1.0f / 60.0f;
        unsigned char* pixels{};
        int            width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        const float scale = size.x < 400 ? 1.75f : 1.0f;
        MMM::Network::setDownloadRoute(MMM::Network::DownloadRoute::Automatic);
        frame(size, scale);
        // 每次先稳定一帧，再用上次的真实矩形打开下拉框。
        // 从 R2 到源站再回自动，覆盖三个选项与当前选中项的更新。
        for ( const int index : { 1, 2, 0 } ) {
            const auto hit = frame(size, scale);
            click(hit, size, scale);
            frame(size, scale);
            ImGuiWindow* popup = nullptr;
            // 只在测试中观察真实活动 Combo 窗口，不写内部状态或制造伪弹层。
            for ( auto* window : ImGui::GetCurrentContext()->Windows )
                if ( window->Active &&
                     (window->Flags & ImGuiWindowFlags_Popup) )
                    popup = window;
            // 必须真的打开弹窗才继续点击，不能用默认枚举值替代控件交互。
            if ( !popup ) {
                passed = false;
                break;
            }
            // 选项来自正式控件，行距遵循当前 ImGui 样式与字体，不假设固定像素。
            const auto   row = ImGui::GetTextLineHeightWithSpacing();
            const ImVec2 choice{ popup->WorkRect.Min.x + 8,
                                 popup->WorkRect.Min.y + row * (index + .5f) };
            passed &= popup->Pos.x >= 0 && popup->Pos.y >= 0 &&
                      popup->Pos.x + popup->Size.x <= size.x &&
                      popup->Pos.y + popup->Size.y <= size.y;
            // 在全部选项可见的约束下点击真实行中心，随后读取后台共用值。
            click(choice, size, scale);
            passed &=
                static_cast<int>(MMM::Network::getDownloadRoute()) == index;
        }
        // 不把测试最终选项泄漏到同一套件的其他用例。
        MMM::Network::setDownloadRoute(MMM::Network::DownloadRoute::Automatic);
        // 所有输入帧均已 Render，销毁上下文不会留下跨帧窗口或弹层观察指针。
        // 下一尺寸重新建立上下文，确保本轮成功不能掩盖后续布局失败。
        ImGui::DestroyContext();
    }
    if ( !passed )
        XERROR(
            "Download route selector did not publish clicked choice or escaped "
            "viewport");
    return passed ? 0 : 1;
}
