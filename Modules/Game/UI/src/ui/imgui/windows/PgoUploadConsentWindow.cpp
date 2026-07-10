#include "ui/imgui/windows/PgoUploadConsentWindow.h"

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>
#include <string>

namespace MMM::UI
{

/// @brief 在尚未记录授权选择时渲染 PGO 上传授权窗口。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：每帧执行；仅在 PGO 插桩构建且尚未授权时绘制。
void PgoUploadConsentWindow::render(float dpiScale) const
{
#ifndef MMM_PGO_INSTRUMENT
    (void)dpiScale;
#else
    auto& appConfig = Config::AppConfig::instance();
    auto& settings  = appConfig.getEditorSettings();
    if ( settings.pgoProfileUploadConsentAsked ) return;

    const std::string popupId = std::string(TR("ui.pgo.consent.title").data()) +
                                "###PgoUploadConsentModal";
    ImGui::OpenPopup(popupId.c_str());

    Utils::CenteredModalPopupScope popupStyle(dpiScale);
    if ( !popupStyle.begin(popupId.c_str(),
                           nullptr,
                           ImGuiWindowFlags_None,
                           ImVec2(620.0f * dpiScale, 0.0f)) ) {
        return;
    }

    ImGui::TextWrapped("%s", TR("ui.pgo.consent.message").data());
    ImGui::Spacing();
    ImGui::TextWrapped("%s", TR("ui.pgo.consent.detail").data());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const auto applyConsent = [&](bool allowUpload) {
        settings.autoUploadPgoProfiles        = allowUpload;
        settings.pgoProfileUploadConsentAsked = true;
        appConfig.save();
        ImGui::CloseCurrentPopup();
    };

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2      buttonSize(128.0f * dpiScale, 0.0f);
    const float buttonRowWidth = buttonSize.x * 2.0f + style.ItemSpacing.x;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    if ( availableWidth > buttonRowWidth ) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (availableWidth - buttonRowWidth) * 0.5f);
    }

    if ( ::MMM::UI::FeedbackButton(TR("ui.pgo.consent.accept").data(),
                                   buttonSize) ) {
        applyConsent(true);
    }
    ImGui::SameLine();
    if ( ::MMM::UI::FeedbackButton(TR("ui.pgo.consent.decline").data(),
                                   buttonSize) ) {
        applyConsent(false);
    }

    ImGui::EndPopup();
#endif
}

}  // namespace MMM::UI
