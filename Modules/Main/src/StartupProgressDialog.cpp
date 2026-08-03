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
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}
}  // namespace

void StartupProgressDialog::beginSync()
{
    Network::AssetSyncProgress initialProgress;
    initialProgress.stage = Network::AssetSyncProgressStage::kCheckingManifest;
    initialProgress.message = "正在检查资源更新...";

    {
        std::scoped_lock lock(m_progressMutex);
        m_pendingProgress = initialProgress;
    }
    m_visibleProgress = initialProgress;
    m_progressDirty.store(false, std::memory_order_release);
    m_hasError          = false;
    m_hasWarning        = false;
    m_retryRequested    = false;
    m_continueRequested = false;
    m_exitRequested     = false;
    m_errorTitle.clear();
    m_errorMessage.clear();
    m_warningTitle.clear();
    m_warningMessage.clear();
}

void StartupProgressDialog::update(const Network::AssetSyncProgress& progress)
{
    {
        std::scoped_lock lock(m_progressMutex);
        m_pendingProgress = progress;
    }
    m_progressDirty.store(true, std::memory_order_release);
}

void StartupProgressDialog::showError(std::string title, std::string message)
{
    consumePendingProgress();
    m_hasError          = true;
    m_hasWarning        = false;
    m_errorTitle        = std::move(title);
    m_errorMessage      = std::move(message);
    m_retryRequested    = false;
    m_continueRequested = false;
}

void StartupProgressDialog::showWarning(std::string title, std::string message)
{
    consumePendingProgress();
    m_hasError          = false;
    m_hasWarning        = true;
    m_warningTitle      = std::move(title);
    m_warningMessage    = std::move(message);
    m_retryRequested    = false;
    m_continueRequested = false;
}

bool StartupProgressDialog::consumeRetryRequest()
{
    const bool requested = m_retryRequested;
    m_retryRequested     = false;
    return requested;
}

bool StartupProgressDialog::consumeContinueRequest()
{
    const bool requested = m_continueRequested;
    m_continueRequested  = false;
    return requested;
}

bool StartupProgressDialog::isExitRequested() const
{
    return m_exitRequested;
}

void StartupProgressDialog::onPrepareResources(vk::PhysicalDevice&, vk::Device&,
                                               Graphic::VKSwapchain&,
                                               vk::CommandPool&, vk::Queue&)
{
}

void StartupProgressDialog::onUpdateUI()
{
    consumePendingProgress();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if ( !viewport ) return;

    const float scale =
        sanitizedScale(Config::AppConfig::instance().getWindowContentScale());
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(30.0f * scale, 26.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(0.055f, 0.064f, 0.092f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.30f, 0.50f, 0.85f));

    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    const bool windowVisible = ImGui::Begin(
        "MusicMapMaker Startup###StartupAssetSync", nullptr, windowFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if ( !windowVisible ) {
        ImGui::End();
        return;
    }

    ImDrawList*  drawList  = ImGui::GetWindowDrawList();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowMax{ windowPos.x + ImGui::GetWindowWidth(),
                            windowPos.y + ImGui::GetWindowHeight() };
    drawList->AddRectFilled(
        ImVec2(windowPos.x + 1.0f, windowPos.y + 1.0f),
        ImVec2(windowMax.x - 1.0f, windowPos.y + 4.0f * scale),
        IM_COL32(74, 134, 230, 255),
        12.0f * scale,
        ImDrawFlags_RoundCornersTop);

    ImGui::TextColored(ImVec4(0.55f, 0.72f, 1.0f, 1.0f), "MUSIC MAP MAKER");

    if ( m_hasError ) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.63f, 0.60f, 1.0f), "%s", m_errorTitle.c_str());
        ImGui::Separator();

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                               ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(m_errorMessage.c_str());
        ImGui::PopTextWrapPos();

        const float buttonWidth = 118.0f * scale;
        const float buttonGap   = 12.0f * scale;
        const float rowWidth    = buttonWidth * 2.0f + buttonGap;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(ImGui::GetContentRegionAvail().x - rowWidth, 0.0f));
        if ( UI::FeedbackButton("退出###StartupExit",
                                ImVec2(buttonWidth, 0.0f)) ) {
            m_exitRequested = true;
        }
        ImGui::SameLine(0.0f, buttonGap);
        if ( UI::FeedbackButton("重试###StartupRetry",
                                ImVec2(buttonWidth, 0.0f)) ) {
            m_retryRequested = true;
        }
    } else if ( m_hasWarning ) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.78f, 0.36f, 1.0f), "%s", m_warningTitle.c_str());
        ImGui::Separator();

        const float buttonHeight = ImGui::GetFrameHeight();
        const float messageHeight =
            std::max(ImGui::GetContentRegionAvail().y - buttonHeight -
                         ImGui::GetStyle().ItemSpacing.y,
                     80.0f * scale);
        if ( ImGui::BeginChild("TranslationOverrideWarningDetails",
                               ImVec2(0.0f, messageHeight),
                               ImGuiChildFlags_Borders) ) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                   ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(m_warningMessage.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();

        const float buttonWidth = 118.0f * scale;
        const float buttonGap   = 12.0f * scale;
        const float rowWidth    = buttonWidth * 2.0f + buttonGap;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(ImGui::GetContentRegionAvail().x - rowWidth, 0.0f));
        if ( UI::FeedbackButton("退出###StartupWarningExit",
                                ImVec2(buttonWidth, 0.0f)) ) {
            m_exitRequested = true;
        }
        ImGui::SameLine(0.0f, buttonGap);
        if ( UI::FeedbackButton("继续###StartupWarningContinue",
                                ImVec2(buttonWidth, 0.0f)) ) {
            m_continueRequested = true;
        }
    } else {
        ImGui::TextUnformatted("正在准备应用资源");
        ImGui::TextColored(ImVec4(0.66f, 0.70f, 0.80f, 1.0f),
                           "%s",
                           progressText(m_visibleProgress).c_str());

        const float       fraction = progressFraction(m_visibleProgress);
        const std::string overlay =
            std::to_string(static_cast<int>(std::round(fraction * 100.0f))) +
            "%";
        ImGui::ProgressBar(
            fraction, ImVec2(-1.0f, ImGui::GetFrameHeight()), overlay.c_str());
        ImGui::TextColored(ImVec4(0.48f, 0.53f, 0.64f, 1.0f),
                           "资源校验和更新期间请保持网络连接");

        const float exitButtonWidth = 96.0f * scale;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(ImGui::GetContentRegionAvail().x - exitButtonWidth, 0.0f));
        if ( UI::FeedbackButton("退出###StartupCancel",
                                ImVec2(exitButtonWidth, 0.0f)) ) {
            m_exitRequested = true;
        }
    }
    ImGui::End();
}

void StartupProgressDialog::onRecordOffscreen(vk::CommandBuffer&, uint32_t) {}

uint32_t StartupProgressDialog::getOffscreenRecordTaskCount() const
{
    return 0;
}

void StartupProgressDialog::consumePendingProgress()
{
    if ( !m_progressDirty.exchange(false, std::memory_order_acq_rel) ) {
        return;
    }

    std::scoped_lock lock(m_progressMutex);
    m_visibleProgress = m_pendingProgress;
}

float StartupProgressDialog::progressFraction(
    const Network::AssetSyncProgress& progress)
{
    const auto byteFraction = [&progress]() {
        if ( progress.totalBytes <= 0 ) return 0.0;
        return std::clamp(static_cast<double>(progress.currentBytes) /
                              static_cast<double>(progress.totalBytes),
                          0.0,
                          1.0);
    };
    const auto fileFraction = [&progress]() {
        if ( progress.totalFileCount == 0 ) return 0.0;
        return std::clamp(static_cast<double>(progress.currentFileIndex) /
                              static_cast<double>(progress.totalFileCount),
                          0.0,
                          1.0);
    };

    switch ( progress.stage ) {
    case Network::AssetSyncProgressStage::kCheckingManifest: return 0.06f;
    case Network::AssetSyncProgressStage::kDownloadingPackage:
        return static_cast<float>(0.08 + byteFraction() * 0.84);
    case Network::AssetSyncProgressStage::kExtractingPackage: return 0.96f;
    case Network::AssetSyncProgressStage::kCheckingFiles:
        return static_cast<float>(0.08 + fileFraction() * 0.34);
    case Network::AssetSyncProgressStage::kDownloadingFile: {
        const double completedFiles =
            progress.currentFileIndex > 0
                ? static_cast<double>(progress.currentFileIndex - 1)
                : 0.0;
        const double totalFiles =
            std::max(static_cast<double>(progress.totalFileCount), 1.0);
        const double combined = (completedFiles + byteFraction()) / totalFiles;
        return static_cast<float>(0.42 + std::clamp(combined, 0.0, 1.0) * 0.52);
    }
    case Network::AssetSyncProgressStage::kFinished: return 1.0f;
    }
    return 0.0f;
}

std::string StartupProgressDialog::progressText(
    const Network::AssetSyncProgress& progress)
{
    switch ( progress.stage ) {
    case Network::AssetSyncProgressStage::kCheckingManifest:
        return "正在检查资源更新...";
    case Network::AssetSyncProgressStage::kDownloadingPackage:
        if ( progress.totalBytes > 0 ) {
            return "正在下载资源包 " +
                   std::to_string(progress.currentBytes / 1024 / 1024) + "/" +
                   std::to_string(progress.totalBytes / 1024 / 1024) + " MB";
        }
        return "正在下载资源包...";
    case Network::AssetSyncProgressStage::kExtractingPackage:
        return "正在解压资源包...";
    case Network::AssetSyncProgressStage::kCheckingFiles:
        if ( progress.totalFileCount > 0 ) {
            return "正在校验本地资源 " +
                   std::to_string(progress.currentFileIndex) + "/" +
                   std::to_string(progress.totalFileCount);
        }
        return "正在校验本地资源...";
    case Network::AssetSyncProgressStage::kDownloadingFile:
        return "正在更新资源文件 " + std::to_string(progress.currentFileIndex) +
               "/" + std::to_string(progress.totalFileCount);
    case Network::AssetSyncProgressStage::kFinished:
        return "资源已经准备完成，正在启动...";
    }
    return progress.message;
}

}  // namespace MMM::Main
