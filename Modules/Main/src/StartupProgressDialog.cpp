#include "main/StartupProgressDialog.h"
#include "config/AppConfig.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace MMM::Main
{
namespace
{
/// @brief 确保 DPI 缩放为有限正数。
/// @param scale 待检查缩放值。
/// @return 可安全用于 ImGui 布局的缩放值。
float sanitizedScale(float scale)
{
    // 配置可能来自旧版本或损坏文件，布局层只接受可参与乘法的正数。
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}
}  // namespace

/// @brief 重置一次资源同步所需的进度、提示和用户操作状态。
/// @warning 启动低频路径：只能在旧同步线程已经停止后由渲染主线程调用。
void StartupProgressDialog::beginSync()
{
    // 每轮都从清单检查开始，避免重试期间暂时显示上一轮的末尾阶段。
    Network::AssetSyncProgress initialProgress;
    initialProgress.stage = Network::AssetSyncProgressStage::kCheckingManifest;
    initialProgress.message = "正在检查资源更新...";

    {
        // 后台同步可能很快启动，先在同一把锁下建立完整的发布快照。
        std::scoped_lock lock(m_progressMutex);
        m_pendingProgress = initialProgress;
    }
    // 主线程可直接采用初值，不必制造一次无意义的脏快照消费。
    m_visibleProgress = initialProgress;
    // dirty=false 与直接可见初值配套，首帧无需争用进度互斥量。
    m_progressDirty.store(false, std::memory_order_release);
    // 错误和警告互斥；新一轮同步必须清空两种终止界面。
    m_hasError   = false;
    m_hasWarning = false;
    // 按钮请求是一次性边沿状态，重试时不能继承此前点击结果。
    m_retryRequested    = false;
    m_continueRequested = false;
    m_exitRequested     = false;
    // 清理文本释放旧提示语义，后续状态只读取与当前标志对应的字段。
    m_errorTitle.clear();
    m_errorMessage.clear();
    // 两套文本分别归属互斥状态，清空可防止调试器观察到陈旧提示。
    m_warningTitle.clear();
    m_warningMessage.clear();
}

/// @brief 从后台同步线程发布最新的进度快照。
/// @param progress 新阶段、计数与说明文字组成的完整快照。
/// @warning 跨线程低频路径：只复制状态并发布脏位，不得调用 ImGui。
void StartupProgressDialog::update(const Network::AssetSyncProgress& progress)
{
    {
        // 快照整体受锁保护，渲染线程不会观察到阶段和计数的混合版本。
        std::scoped_lock lock(m_progressMutex);
        m_pendingProgress = progress;
    }
    // release 保证先完成快照写入，再让渲染线程看见待消费标志。
    m_progressDirty.store(true, std::memory_order_release);
}

/// @brief 切换到允许退出或重试的错误界面。
/// @param title 面向用户的错误摘要。
/// @param message 可换行展示的诊断详情。
/// @warning 启动低频路径：仅由渲染主线程更新界面状态。
void StartupProgressDialog::showError(std::string title, std::string message)
{
    // 先合并最后一份后台进度，使错误发生前的状态保持一致。
    consumePendingProgress();
    // 错误和可继续警告不能同时展示，分支状态在此一次性切换。
    m_hasError   = true;
    m_hasWarning = false;
    // move 接管调用方临时字符串，错误路径不额外复制可能很长的诊断文本。
    m_errorTitle   = std::move(title);
    m_errorMessage = std::move(message);
    // 新错误必须等待本轮用户选择，不能复用之前按钮的边沿请求。
    m_retryRequested    = false;
    m_continueRequested = false;
}

/// @brief 切换到允许退出或继续的非致命警告界面。
/// @param title 面向用户的警告摘要。
/// @param message 需要用户确认的完整说明。
/// @warning 启动低频路径：仅由渲染主线程更新界面状态。
void StartupProgressDialog::showWarning(std::string title, std::string message)
{
    // 警告覆盖进度界面前先固定最后一次后台状态，便于后续继续启动。
    consumePendingProgress();
    // 清除错误分支确保 onUpdateUI 每帧只执行一种交互路径。
    m_hasError   = false;
    m_hasWarning = true;
    // 标题和正文一起切换，渲染分支不会读取一新一旧的提示组合。
    m_warningTitle   = std::move(title);
    m_warningMessage = std::move(message);
    // 提示刚出现时尚未获得用户决策，两个请求都从 false 开始。
    m_retryRequested    = false;
    m_continueRequested = false;
}

/// @brief 读取并清除一次重试按钮请求。
/// @return 上次消费后用户是否点击过重试。
bool StartupProgressDialog::consumeRetryRequest()
{
    // 采用边沿消费语义，主循环不会对同一次点击重复启动同步任务。
    const bool requested = m_retryRequested;
    m_retryRequested     = false;
    return requested;
}

/// @brief 读取并清除一次继续按钮请求。
/// @return 上次消费后用户是否确认忽略当前警告。
bool StartupProgressDialog::consumeContinueRequest()
{
    // 与重试请求保持相同的一次性语义，调用者无需额外复位界面状态。
    const bool requested = m_continueRequested;
    m_continueRequested  = false;
    return requested;
}

/// @brief 查询用户是否请求终止启动流程。
/// @return 任一启动界面的退出按钮被点击后返回 true。
bool StartupProgressDialog::isExitRequested() const
{
    // 退出属于粘滞状态，由拥有者结束流程，不在查询时自动清除。
    return m_exitRequested;
}

/// @brief 实现图形钩子的资源准备入口；启动界面没有专用 Vulkan 资源。
/// @warning 启动渲染热路径：保持为空，不得增加分配、等待或文件访问。
void StartupProgressDialog::onPrepareResources(vk::PhysicalDevice&, vk::Device&,
                                               Graphic::VKSwapchain&,
                                               vk::CommandPool&, vk::Queue&)
{
}

/// @brief 绘制当前资源同步进度、错误或警告状态。
/// @warning 启动渲染热路径：每帧调用，只允许消费快照并提交 ImGui 绘制。
void StartupProgressDialog::onUpdateUI()
{
    // 每帧至多获取一次进度锁，且没有新快照时仅执行一个原子读改写。
    consumePendingProgress();

    // 主视口尚未建立时无法确定覆盖区域，本帧安全跳过绘制。
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if ( !viewport ) return;

    // 所有像素尺寸统一采用配置中的内容缩放，非法配置回退到 1 倍。
    const float scale =
        sanitizedScale(Config::AppConfig::instance().getWindowContentScale());
    // 启动窗口完全覆盖工作区，不使用持久布局或用户拖拽结果。
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    // 临时样式只包围 Begin，窗口创建后立即恢复全局主题栈。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(30.0f * scale, 26.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * scale);
    // 边框保持一个样式单位，避免高 DPI 下外沿过度加粗。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    // 启动配色固定且不依赖尚未完成加载的用户皮肤。
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(0.055f, 0.064f, 0.092f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.30f, 0.50f, 0.85f));

    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    // ### 后缀固定内部 ID，显示标题变化也不会破坏窗口身份。
    const bool windowVisible = ImGui::Begin(
        "MusicMapMaker Startup###StartupAssetSync", nullptr, windowFlags);
    // 无论 Begin 返回值如何都平衡样式栈，避免污染后续 ImGui 窗口。
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if ( !windowVisible ) {
        // Begin 与 End 必须严格成对，折叠或裁剪也不能提前遗漏 End。
        ImGui::End();
        return;
    }

    // 顶部蓝色细条直接写入窗口绘制列表，不额外创建子窗口或纹理。
    ImDrawList*  drawList  = ImGui::GetWindowDrawList();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowMax{ windowPos.x + ImGui::GetWindowWidth(),
                            windowPos.y + ImGui::GetWindowHeight() };
    // 内缩一个像素避免强调条覆盖边框，顶部圆角与窗口轮廓一致。
    drawList->AddRectFilled(
        ImVec2(windowPos.x + 1.0f, windowPos.y + 1.0f),
        ImVec2(windowMax.x - 1.0f, windowPos.y + 4.0f * scale),
        IM_COL32(74, 134, 230, 255),
        12.0f * scale,
        ImDrawFlags_RoundCornersTop);

    ImGui::TextColored(ImVec4(0.55f, 0.72f, 1.0f, 1.0f), "MUSIC MAP MAKER");

    if ( m_hasError ) {
        // 错误分支优先于警告和进度，提供明确的退出与重试决策。
        ImGui::TextColored(
            ImVec4(1.0f, 0.63f, 0.60f, 1.0f), "%s", m_errorTitle.c_str());
        ImGui::Separator();

        // 详情随可用宽度换行，长网络错误不会横向撑大窗口。
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                               ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(m_errorMessage.c_str());
        ImGui::PopTextWrapPos();

        // 两个动作使用固定逻辑宽度，翻译文字变化不会造成按钮跳动。
        const float buttonWidth = 118.0f * scale;
        const float buttonGap   = 12.0f * scale;
        const float rowWidth    = buttonWidth * 2.0f + buttonGap;
        // 按钮组靠右对齐；窄窗口用 max 防止游标向左越过内容起点。
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(ImGui::GetContentRegionAvail().x - rowWidth, 0.0f));
        if ( UI::FeedbackButton("退出###StartupExit",
                                ImVec2(buttonWidth, 0.0f)) ) {
            // 退出请求保持为 true，由外层启动循环统一终止同步和窗口。
            m_exitRequested = true;
        }
        ImGui::SameLine(0.0f, buttonGap);
        // 重试位于右侧，作为错误恢复的主要动作。
        if ( UI::FeedbackButton("重试###StartupRetry",
                                ImVec2(buttonWidth, 0.0f)) ) {
            // 重试采用一次性请求，下一帧由启动状态机重新创建同步任务。
            m_retryRequested = true;
        }
    } else if ( m_hasWarning ) {
        // 警告允许继续启动，因此与必须修复的错误使用独立按钮语义。
        ImGui::TextColored(
            ImVec4(1.0f, 0.78f, 0.36f, 1.0f), "%s", m_warningTitle.c_str());
        ImGui::Separator();

        const float buttonHeight = ImGui::GetFrameHeight();
        // 详情子区域占用按钮行之外的空间，并保留最小可阅读高度。
        const float messageHeight =
            std::max(ImGui::GetContentRegionAvail().y - buttonHeight -
                         ImGui::GetStyle().ItemSpacing.y,
                     80.0f * scale);
        if ( ImGui::BeginChild("TranslationOverrideWarningDetails",
                               ImVec2(0.0f, messageHeight),
                               ImGuiChildFlags_Borders) ) {
            // 子窗口允许长提示滚动，文本宽度始终跟随当前内容区。
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                   ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(m_warningMessage.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();

        // 退出与继续保持等宽，避免把继续选择误表现为无条件推荐。
        const float buttonWidth = 118.0f * scale;
        const float buttonGap   = 12.0f * scale;
        const float rowWidth    = buttonWidth * 2.0f + buttonGap;
        // 警告按钮沿用错误分支的宽度和对齐，降低启动界面的布局跳动。
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(ImGui::GetContentRegionAvail().x - rowWidth, 0.0f));
        if ( UI::FeedbackButton("退出###StartupWarningExit",
                                ImVec2(buttonWidth, 0.0f)) ) {
            m_exitRequested = true;
        }
        ImGui::SameLine(0.0f, buttonGap);
        // 继续只在非致命警告分支出现，错误状态不会提供绕过入口。
        if ( UI::FeedbackButton("继续###StartupWarningContinue",
                                ImVec2(buttonWidth, 0.0f)) ) {
            // 继续请求只确认当前警告，不在视图层直接启动主应用。
            m_continueRequested = true;
        }
    } else {
        // 正常分支只展示已消费的稳定快照，不直接访问后台写入对象。
        ImGui::TextUnformatted("正在准备应用资源");
        ImGui::TextColored(ImVec4(0.66f, 0.70f, 0.80f, 1.0f),
                           "%s",
                           progressText(m_visibleProgress).c_str());

        // 阶段函数负责把不同同步策略映射到同一条单调视觉进度轴。
        const float fraction = progressFraction(m_visibleProgress);
        // 百分比仅用于展示，四舍五入不会反向参与同步状态判断。
        const std::string overlay =
            std::to_string(static_cast<int>(std::round(fraction * 100.0f))) +
            "%";
        // overlay 的存储覆盖本次 ProgressBar 调用，传入的 c_str 不会悬空。
        ImGui::ProgressBar(
            fraction, ImVec2(-1.0f, ImGui::GetFrameHeight()), overlay.c_str());
        // 网络提示保持为次要灰色，不与阶段说明和百分比争夺注意力。
        ImGui::TextColored(ImVec4(0.48f, 0.53f, 0.64f, 1.0f),
                           "资源校验和更新期间请保持网络连接");

        const float exitButtonWidth = 96.0f * scale;
        // 单按钮同样靠右，保持与错误和警告操作区一致的视觉锚点。
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(ImGui::GetContentRegionAvail().x - exitButtonWidth, 0.0f));
        if ( UI::FeedbackButton("退出###StartupCancel",
                                ImVec2(exitButtonWidth, 0.0f)) ) {
            // 取消同步由外层状态机执行，UI 不在渲染回调里阻塞等待线程。
            m_exitRequested = true;
        }
    }
    // 所有状态分支共享同一次 Begin，统一在函数末尾结束窗口。
    ImGui::End();
}

/// @brief 实现空的离屏命令录制钩子。
/// @warning 启动渲染热路径：启动界面完全由 ImGui 主通道绘制。
void StartupProgressDialog::onRecordOffscreen(vk::CommandBuffer&, uint32_t) {}

/// @brief 返回启动界面需要并行录制的离屏任务数量。
/// @return 始终返回 0，因为没有离屏渲染资源。
/// @warning 启动渲染热路径：只返回常量，不得查询设备或线程池。
uint32_t StartupProgressDialog::getOffscreenRecordTaskCount() const
{
    return 0;
}

/// @brief 将后台发布的最新进度复制到渲染线程可见状态。
/// @warning 启动渲染热路径：无更新时不加锁，有更新时仅复制一个快照。
void StartupProgressDialog::consumePendingProgress()
{
    // acq_rel 同时消费后台 release 写入，并原子清除本轮脏位。
    if ( !m_progressDirty.exchange(false, std::memory_order_acq_rel) ) {
        return;
    }

    // 锁内只做值复制，避免渲染线程等待网络或文件系统操作。
    std::scoped_lock lock(m_progressMutex);
    m_visibleProgress = m_pendingProgress;
}

/// @brief 将资源同步的离散阶段和局部计数映射到统一进度比例。
/// @param progress 当前渲染线程持有的稳定进度快照。
/// @return 限制在 0 到 1 区间的展示比例。
float StartupProgressDialog::progressFraction(
    const Network::AssetSyncProgress& progress)
{
    // 包下载按字节计算；未知总量时保持阶段起点而不猜测比例。
    const auto byteFraction = [&progress]() {
        if ( progress.totalBytes <= 0 ) return 0.0;
        // clamp 防御服务端计数异常或完成瞬间的短暂超界。
        return std::clamp(static_cast<double>(progress.currentBytes) /
                              static_cast<double>(progress.totalBytes),
                          0.0,
                          1.0);
    };
    // 逐文件校验使用文件序号，零文件清单同样回落到阶段起点。
    const auto fileFraction = [&progress]() {
        if ( progress.totalFileCount == 0 ) return 0.0;
        // 服务报告的序号可能采用阶段边界值，统一限制后再参与映射。
        return std::clamp(static_cast<double>(progress.currentFileIndex) /
                              static_cast<double>(progress.totalFileCount),
                          0.0,
                          1.0);
    };

    // 各区间预留阶段切换反馈，避免检查、下载和解压显示同一百分比。
    switch ( progress.stage ) {
    // 清单检查只占初始小段，给用户立即可见的启动反馈。
    case Network::AssetSyncProgressStage::kCheckingManifest: return 0.06f;
    case Network::AssetSyncProgressStage::kDownloadingPackage:
        // 整包下载覆盖主要区间，字节进度在其中线性展开。
        return static_cast<float>(0.08 + byteFraction() * 0.84);
    // 解压缺少细粒度计数，以接近完成但未结束的固定位置表示。
    case Network::AssetSyncProgressStage::kExtractingPackage: return 0.96f;
    case Network::AssetSyncProgressStage::kCheckingFiles:
        // 增量校验只占前段，为后续逐文件下载保留大部分空间。
        return static_cast<float>(0.08 + fileFraction() * 0.34);
    case Network::AssetSyncProgressStage::kDownloadingFile: {
        // 当前文件序号包含正在处理项，先减一得到已经完整结束的文件数。
        const double completedFiles =
            progress.currentFileIndex > 0
                ? static_cast<double>(progress.currentFileIndex - 1)
                : 0.0;
        // 至少以一为分母，损坏或尚未发布的总数不会产生除零。
        const double totalFiles =
            std::max(static_cast<double>(progress.totalFileCount), 1.0);
        // 已完成文件与当前文件字节比例组合成连续的整体下载进度。
        const double combined = (completedFiles + byteFraction()) / totalFiles;
        // 下载阶段映射到 42% 至 94%，为收尾和界面切换留下余量。
        return static_cast<float>(0.42 + std::clamp(combined, 0.0, 1.0) * 0.52);
    }
    // 完成阶段显式返回满值，避免浮点映射停留在接近 100%。
    case Network::AssetSyncProgressStage::kFinished: return 1.0f;
    }
    // 防御未来新增枚举未同步处理时出现未定义展示值。
    return 0.0f;
}

/// @brief 根据同步阶段生成面向用户的简短进度说明。
/// @param progress 当前渲染线程持有的稳定进度快照。
/// @return 含可用计数信息的本地化启动提示。
std::string StartupProgressDialog::progressText(
    const Network::AssetSyncProgress& progress)
{
    // 阶段文本与 progressFraction 使用同一枚举分派，保证描述和进度一致。
    switch ( progress.stage ) {
    case Network::AssetSyncProgressStage::kCheckingManifest:
        // 清单阶段不依赖服务端消息，保持首次反馈稳定。
        return "正在检查资源更新...";
    case Network::AssetSyncProgressStage::kDownloadingPackage:
        // 仅在总字节可靠时展示数值，避免出现无意义的 0/0 MB。
        if ( progress.totalBytes > 0 ) {
            // 启动界面使用整数 MB 保持文案紧凑，不作为精确传输统计。
            return "正在下载资源包 " +
                   std::to_string(progress.currentBytes / 1024 / 1024) + "/" +
                   std::to_string(progress.totalBytes / 1024 / 1024) + " MB";
        }
        // 总量未知时省略计数，避免展示无法解释的零值。
        return "正在下载资源包...";
    case Network::AssetSyncProgressStage::kExtractingPackage:
        // 解压服务没有连续计数，使用阶段描述配合固定进度位置。
        return "正在解压资源包...";
    case Network::AssetSyncProgressStage::kCheckingFiles:
        // 文件总数可用时显示清单位置，帮助识别大型资源集仍在推进。
        if ( progress.totalFileCount > 0 ) {
            return "正在校验本地资源 " +
                   std::to_string(progress.currentFileIndex) + "/" +
                   std::to_string(progress.totalFileCount);
        }
        // 总文件数尚未发布时只描述当前阶段。
        return "正在校验本地资源...";
    case Network::AssetSyncProgressStage::kDownloadingFile:
        // 单文件更新直接显示当前项和总数，字节细节由进度条承担。
        // 索引由同步服务提供，文本层不自行修正以免与日志指向不同文件。
        return "正在更新资源文件 " + std::to_string(progress.currentFileIndex) +
               "/" + std::to_string(progress.totalFileCount);
    case Network::AssetSyncProgressStage::kFinished:
        // 资源完成后仍可能创建主窗口，因此文案说明正在进入应用。
        return "资源已经准备完成，正在启动...";
    }
    // 未识别阶段优先保留服务提供的消息，为协议扩展提供可读回退。
    // 该分支也让新增阶段在正式本地化前仍可显示服务侧诊断。
    return progress.message;
}

}  // namespace MMM::Main
