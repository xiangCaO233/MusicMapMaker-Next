#pragma once

#include "network/DownloadRouting.h"
#include <algorithm>
#include <imgui.h>

namespace MMM::UI
{
/// @brief 启动资源与更新窗口共用的会话选线入口。
/// @param downloading 是否显示切换后重下当前文件的提示。
/// @param scale 当前 UI 倍率，触摸窗口可使用较大的帧内边距。
/// @warning 每帧只绘制控件并读取单一原子枚举，不访问磁盘或等待网络线程。
/// @details 三个选项共用网络层策略，窗口不能自行拼接不同版本的下载地址。
/// 此处不保存句柄、后台对象或回调，所有下载仍由原异步服务持有。
inline void drawDownloadRouteSelector(bool downloading, float scale)
{
    // 文本放在控件上方，窄屏不因右侧标签挤掉当前选中的线路名称。
    ImGui::TextUnformatted("下载线路");
    const auto            route    = Network::getDownloadRoute();
    constexpr const char* labels[] = { "自动选择", "R2", "服务器直连" };
    // 选项枚举由同一个网络接口校验，界面与后台使用一致的下标约定。
    // getter 只读会话意图；不可根据某次自动选中的 IP 反向改写用户选项。
    // 同一会话的两个窗口共享此值，避免启动资源和更新包各持有一份陈旧选择。
    const int selected = static_cast<int>(route);
    ImGui::SetNextItemWidth(
        std::min(320.0f * scale, ImGui::GetContentRegionAvail().x));
    if ( ImGui::BeginCombo("##DownloadRoute", labels[selected]) ) {
        for ( int index = 0; index < 3; ++index ) {
            if ( ImGui::Selectable(labels[index], selected == index) )
                Network::setDownloadRoute(
                    static_cast<Network::DownloadRoute>(index));
            if ( selected == index ) ImGui::SetItemDefaultFocus();
        }
        // EndCombo 只与成功 Begin 配对，保持弹窗输入栈平衡。
        ImGui::EndCombo();
    }
    // 进度重置来自安全截断，提前说明当前文件会重下，避免误认为资源全部重来。
    if ( downloading ) ImGui::TextWrapped("切换线路后会重新下载当前文件");
}
}  // namespace MMM::UI
