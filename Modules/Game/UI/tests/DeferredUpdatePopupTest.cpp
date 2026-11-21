#include "ui/imgui/menu/actions/help/DeferredUpdatePopup.h"
#include "log/colorful-log.h"
#include "ui/utils/UIWidgetUtils.h"
#include <imgui.h>

/// @brief 使用真实 ImGui 弹层状态验证更新通知排队，不访问网络和音频设备。
/// @return 通知避让授权、从欢迎页焦点切换恢复并且只确认一次时返回零。
/// @details 授权关闭的同一帧启动更新弹窗，稍后提交的欢迎页夺走焦点。
/// 更新弹窗跨帧恢复时与真实 UI 一样调用生产的打开和确认函数。
/// 后续只有显式重查才产生新通知，已经展示并关闭的弹窗不能自行重开。
/// 测试仅装配 ImGui 上下文，无需平台窗口、渲染器或用户配置。
int main()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    // 不读取或写入用户布局，测试结束不会改变任何实际桌面窗口。
    io.IniFilename = nullptr;
    io.DisplaySize = { 800, 600 };
    // 固定帧间隔只推进 ImGui 状态，测试不通过 sleep 等待异步网络。
    io.DeltaTime = 1.0f / 60.0f;
    // 无平台渲染器消费纹理任务，测试主动准备默认字体图集供 NewFrame 使用。
    unsigned char* pixels{};
    int            width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 测试必须静音；统计实际执行的展示确认，覆盖提示音调用的同一条件。
    MMM::UI::SetInteractionFeedbackEnabled(false);
    // 模拟一次已经完成的检查事件，不在每帧按持续终态重新置位。
    bool pending = true;
    bool passed  = true;
    int  confirmations{};
    for ( int frame = 0; frame < 12; ++frame ) {
        ImGui::NewFrame();
        // 两个模态共用固定宿主，覆盖同层弹窗之间的竞争。
        ImGui::Begin("StablePopupHost");
        if ( frame == 0 ) MMM::UI::FeedbackOpenPopup("Consent");
        if ( ImGui::BeginPopupModal("Consent") ) {
            // 模态正文由正常 Begin/End 维持，不能通过测试手工伪造栈状态。
            // 先关闭授权再尝试更新，允许两者在该帧直接完成交接。
            if ( frame == 2 ) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        // 用户明确重查是第二个事件，不能被第一次提示的取消永久屏蔽。
        if ( frame == 7 ) pending = true;
        MMM::UI::Detail::openDeferredUpdatePopup("Update", pending);
        // 已有授权弹窗时保留通知，禁止把授权窗口从栈上替换掉。
        if ( frame < 2 )
            passed &= pending && ImGui::IsPopupOpen("Consent") &&
                      !ImGui::IsPopupOpen("Update");

        // 与生产中的更新日志弹窗一样指定尺寸，首次尝试也必须开始模态。
        ImGui::SetNextWindowSize({ 320, 240 }, ImGuiCond_Always);
        const bool beganUpdate = ImGui::BeginPopupModal("Update");
        if ( beganUpdate ) {
            ImGui::TextUnformatted("New version available");
            // 窗口可连续绘制多帧，确认次数只记录事件的消费边沿。
            if ( MMM::UI::Detail::confirmDeferredUpdatePopup(pending) )
                ++confirmations;
            // 用户点击稍后再说后停止展示；下一轮需要重新登记。
            if ( frame == 5 || frame == 9 ) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // 欢迎页在更新窗口完成首次 Begin 后显式抢焦点，重现桌面启动顺序。
        if ( frame == 2 )
            passed &= beganUpdate && pending && confirmations == 0;
        // 第一次自动通知必须成功展示，不能靠手动重查补偿。
        if ( frame == 6 ) passed &= confirmations == 1;
        // 在没有新事件的帧保留检查结果，但不留待展示的通知。
        if ( frame == 6 || frame >= 10 )
            passed &= !pending && !ImGui::IsPopupOpen("Update");
        ImGui::End();
        if ( frame == 2 ) {
            // 独立窗口模拟欢迎页，焦点调用不来自更新弹窗的子级。
            ImGui::SetNextWindowFocus();
            ImGui::Begin("Welcome");
            ImGui::End();
            // 断言本帧的模态确实被移除，防止测试只覆盖无竞争的打开路径。
            passed &= !ImGui::IsPopupOpen(nullptr,
                                          ImGuiPopupFlags_AnyPopupId |
                                              ImGuiPopupFlags_AnyPopupLevel) &&
                      pending;
        }
        // 使用真实 Render 结束当前帧，再继续处理栈与窗口的后续状态。
        ImGui::Render();
    }
    // 首次被抢掉的弹窗没有确认，重试和用户重查各确认一次。
    // 确认次数同时限制永久丢失通知和重复播放提示音两种退化。
    passed &= confirmations == 2;
    ImGui::DestroyContext();
    if ( !passed )
        XERROR("Deferred update popup reopened or displaced another modal");
    return passed ? 0 : 1;
}
