#define IMGUI_DEFINE_MATH_OPERATORS
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/imgui/menu/MainMenuView.h"
#include "ui/imgui/menu/actions/MainMenuHelpActions.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <memory>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 检查更新动作。
class CheckUpdateAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 构造检查更新动作并创建更新检查器。
    CheckUpdateAction()
        : m_updateChecker(std::make_unique<MMM::Network::UpdateChecker>())
    {
    }

    /// @brief 启动时自动检查和静默检查状态轮询。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧只轮询更新检查器状态，不执行网络阻塞操作。
    void update(MainMenuContext& context) override
    {
        if ( !m_hasCheckedOnStartup ) {
            m_hasCheckedOnStartup = true;

            if ( MMM::Network::UpdateChecker::checkStartupUpdateMarker() ) {
                m_showUpdateSuccessPopup = true;
            } else {
                m_isSilentCheck = true;
                m_updateChecker->checkAsync();
            }
        }

        if ( !m_isSilentCheck ) return;

        auto info = m_updateChecker->getInfo();
        if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
            m_showUpdatePopup = true;
            m_isSilentCheck   = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kUpToDate ) {
            context.view.showStatusMessage(TR("ui.help.up_to_date").data(),
                                           5.0f);
            m_isSilentCheck = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
            m_isSilentCheck = false;
        }
    }

    /// @brief 启动一次非静默更新检查。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        m_isSilentCheck       = false;
        m_showCheckingPopup   = true;
        m_updatePopupCanceled = false;
        m_updateRestartError.clear();
        m_updateChecker->checkAsync();
    }

    /// @brief 渲染更新相关弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；只绘制已经打开的更新弹窗。
    void renderDeferred(MainMenuContext& context) override
    {
        (void)context;
        renderUpdateCheckingPopup();
        renderUpdatePopup();
        renderUpdateSuccessPopup();
    }

private:
    /// @brief 渲染更新检查中的状态弹窗。
    /// @warning UI 热路径：每帧执行；只轮询更新检查器状态。
    void renderUpdateCheckingPopup()
    {
        if ( m_showCheckingPopup ) {
            ImGui::OpenPopup(TR("ui.help.check_update"));
            m_showCheckingPopup = false;
        }

        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(TR("ui.help.check_update")) ) {
            auto info = m_updateChecker->getInfo();

            if ( info.status == MMM::Network::UpdateStatus::kChecking ) {
                float textWidth =
                    ImGui::CalcTextSize(TR("ui.help.checking").data()).x;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) *
                                     0.5f);
                ImGui::TextUnformatted(TR("ui.help.checking").data());

                float barWidth = 240.0f * dpiScale;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) *
                                     0.5f);
                float fraction = -1.0f * (float)ImGui::GetTime();
                ImGui::ProgressBar(
                    fraction, ImVec2(barWidth, 12.0f * dpiScale), "");
            } else if ( info.status == MMM::Network::UpdateStatus::kUpToDate ) {
                float textWidth =
                    ImGui::CalcTextSize(TR("ui.help.up_to_date").data()).x;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) *
                                     0.5f);
                ImGui::TextUnformatted(TR("ui.help.up_to_date").data());

                float btnWidth = 120.0f * dpiScale;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) *
                                     0.5f);
                if ( ::MMM::UI::FeedbackButton(
                         TR("ui.help.ok").data(),
                         ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                    ImGui::CloseCurrentPopup();
                }
            } else if ( info.status ==
                        MMM::Network::UpdateStatus::kUpdateFound ) {
                ImGui::CloseCurrentPopup();
                m_showUpdatePopup = true;
            } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
                renderErrorBody(info.errorMessage, dpiScale);
            }

            ImGui::EndPopup();
        }

        if ( m_showUpdatePopup ) {
            m_showCheckingPopup = false;
        }
    }

    /// @brief 渲染发现更新后的下载确认与下载进度弹窗。
    /// @warning UI 热路径：每帧执行；下载操作由更新检查器异步执行。
    void renderUpdatePopup()
    {
        auto info = m_updateChecker->getInfo();

        if ( m_showUpdatePopup ) {
            ImGui::OpenPopup(TR("ui.help.update_found"));
            m_showUpdatePopup = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kUpdateFound &&
                    !m_updatePopupCanceled ) {
            if ( !ImGui::IsPopupOpen(TR("ui.help.update_found")) )
                ImGui::OpenPopup(TR("ui.help.update_found"));
        }

        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(TR("ui.help.update_found")) ) {
            info = m_updateChecker->getInfo();

            if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
                renderUpdateFoundBody(info, dpiScale);
            } else if ( info.status ==
                        MMM::Network::UpdateStatus::kDownloading ) {
                renderDownloadingBody(info, dpiScale);
            } else if ( info.status ==
                        MMM::Network::UpdateStatus::kDownloaded ) {
                renderDownloadedBody(info, dpiScale);
            } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
                renderErrorBody(info.errorMessage, dpiScale);
            }

            ImGui::EndPopup();
        }
    }

    /// @brief 渲染更新下载成功后的提示弹窗。
    /// @warning UI 热路径：每帧执行；只绘制完成提示。
    void renderUpdateSuccessPopup()
    {
        if ( m_showUpdateSuccessPopup ) {
            ImGui::OpenPopup(TR("ui.help.update_success"));
            m_showUpdateSuccessPopup = false;
        }

        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( !modalScope.begin(TR("ui.help.update_success")) ) return;

        ImVec4 greenColor(0.3f, 1.0f, 0.3f, 1.0f);
        float  textWidth =
            ImGui::CalcTextSize(TR("ui.help.update_success_msg").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextColored(
            greenColor, "%s", TR("ui.help.update_success_msg").data());

        if ( ImGui::BeginTable("SuccessInfoTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("ui.help.current_version").data());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(greenColor, MMM_VERSION_STRING);

            ImGui::EndTable();
        }

        ImGui::Spacing();

        float btnWidth = 120.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.ok").data(),
                                       ImVec2(btnWidth, 32.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    /// @brief 渲染发现更新时的版本信息和操作按钮。
    /// @param info 更新检查信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：点击下载按钮时只启动异步下载。
    void renderUpdateFoundBody(const MMM::Network::UpdateInfo& info,
                               float                           dpiScale)
    {
        if ( ImGui::BeginTable("UpdateInfoTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            auto addRow = [&](const char*   label,
                              const char*   value,
                              const ImVec4* color = nullptr) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if ( color )
                    ImGui::TextColored(*color, "%s", value);
                else
                    ImGui::TextDisabled("%s", value);
            };

            addRow(TR("ui.help.current_version").data(),
                   info.currentVersion.c_str());
            ImVec4 green(0.3f, 1.0f, 0.3f, 1.0f);
            addRow(TR("ui.help.latest_version").data(),
                   info.latestVersion.c_str(),
                   &green);

            if ( !info.releaseDate.empty() )
                addRow(TR("ui.help.release_date").data(),
                       info.releaseDate.c_str());

            if ( info.downloadSize > 0 ) {
                char sizeBuf[64];
                if ( info.downloadSize >= 1024 * 1024 )
                    snprintf(sizeBuf,
                             sizeof(sizeBuf),
                             "%.1f MB",
                             info.downloadSize / (1024.0 * 1024.0));
                else if ( info.downloadSize >= 1024 )
                    snprintf(sizeBuf,
                             sizeof(sizeBuf),
                             "%.0f KB",
                             info.downloadSize / 1024.0);
                else
                    snprintf(sizeBuf,
                             sizeof(sizeBuf),
                             "%lld B",
                             (long long)info.downloadSize);
                addRow(TR("ui.help.file_size").data(), sizeBuf);
            }
            ImGui::EndTable();
        }

        if ( !info.changelog.empty() ) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted(TR("ui.help.changelog").data());

            ImGui::BeginChild("ChangelogScroll",
                              ImVec2(400.0f * dpiScale, 150.0f * dpiScale),
                              ImGuiChildFlags_Borders);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
            ImGui::TextWrapped("%s", info.changelog.c_str());
            ImGui::PopStyleVar();
            ImGui::EndChild();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float buttonWidth  = 140.0f * dpiScale;
        float totalButtons = info.downloadUrl.empty() ? 1.0f : 2.0f;
        float spacing      = ImGui::GetStyle().ItemSpacing.x;
        float totalWidth =
            totalButtons * buttonWidth + (totalButtons - 1) * spacing;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

        if ( !info.downloadUrl.empty() ) {
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.help.download_and_install").data(),
                     ImVec2(buttonWidth, 36.0f * dpiScale)) ) {
                m_updateRestartError.clear();
                m_updateChecker->downloadAsync();
            }
            ImGui::SameLine();
        }

        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.help.cancel").data(),
                 ImVec2(buttonWidth, 36.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
            m_updatePopupCanceled = true;
        }
    }

    /// @brief 渲染下载中状态。
    /// @param info 更新检查信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：只展示更新检查器进度。
    void renderDownloadingBody(const MMM::Network::UpdateInfo& info,
                               float                           dpiScale)
    {
        float textWidth =
            ImGui::CalcTextSize(TR("ui.help.downloading").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextUnformatted(TR("ui.help.downloading").data());

        float barWidth = 360.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) * 0.5f);
        ImGui::ProgressBar(static_cast<float>(info.downloadProgress),
                           ImVec2(barWidth, 20.0f * dpiScale));

        char progressText[128];
        if ( info.downloadSize > 0 ) {
            snprintf(progressText,
                     sizeof(progressText),
                     "%lld / %lld MB",
                     (long long)(info.downloadedBytes / (1024 * 1024)),
                     (long long)(info.downloadSize / (1024 * 1024)));
        } else {
            snprintf(progressText,
                     sizeof(progressText),
                     "%.0f%%",
                     info.downloadProgress * 100.0);
        }
        float pWidth = ImGui::CalcTextSize(progressText).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - pWidth) * 0.5f);
        ImGui::TextDisabled("%s", progressText);
    }

    /// @brief 渲染下载完成状态。
    /// @param info 更新检查信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：点击重启按钮时启动更新器进程。
    void renderDownloadedBody(const MMM::Network::UpdateInfo& info,
                              float                           dpiScale)
    {
        float textWidth =
            ImGui::CalcTextSize(TR("ui.help.download_complete").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextUnformatted(TR("ui.help.download_complete").data());

        float btnWidth = std::max(
            200.0f * dpiScale,
            ImGui::CalcTextSize(TR("ui.help.restart_to_update").data()).x +
                48.0f * dpiScale);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.restart_to_update").data(),
                                       ImVec2(btnWidth, 40.0f * dpiScale)) ) {
            std::string restartError;
            m_updateRestartError.clear();
            if ( !MMM::Network::UpdateChecker::applyUpdateAndRestart(
                     info.downloadedFilePath,
                     info.updaterFilePath,
                     &restartError) ) {
                m_updateRestartError = restartError.empty()
                                           ? "Failed to launch updater"
                                           : restartError;
            }
        }

        if ( !m_updateRestartError.empty() ) {
            ImGui::Spacing();
            ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, errColor);
            ImGui::TextWrapped("%s", m_updateRestartError.c_str());
            ImGui::PopStyleColor();
        }
    }

    /// @brief 渲染更新错误状态。
    /// @param errorMessage 错误信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：只绘制错误文本。
    void renderErrorBody(const std::string& errorMessage, float dpiScale)
    {
        ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
        float  textWidth =
            ImGui::CalcTextSize(TR("ui.help.update_error").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextColored(errColor, "%s", TR("ui.help.update_error").data());

        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", errorMessage.c_str());
        ImGui::PopStyleColor();

        float btnWidth = 120.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.ok").data(),
                                       ImVec2(btnWidth, 32.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }
    }

    /// @brief 是否已完成启动时的自动更新检查。
    bool m_hasCheckedOnStartup = false;

    /// @brief 是否为启动时的静默检查。
    bool m_isSilentCheck = false;

    /// @brief 是否在下一帧打开更新下载弹窗。
    bool m_showUpdatePopup = false;

    /// @brief 是否在下一帧打开更新检查中弹窗。
    bool m_showCheckingPopup = false;

    /// @brief 是否在下一帧打开更新成功弹窗。
    bool m_showUpdateSuccessPopup = false;

    /// @brief 用户是否取消或关闭了更新弹窗。
    bool m_updatePopupCanceled = false;

    /// @brief 点击重启更新失败时显示的错误信息。
    std::string m_updateRestartError;

    /// @brief 更新检查器实例。
    std::unique_ptr<MMM::Network::UpdateChecker> m_updateChecker;
};
}  // namespace

/// @brief 创建检查更新动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCheckUpdateAction()
{
    return std::make_unique<CheckUpdateAction>();
}

}  // namespace MMM::UI
