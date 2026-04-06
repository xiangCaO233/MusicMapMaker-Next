#include "ui/imgui/manager/AudioManagerView.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::UI
{
// 内部绘制逻辑 (Clay/ImGui)
void AudioManagerView::onUpdate(LayoutContext& layoutContext,
                                UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();

    CLayVBox rootVBox;

    if ( !project ) {
        CLayHBox labelHBox;
        auto     fh = ImGui::GetFrameHeight();
        labelHBox.addSpring()
            .addElement("InitialHint",
                        Sizing::Grow(),
                        Sizing::Fixed(fh),
                        [=](Clay_BoundingBox r, bool isHovered) {
                            float offY =
                                (r.height - ImGui::GetFontSize()) * 0.5f;
                            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);
                            ImVec2 textSize = ImGui::CalcTextSize(
                                TR("ui.audio_manager.initial_hint"));
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                                 (r.width - textSize.x) * 0.5f);
                            ImGui::TextEx(TR("ui.audio_manager.initial_hint"));
                        })
            .addSpring();

        rootVBox.setPadding(12, 12, 12, 12)
            .addLayout(
                "labelHBox", labelHBox, Sizing::Grow(), Sizing::Fixed(40));
        rootVBox.addSpring();
        rootVBox.render(layoutContext);
        return;
    }

    // 已打开项目时的界面
    CLayVBox listVBox;
    listVBox.setSpacing(4);

    listVBox.addElement(
        "AudioTracksHeader",
        Sizing::Grow(),
        Sizing::Fixed(24),
        [](Clay_BoundingBox r, bool isHovered) {
            ImGui::SeparatorText(TR("ui.audio_manager.audio_tracks").data());
        });

    for ( const auto& audio : project->m_audioResources ) {
        listVBox.addElement("Audio_" + audio.m_id,
                            Sizing::Grow(),
                            Sizing::Fixed(28),
                            [&audio](Clay_BoundingBox r, bool isHovered) {
                                ImGui::Selectable(audio.m_id.c_str());
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip(
                                        "%s (Type: %s)",
                                        audio.m_path.c_str(),
                                        audio.m_type == AudioTrackType::Main
                                            ? "Main"
                                            : "Effect");
                                }
                            });
    }

    CLayHBox offsetHBox;
    offsetHBox.setPadding(8, 8, 8, 8)
        .addElement(
            "AudioConfigSettings",
            Sizing::Grow(),
            Sizing::Grow(),
            [this, &engine](Clay_BoundingBox r, bool isHovered) {
                auto config = engine.getEditorConfig();

                bool changed = false;

                // Visual Offset
                float offsetMs = config.visual.visualOffset * 1000.0f;
                ImGui::SetNextItemWidth(r.width * 0.6f);
                if ( ImGui::SliderFloat("###Visual Offset",
                                        &offsetMs,
                                        -500.0f,
                                        500.0f,
                                        "%.0f ms") ) {
                    config.visual.visualOffset = offsetMs / 1000.0f;
                    changed                    = true;
                }

                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip(
                        "%s",
                        TR("ui.audio_manager.visual_offset_tooltip").data());
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Polyline SFX Strategy
                int currentStrategy = static_cast<int>(
                    config.settings.sfxConfig.polylineStrategy);
                const char* strategyNames[] = { "Exact",
                                                "InternalAsNormal",
                                                "OnlyTailExact",
                                                "AllAsNormal" };
                ImGui::SetNextItemWidth(r.width * 0.6f);
                if ( ImGui::Combo("Polyline SFX Strategy",
                                  &currentStrategy,
                                  strategyNames,
                                  4) ) {
                    config.settings.sfxConfig.polylineStrategy =
                        static_cast<Config::PolylineSfxStrategy>(
                            currentStrategy);
                    changed = true;
                }

                ImGui::Spacing();

                // Flick Width Volume Scaling
                bool scaling =
                    config.settings.sfxConfig.enableFlickWidthVolumeScaling;
                if ( ImGui::Checkbox("Flick Width Volume Scaling", &scaling) ) {
                    config.settings.sfxConfig.enableFlickWidthVolumeScaling =
                        scaling;
                    changed = true;
                }

                if ( scaling ) {
                    float mult =
                        config.settings.sfxConfig.flickWidthVolumeMultiplier;
                    ImGui::SetNextItemWidth(r.width * 0.6f);
                    if ( ImGui::SliderFloat("Per-Track Vol Multiplier",
                                            &mult,
                                            0.0f,
                                            1.0f,
                                            "%.2f") ) {
                        config.settings.sfxConfig.flickWidthVolumeMultiplier =
                            mult;
                        changed = true;
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // File Picker Style
                int currentStyle =
                    static_cast<int>(config.settings.filePickerStyle);
                const char* styleNames[] = { "Native (NFD)",
                                             "Unified (ImGuiFileDialog)" };
                ImGui::SetNextItemWidth(r.width * 0.6f);
                if ( ImGui::Combo(
                         "File Picker Style", &currentStyle, styleNames, 2) ) {
                    config.settings.filePickerStyle =
                        static_cast<Config::FilePickerStyle>(currentStyle);
                    changed = true;
                }

                if ( changed ) {
                    engine.setEditorConfig(config);
                    Config::AppConfig::instance().save();
                }
            });

    rootVBox.setPadding(12, 12, 12, 12)
        .setSpacing(12)
        .addLayout("listVBox", listVBox, Sizing::Grow(), Sizing::Grow())
        .addLayout("offsetHBox", offsetHBox, Sizing::Grow(), Sizing::Fixed(200))
        .addSpring();
    rootVBox.render(layoutContext);
}
}  // namespace MMM::UI
