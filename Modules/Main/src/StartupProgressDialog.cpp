#include "main/StartupProgressDialog.h"
#include "config/AppConfig.h"
#include "graphic/window/StartupWindowAppearance.h"
#include "main/StartupLogoShader.h"
#include "ui/utils/DownloadRouteSelector.h"
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
/// @brief 完整播放后才能开始资源同步的 Logo 入场时长。
constexpr float INTRO_DURATION = 1.6f;

/// @brief 启动窗口保留最近的状态变化，逐文件下载不会无界积累。
constexpr std::size_t MAX_LOG_ENTRIES = 16;

}  // namespace

/// @brief 动画只在窗口首次出现时播放，后续重试复用同一渲染状态。
StartupProgressDialog::StartupProgressDialog() = default;

/// @brief 由启动窗口所有者按设备生命周期释放 GPU 资源。
StartupProgressDialog::~StartupProgressDialog() = default;

/// @brief 重置一次资源同步所需的进度、提示和用户操作状态。
/// @warning 启动低频路径：只能在旧同步线程已经停止后由渲染主线程调用。
void StartupProgressDialog::beginSync()
{
    // 第一轮只会在 Logo 终点帧提交后进入，重试不重置动画时钟。
    m_syncStarted = true;
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
    m_progressLabel   = progressText(m_visibleProgress);
    // 每轮同步从清单检查重新记日志，详情开合由用户保留。
    m_logs.clear();
    m_logs.emplace_back(m_progressLabel);
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

/// @brief 查询 Logo 是否仍处于资源同步之前的独立动画阶段。
/// @warning 启动每帧调用；只读同一渲染线程维护的时间。
bool StartupProgressDialog::isIntroPlaying() const
{
    return !m_introPresented;
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

/// @brief 在首次启动绘制前准备独立的 Logo shader。
/// @warning 检查每帧执行；分配只发生在第一次，管线失败也不会反复尝试。
void StartupProgressDialog::onPrepareResources(
    vk::PhysicalDevice& physicalDevice, vk::Device& device,
    Graphic::VKSwapchain& swapchain, vk::CommandPool& commandPool,
    vk::Queue& queue)
{
    // 构造结果也作为已尝试标志，失败后进入静态回退而不反复创建。
    if ( !m_logoShader )
        m_logoShader = std::make_unique<StartupLogoShader>(
            physicalDevice, device, swapchain, commandPool, queue);
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
    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    // 固定 ID 与正式应用隔离，首次运行、重试及错误切换共用一个输入焦点。
    if ( !ImGui::Begin("MusicMapMaker Startup###StartupAssetSync",
                       nullptr,
                       windowFlags) ) {
        ImGui::End();
        return;
    }

    // 原生窗口保持无标题栏。背景层按完整客户区绘制，四角留给桌面合成，
    // 边线轻微内收以免 ImGui 的抗锯齿笔画被 framebuffer 边缘截断。
    const ImVec2 origin       = viewport->WorkPos;
    const ImVec2 area         = viewport->WorkSize;
    const float  cornerRadius = Graphic::STARTUP_WINDOW_CORNER_RADIUS * scale;
    auto*        frame        = ImGui::GetBackgroundDrawList(viewport);
    // 背景填充和描边使用同一尺寸，外圈像素由桌面合成或原生裁剪保留。
    // 在正文前提交边框，动画首帧和同步终态共享同一个窗口外形。
    frame->AddRectFilled(origin,
                         { origin.x + area.x, origin.y + area.y },
                         ImGui::GetColorU32(ImGuiCol_WindowBg),
                         cornerRadius);
    frame->AddRect({ origin.x + 1.0f, origin.y + 1.0f },
                   { origin.x + area.x - 1.0f, origin.y + area.y - 1.0f },
                   ImGui::GetColorU32(ImGuiCol_Text, 0.24f),
                   std::max(0.0f, cornerRadius - 1.0f));

    // 以逻辑宽度和可用比例决定布局。横屏让 Logo 和更新面板并列，
    // 手机竖屏在同一绘制路径回落为上下排列。
    const bool   horizontal = area.x >= 560.0f * scale && area.x > area.y;
    const ImVec2 center{ origin.x + area.x * (horizontal ? 0.205f : 0.5f),
                         origin.y + area.y * (horizontal ? 0.5f : 0.23f) };
    // 动画与进度阶段共享锚点，出现详情时 Logo 不移动；小视口按短边限幅。
    const float logoSize = std::min({ 252.0f * scale,
                                      area.x * (horizontal ? 0.34f : 0.74f),
                                      area.y * (horizontal ? 0.68f : 0.38f) });
    const float radius   = logoSize * 0.5f;

    // 先提交这一帧的时间再累积下一帧：首帧显示开场，掉帧也不会跳过显影。
    if ( !m_logoShader || !m_logoShader->isValid() ) {
        // GPU 管线无法使用时，以相同 SVG 的稳定帧继续资源检查。
        m_introElapsed = INTRO_DURATION;
    }
    if ( m_logoShader ) {
        m_logoShader->draw(center.x - radius,
                           center.y - radius,
                           logoSize,
                           m_introElapsed / INTRO_DURATION);
    }
    // 引导尚无按钮，通过 Esc 也可退出。外层收到请求后不会启动资源任务。
    // Esc 同样适用于还没有显示退出按钮的首个阶段。
    if ( ImGui::IsKeyPressed(ImGuiKey_Escape) ) m_exitRequested = true;
    if ( m_introElapsed < INTRO_DURATION ) {
        // 使用有限步长推进实际呈现的帧。动画期间不提交路线、进度或日志控件。
        m_introElapsed = std::min(
            INTRO_DURATION,
            m_introElapsed + std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f));
        ImGui::End();
        return;
    }

    // 本帧已登记静态终点。下一帧开始资源任务后才允许显示进度和操作。
    // 此握手不会把倒数第二帧误当作完全显影，也不会在重试时重新播放。
    m_introPresented = true;
    // 最后一帧独立展示标志，测试可通过服务端首个请求区分动画与同步阶段。
    if ( !m_syncStarted ) {
        ImGui::End();
        return;
    }

    const auto& style             = ImGui::GetStyle();
    const auto  padding           = style.WindowPadding;
    const float contentX          = horizontal ? area.x * 0.39f : padding.x;
    const float progressBarHeight = 6.0f * scale;
    const float rowGap            = style.ItemSpacing.y;
    // 状态文字和操作按钮始终存在。资源进度、下载线路只在对应状态加入；
    // 使用当前字体和边距，DPI 改变时仍以控件的实际行高对齐。
    // 状态行本身不含末尾留白，操作按钮使用普通 ImGui 帧。
    // 下方只加实际可见的两组控件，离线警告不参与线路下拉框计高。
    float controlsHeight =
        ImGui::GetTextLineHeight() + rowGap + ImGui::GetFrameHeight();
    if ( !m_hasError && !m_hasWarning )
        controlsHeight += rowGap + progressBarHeight;
    if ( !m_hasWarning )
        controlsHeight += ImGui::GetTextLineHeightWithSpacing() + rowGap +
                          ImGui::GetFrameHeight();

    // 展开时将日志与控件一起居中，日志高度受当前窗口限制。
    // 在较矮的视口优先让操作按钮可见，日志继续在剩余空间内滚动。
    // 用固定可见行数设定目标高度，后台日志条目数变化时面板不抖动。
    // 窗口不足时以外层内边距为界，滚动区不能把面板推出客户区。
    const float logPaddingY = 8.0f * scale;
    const float logHeight   = std::max(
        1.0f,
        std::min(8.0f * ImGui::GetTextLineHeightWithSpacing() + 2 * logPaddingY,
                 area.y - 2 * padding.y - controlsHeight - rowGap));
    // 捕获这一帧的展开态，避免按钮回调使新日志出现在上一种布局中。
    const bool  detailsExpanded = m_detailsExpanded;
    const float panelHeight =
        controlsHeight + (detailsExpanded ? rowGap + logHeight : 0.0f);
    // 横屏的面板与 Logo 共用同一水平中线。竖屏仍从 Logo 下方布局。
    const float contentY =
        horizontal ? std::max(padding.y, (area.y - panelHeight) * 0.5f)
                   : center.y - origin.y + radius * 0.62f + 14.0f * scale;
    const float contentWidth  = std::max(1.0f, area.x - contentX - padding.x);
    const float contentHeight = std::max(1.0f, area.y - contentY - padding.y);
    // 用子区域限制所有控件的宽度，复用既有线路选择器及其窄屏裁剪。
    // 子区域起点用屏幕坐标设置，后续 SameLine 仍以子区域的局部原点计算。
    ImGui::SetCursorScreenPos({ origin.x + contentX, origin.y + contentY });
    if ( !ImGui::BeginChild("StartupUpdateContent",
                            { contentWidth, contentHeight },
                            ImGuiChildFlags_None,
                            ImGuiWindowFlags_NoBackground) ) {
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    if ( m_hasError || m_hasWarning ) {
        // 错误与警告仍拥有原来的恢复动作，长正文放在详情区域。
        ImGui::TextUnformatted(m_hasError ? m_errorTitle.c_str()
                                          : m_warningTitle.c_str());
    } else {
        const float fraction = progressFraction(m_visibleProgress);
        const float rightEdge =
            ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        // 百分比与阶段分离，细条不会因内嵌文本截挡而降低对比度。
        ImGui::TextUnformatted(m_progressLabel.c_str());
        ImGui::SameLine(std::max(rightEdge - ImGui::CalcTextSize("100%").x,
                                 ImGui::GetItemRectMax().x -
                                     ImGui::GetWindowPos().x +
                                     ImGui::GetStyle().ItemSpacing.x));
        ImGui::Text("%d%%", static_cast<int>(std::round(fraction * 100.0f)));
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, progressBarHeight), "");
    }
    // 重用原有会话选线，失败也是换线时机；皮肤警告没有待下载内容。
    // 不在 UI 记录或覆盖实际选中的地址，后台下载仍按共用线路策略处理。
    if ( !m_hasWarning ) UI::drawDownloadRouteSelector(false, scale);

    if ( UI::FeedbackButton(detailsExpanded
                                ? "收起详情###StartupDetailsToggle"
                                : "展开详情###StartupDetailsToggle") )
        m_detailsExpanded = !detailsExpanded;

    // 退出和状态确认共用当前行右边界，保持展开折叠时的横向位置。
    const bool hasConfirm = m_hasError || m_hasWarning;
    // 狭窄视口仍给两个并排操作及详情折叠按钮分配各自的命中空间。
    const float buttonWidth = std::min(96.0f * scale, contentWidth * 0.26f);
    const float buttonGap   = ImGui::GetStyle().ItemSpacing.x;
    const float actionsWidth =
        hasConfirm ? 2.0f * buttonWidth + buttonGap : buttonWidth;
    // SameLine 的偏移相对当前窗口而非系统屏幕，窄窗口优先保留同排控件间隔。
    const float rowEnd =
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(std::max(
        rowEnd - actionsWidth,
        ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + buttonGap));
    if ( UI::FeedbackButton("退出###StartupExit", ImVec2(buttonWidth, 0.0f)) )
        m_exitRequested = true;
    if ( hasConfirm ) {
        ImGui::SameLine();
        // 意图由启动循环消费，视图本身不启动下载或切换上下文。
        if ( UI::FeedbackButton(
                 m_hasError ? "重试###StartupRetry" : "继续###StartupContinue",
                 ImVec2(buttonWidth, 0.0f)) ) {
            m_retryRequested    = m_hasError;
            m_continueRequested = m_hasWarning;
        }
    }

    // 日志默认收起，切换详情与重新计算面板位置在同一帧边界生效。
    // 详情不拥有后台下载对象，关闭后日志仍随快照继续累积。
    if ( detailsExpanded ) {
        // 详情区域下边界由窗口内容区决定；长诊断仅滚动正文。
        // 错误信息使用同一滚动区域，用户换线重试时仍可保留展开状态。
        // 日志用独立内边距，避免沿用外窗的大留白占走可阅读的行数。
        // 容器占用剩余高度，状态文本较长时可换行滚动而无需移动整块面板。
        // 收起只停止提交日志区域，保留已有日志以便再次展开时查看。
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(14.0f * scale, logPaddingY));
        if ( ImGui::BeginChild("StartupDetails",
                               ImVec2(0.0f, horizontal ? logHeight : 0.0f),
                               ImGuiChildFlags_Borders) ) {
            for ( const auto& entry : m_logs )
                ImGui::TextWrapped("%s", entry.c_str());
            if ( m_hasError || m_hasWarning )
                ImGui::TextWrapped(
                    "%s",
                    (m_hasError ? m_errorMessage : m_warningMessage).c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
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
    {
        std::scoped_lock lock(m_progressMutex);
        m_visibleProgress = m_pendingProgress;
    }
    // 格式化放在锁外且仅在快照变化时执行，不延长后台回调等待。
    m_progressLabel = progressText(m_visibleProgress);
    // 原始阶段信息可含当前文件名，优先用于诊断；字节进度只更新上面的状态行。
    const auto& entry = m_visibleProgress.message.empty()
                            ? m_progressLabel
                            : m_visibleProgress.message;
    // 每个进度快照只记最后一个有变化的描述，连续字节回调不会刷屏。
    // 容量到顶丢弃最旧日志，长期下载时仍只保留当前操作附近的线索。
    if ( m_logs.empty() || m_logs.back() != entry ) {
        if ( m_logs.size() == MAX_LOG_ENTRIES ) m_logs.pop_front();
        m_logs.push_back(entry);
    }
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
