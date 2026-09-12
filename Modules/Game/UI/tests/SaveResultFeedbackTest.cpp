#include "ui/imgui/feedback/SaveResultFeedback.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveProgressEvent.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "ui/imgui/status/IStatusMessageSink.h"

#include "imgui.h"

#include <string>

/// @file SaveResultFeedbackTest.cpp
/// @brief 保存反馈在长帧、文件操作遮罩、失败优先级和过期清理下的回归测试。
/// @details 测试建立无平台后端的最小 ImGui 上下文，通过前景 DrawList 顶点数
/// 判断是否绘制，不访问真实项目或文件系统。

namespace
{

/// @brief 测试中接收状态栏消息的空实现。
class TestStatusMessageSink final : public MMM::UI::IStatusMessageSink
{
public:
    /// @brief 忽略本测试不涉及的状态栏消息。
    /// @details 不保存状态栏文本，因为本文件只观察前景 DrawList。
    /// @param message 状态消息文本。
    /// @param durationSeconds 显示时长。
    void showStatusMessage(std::string message, float durationSeconds) override
    {
        (void)message;
        (void)durationSeconds;
    }
};

/// @brief 验证长帧中收到的打包成功事件仍会获得完整的首次绘制机会。
/// @return 前景绘制列表生成反馈气泡几何时返回 true。
/// @note 五秒 delta 大于成功气泡时限，用于捕获先计时后消费事件的错误顺序。
bool testLongFrameDoesNotExpireNewPackageFeedback()
{
    // 反馈器在构造时订阅事件总线，发布结果会进入其私有队列。
    MMM::UI::SaveResultFeedback feedback;
    TestStatusMessageSink       statusMessageSink;
    MMM::Event::EventBus::instance().publish(MMM::Event::BeatmapSaveResultEvent{
        .path     = "/tmp/feedback-test.mcz",
        .success  = true,
        .isExport = true,
    });

    // 模拟原生文件对话框返回后的长帧，再消费刚到达的成功结果。
    feedback.update(5.0F, statusMessageSink);

    // 首次绘制必须仍产生气泡几何，不能被同一长帧直接扣完时限。
    ImGui::NewFrame();
    feedback.render(1.0F);
    // 在结束帧前读取前景顶点，结果不依赖实际 GPU 后端。
    const bool rendered = ImGui::GetForegroundDrawList()->VtxBuffer.Size > 0;
    ImGui::EndFrame();
    return rendered;
}

}  // namespace

/// @brief 验证耗时操作持续绘制、结束清理以及失败反馈不被进度结束吞掉。
/// @return 忙碌遮罩与失败气泡均绘制、过期后清空时返回 true。
bool testFileOperationProgress()
{
    // 使用独立反馈器，避免继承上一场景尚未过期的气泡状态。
    MMM::UI::SaveResultFeedback feedback;
    TestStatusMessageSink       sink;
    auto&                       bus = MMM::Event::EventBus::instance();
    // 阶段事件默认 active，驱动全屏文件操作遮罩。
    bus.publish(MMM::Event::BeatmapSaveProgressEvent{ .stage = "Encoding" });
    // 即使 delta 很大，门闩占用期间遮罩仍必须绘制。
    feedback.update(30.0F, sink);
    ImGui::NewFrame();
    feedback.render(1.0F, true);
    const bool busyRendered =
        ImGui::GetForegroundDrawList()->VtxBuffer.Size > 0;
    // 结束当前帧后再发布最终结果，模拟真实文件任务跨帧完成。
    ImGui::EndFrame();
    // 文件操作失败结果先于结束阶段入队，队列顺序必须保留两者。
    bus.publish(MMM::Event::BeatmapSaveResultEvent{
        .path = "test.zip", .success = false, .isExport = true });
    bus.publish(MMM::Event::BeatmapSaveProgressEvent{ .active = false });
    // 门闩释放后应显示失败气泡，而不是被 progressActive=false 覆盖。
    feedback.update(30.0F, sink);
    ImGui::NewFrame();
    feedback.render(1.0F);
    const bool failureRendered =
        ImGui::GetForegroundDrawList()->VtxBuffer.Size > 0;
    // 完成首次失败反馈帧后再推进过期时间。
    ImGui::EndFrame();
    // 失败气泡三秒有效，推进四秒后不应再生成前景顶点。
    feedback.update(4.0F, sink);
    ImGui::NewFrame();
    feedback.render(1.0F);
    const bool cleared = ImGui::GetForegroundDrawList()->VtxBuffer.Size == 0;
    // 即使没有绘制几何也要正常结束帧，保持上下文状态平衡。
    ImGui::EndFrame();
    // 三个阶段必须全部满足才证明遮罩到最终反馈的完整生命周期。
    return busyRendered && failureRendered && cleared;
}

/// @brief 运行保存与打包结果反馈的长帧回归测试。
/// @return 测试通过时返回 0。
/// @note 字体图集需要在首个 NewFrame 前构建，测试禁用 ini 文件持久化。
int main()
{
    // 测试直接驱动 ImGui 帧，先校验头文件与库版本一致。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // 固定显示尺寸和帧间隔，避免依赖宿主窗口环境。
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0F, 600.0F);
    io.DeltaTime   = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    // 构建默认字体纹理数据以满足 ImGui 无后端帧的前置条件。
    unsigned char* fontPixels = nullptr;
    int            fontWidth  = 0;
    int            fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    // 字体准备失败时不进入绘制场景，最终仍统一销毁上下文。
    const bool fontReady = fontPixels && fontWidth > 0 && fontHeight > 0;
    const bool valid     = fontReady &&
                           testLongFrameDoesNotExpireNewPackageFeedback() &&
                           testFileOperationProgress();
    // 所有测试对象先于上下文销毁，确保析构订阅时 ImGui 状态仍有效。
    ImGui::DestroyContext();
    return valid ? 0 : 1;
}
