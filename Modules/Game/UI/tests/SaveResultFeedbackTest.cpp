#include "ui/imgui/feedback/SaveResultFeedback.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "ui/imgui/status/IStatusMessageSink.h"

#include "imgui.h"

#include <string>

namespace
{

/// @brief 测试中接收状态栏消息的空实现。
class TestStatusMessageSink final : public MMM::UI::IStatusMessageSink
{
public:
    /// @brief 忽略本测试不涉及的状态栏消息。
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
bool testLongFrameDoesNotExpireNewPackageFeedback()
{
    MMM::UI::SaveResultFeedback feedback;
    TestStatusMessageSink       statusMessageSink;
    MMM::Event::EventBus::instance().publish(MMM::Event::BeatmapSaveResultEvent{
        .path     = "/tmp/feedback-test.mcz",
        .success  = true,
        .isExport = true,
    });

    feedback.update(5.0F, statusMessageSink);

    ImGui::NewFrame();
    feedback.render(1.0F);
    const bool rendered = ImGui::GetForegroundDrawList()->VtxBuffer.Size > 0;
    ImGui::EndFrame();
    return rendered;
}

}  // namespace

/// @brief 运行保存与打包结果反馈的长帧回归测试。
/// @return 测试通过时返回 0。
int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io               = ImGui::GetIO();
    io.DisplaySize            = ImVec2(800.0F, 600.0F);
    io.DeltaTime              = 1.0F / 60.0F;
    io.IniFilename            = nullptr;
    unsigned char* fontPixels = nullptr;
    int            fontWidth  = 0;
    int            fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    const bool fontReady = fontPixels && fontWidth > 0 && fontHeight > 0;
    const bool valid =
        fontReady && testLongFrameDoesNotExpireNewPackageFeedback();
    ImGui::DestroyContext();
    return valid ? 0 : 1;
}
