#include "ui/imgui/menu/actions/help/DeferredUpdatePopup.h"
#include "log/colorful-log.h"
#include "ui/utils/UIWidgetUtils.h"
#include <imgui.h>

/// @brief 使用真实 ImGui 弹层状态验证更新通知排队，不访问网络和音频设备。
/// @return 同意窗口未被抢占、自动请求只消费一次、手动请求仍可打开时返回零。
/// @details 帧二关闭同意窗口，帧四模拟外部关闭更新窗口，帧七模拟手动重查。
/// 持续的网络终态不会重新设置 pending，因此关闭后必须保持安静。
/// 测试包含先占用宿主的模态，重现启动同意框与更新结果的竞争。
/// 手动重查在旧窗口关闭后发生，保留用户主动查看更新的能力。
int main()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    // 不读取或写入用户布局，测试结束不会改变任何实际桌面窗口。
    io.IniFilename = nullptr;
    io.DisplaySize = { 800, 600 };
    // 固定帧间隔只推进 ImGui 状态，测试不通过 sleep 等待异步网络。
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels{};
    int            width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 测试必须静音；通过实际打开次数验证反馈入口，而非制造可听弹窗声。
    MMM::UI::SetInteractionFeedbackEnabled(false);
    // 模拟一次已经完成的检查事件，不在每帧按持续终态重新置位。
    bool pending = true;
    bool passed  = true;
    int  requests{};
    for ( int frame = 0; frame < 12; ++frame ) {
        ImGui::NewFrame();
        ImGui::Begin("StablePopupHost");
        if ( frame == 0 ) MMM::UI::FeedbackOpenPopup("Consent");
        if ( ImGui::BeginPopupModal("Consent") ) {
            // 模态正文由正常 Begin/End 维持，不能通过测试手工伪造栈状态。
            if ( frame == 2 ) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        // 用户明确重查是第二个事件，不能被第一次提示的取消永久屏蔽。
        if ( frame == 7 ) pending = true;
        const bool opened =
            MMM::UI::Detail::openDeferredUpdatePopup("Update", pending);
        // 计数生产 helper 的真实打开边沿，不以窗口当前可见帧数代替。
        requests += opened ? 1 : 0;
        // 前两帧已有模态，不允许消费请求或播放一次新的打开反馈。
        if ( frame < 2 )
            passed &= !opened && pending && ImGui::IsPopupOpen("Consent");
        if ( ImGui::BeginPopupModal("Update") ) {
            // 外部关闭和显式取消都不应触发下一帧重新打开。
            if ( frame == 4 || frame == 9 ) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        // 关闭后仍保留终态的阶段，既没有请求也不应有更新弹层。
        if ( frame == 5 || frame == 6 || frame >= 10 )
            passed &= !opened && !pending && !ImGui::IsPopupOpen("Update");
        // 每一帧严格配对窗口和渲染生命周期，让关闭状态正常跨帧生效。
        ImGui::End();
        ImGui::Render();
    }
    // 一次自动完成、一次用户重查，共两次打开，不等于总渲染帧数。
    // 等待已有模态结束期间不消费事件，才能保证提示既不丢失也不重复。
    // 打开计数由实际 ImGui helper 返回，未根据预设帧号伪造结果。
    // 同时防止丢掉排队通知和禁用所有弹窗这两种假修复。
    passed &= requests == 2;
    ImGui::DestroyContext();
    if ( !passed )
        XERROR("Deferred update popup reopened or displaced another modal");
    return passed ? 0 : 1;
}
