/// @file StartupPagePreview.cpp
/// @brief 使用真实启动页渲染固定的离线进度，供开发时人工检查动效与交互。
#include "main/StartupProgressDialog.h"
#include "main/StartupWindow.h"

#include "graphic/imguivk/IGraphicUserHook.h"
#include "graphic/imguivk/VKRenderer.h"
#include "log/colorful-log.h"
#include "network/AssetSyncService.h"
#include "ui/utils/UIWidgetUtils.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace
{

/// @brief 模拟下载的终态，重试后使用正常完成路径。
/// @note 启动参数只改变最后的资源状态，所有模式都先完整播放 Logo。
enum class PreviewResult { Ready, Error, Warning };

/// @brief 资源包大小仅用于展示整数 MB 和进度条，不对应实际文件。
constexpr std::int64_t PACKAGE_BYTES = 128 * 1024 * 1024;
/// @brief 每轮先分四步模拟下载，再单独展示解压和完成。
constexpr int DOWNLOAD_STEPS = 4;
/// @brief 终态后停止发布，保留窗口与日志供人工检查。
constexpr int TOTAL_STEPS = DOWNLOAD_STEPS + 2;

/// @brief 从固定步序号生成正式 UI 可消费的资源同步快照。
/// @param step 从零开始的步骤，下标范围为零至 TOTAL_STEPS 之前。
/// @details 四个更新包使用同一个已知总量，便于检查文字和进度条是否一致。
/// @warning 仅在模拟步骤边沿调用，不逐帧创建或发布字符串。
/// 每个快照由同一生产函数生成，保持字节、阶段和详情文案对应。
/// 调用方保持序号单调递增。重试切回首阶段时保留对话框的详情展开状态。
MMM::Network::AssetSyncProgress sampleProgress(int step)
{
    using Stage = MMM::Network::AssetSyncProgressStage;
    if ( step < DOWNLOAD_STEPS ) {
        // 逐步增大已下载字节数，最后一次下载快照明确到达总量。
        return { .stage        = Stage::kDownloadingPackage,
                 .message      = "测试资源包",
                 .currentBytes = PACKAGE_BYTES * (step + 1) / DOWNLOAD_STEPS,
                 .totalBytes   = PACKAGE_BYTES };
    }

    // 解压和成功均无独立字节计数，由正式视图按阶段计算进度。
    if ( step == DOWNLOAD_STEPS )
        return { .stage = Stage::kExtractingPackage, .message = "模拟解压" };
    return { .stage = Stage::kFinished, .message = "模拟更新完成" };
}

/// @brief 按正式的 Logo 结束信号启动模拟进度，保留窗口以便检查当前状态。
/// @warning
/// 预览渲染热路径：只读取时钟和状态，不等待模拟阶段或调用资源同步服务。
void runPreview(MMM::Main::StartupWindow& startup, PreviewResult result)
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    // 对话框先于调用方的窗口释放，Logo 管线不会借用已经销毁的设备。
    MMM::Main::StartupProgressDialog               dialog;
    std::array<MMM::Graphic::IGraphicUserHook*, 1> hooks{ &dialog };
    // 默认时点表示还没进入资源阶段，首帧在正式动画结束后建立截止时间。
    // 截止时间仅用于将样例逐项展开，不作为 Windows 或 Vulkan 同步手段。
    constexpr auto    stepDuration = 650ms;
    Clock::time_point nextStep{};
    int               step     = 0;
    auto&             window   = startup.window();
    auto&             renderer = startup.context().getRenderer();

    // 清单检查必须在真实 Shader 终帧提交之后开始，与应用顺序一致。
    // 用时钟比较调度步骤，窗口事件和按钮仍在每帧处理。
    while ( !window.shouldClose() && !dialog.isExitRequested() ) {
        renderer.render(window, hooks);
        if ( dialog.isIntroPlaying() ) continue;
        // 继续只完成这次预览，无需创建正式编辑器，方便单测警告确认。
        if ( dialog.consumeContinueRequest() ) return;

        const auto now = Clock::now();
        if ( nextStep == Clock::time_point{} || dialog.consumeRetryRequest() ) {
            // 重试保留同一个窗口和 Logo 终态，只重置同步模型。
            if ( nextStep != Clock::time_point{} )
                result = PreviewResult::Ready;
            dialog.beginSync();
            step     = 0;
            nextStep = now + stepDuration;
        } else if ( step < TOTAL_STEPS && now >= nextStep ) {
            // 错误模拟下载中断，警告模拟已有资源可以离线继续。
            if ( result == PreviewResult::Error && step == 2 ) {
                // 进入错误后暂停样例序列，直到用户显式选择重试或退出。
                dialog.showError("模拟下载失败", "可切换线路后点击重试。");
                step = TOTAL_STEPS;
            } else {
                // 最后一个成功快照之后便不再写入同步模型，窗口可持续检查。
                // 离线提示用同一个完成进度，再由界面决定继续按钮的位置。
                dialog.update(sampleProgress(step++));
                if ( step == TOTAL_STEPS && result == PreviewResult::Warning )
                    dialog.showWarning("模拟离线启动", "可使用已有资源继续。");
            }
            // 下一阶段将在普通帧里被观察到；慢帧也不跳过任何一个样例。
            nextStep = now + stepDuration;
        }
    }
}

}  // namespace

/// @brief 只运行启动窗口。可通过参数切换可重试错误和可继续警告。
/// @return 参数或窗口初始化失败时返回非零，用户正常退出时返回零。
/// @details 配置根由公共测试启动代码隔离。窗口初始化只加载 Bootstrap
/// 资源，入口不加载正式皮肤、工程或资源包，也不发出下载请求。
/// @note 预览是手动工具，结束条件为关闭、退出或确认离线提示。
int main(int argc, char* argv[])
{
    // 参数错误和帮助在初始化图形系统前处理，允许从无窗口终端查询用法。
    auto result = PreviewResult::Ready;
    if ( argc > 1 ) {
        const std::string_view mode = argv[1];
        if ( argc == 2 && mode == "--error" )
            result = PreviewResult::Error;
        else if ( argc == 2 && mode == "--warning" )
            result = PreviewResult::Warning;
        else {
            XINFO("Usage: StartupPagePreview [--error | --warning]");
            return argc == 2 && mode == "--help" ? 0 : 2;
        }
    }

    // 与主程序的启动阶段一致，未加载皮肤音频时关闭按钮音效。
    MMM::UI::SetInteractionFeedbackEnabled(false);
    MMM::Main::StartupWindow startup("MusicMapMaker - Startup Page Preview");
    if ( !startup.isReady() ) return 1;
    runPreview(startup, result);
    return 0;
}
