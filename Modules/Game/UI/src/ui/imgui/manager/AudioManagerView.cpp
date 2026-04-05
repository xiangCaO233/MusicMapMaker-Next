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
    CLayVBox rootVBox;

    CLayHBox labelHBox;
    auto     fh = ImGui::GetFrameHeight();
    // 获取翻译文本
    auto hintText = TR("ui.audio_manager.initial_hint");
    labelHBox.addSpring()
        .addElement(hintText,
                    Sizing::Grow(),
                    Sizing::Fixed(fh),
                    [=](Clay_BoundingBox r, bool isHovered) {
                        // 文本在 30px 高度的坑位里垂直居中
                        float offY = (r.height - ImGui::GetFontSize()) * 0.5f;
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);

                        // 为了真正居中，可以用 ImGui 的居中文字函数
                        // 或者计算偏移：(r.width - CalcTextSize.x) * 0.5f
                        ImVec2 textSize = ImGui::CalcTextSize(hintText);
                        // 移动游标实现垂直居中
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                             (r.width - textSize.x) * 0.5f);

                        ImGui::TextEx(hintText);
                    })
        .addSpring();

    CLayHBox offsetHBox;
    offsetHBox.setPadding(8, 8, 8, 8)
        .addElement(
            "AudioConfigSettings",
            Sizing::Grow(),
            Sizing::Grow(),
            [this](Clay_BoundingBox r, bool isHovered) {
                auto& engine = Logic::EditorEngine::instance();
                auto  config = engine.getEditorConfig();

                bool changed = false;

                // Visual Offset
                float offsetMs = config.visualOffset * 1000.0f;
                ImGui::SetNextItemWidth(r.width * 0.6f);
                if ( ImGui::SliderFloat("Visual Offset",
                                        &offsetMs,
                                        -500.0f,
                                        500.0f,
                                        "%.0f ms") ) {
                    config.visualOffset = offsetMs / 1000.0f;
                    changed             = true;
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
                int currentStrategy =
                    static_cast<int>(config.sfxConfig.polylineStrategy);
                const char* strategyNames[] = { "Exact",
                                                "InternalAsNormal",
                                                "OnlyTailExact",
                                                "AllAsNormal" };
                ImGui::SetNextItemWidth(r.width * 0.6f);
                if ( ImGui::Combo("Polyline SFX Strategy",
                                  &currentStrategy,
                                  strategyNames,
                                  4) ) {
                    config.sfxConfig.polylineStrategy =
                        static_cast<Common::EditorConfig::PolylineSfxStrategy>(
                            currentStrategy);
                    changed = true;
                }

                ImGui::Spacing();

                // Flick Width Volume Scaling
                bool scaling = config.sfxConfig.enableFlickWidthVolumeScaling;
                if ( ImGui::Checkbox("Flick Width Volume Scaling", &scaling) ) {
                    config.sfxConfig.enableFlickWidthVolumeScaling = scaling;
                    changed                                        = true;
                }

                if ( scaling ) {
                    float mult = config.sfxConfig.flickWidthVolumeMultiplier;
                    ImGui::SetNextItemWidth(r.width * 0.6f);
                    if ( ImGui::SliderFloat("Per-Track Vol Multiplier",
                                            &mult,
                                            0.0f,
                                            1.0f,
                                            "%.2f") ) {
                        config.sfxConfig.flickWidthVolumeMultiplier = mult;
                        changed                                     = true;
                    }
                }

                if ( changed ) {
                    engine.setEditorConfig(config);
                }
            });

    rootVBox.setPadding(12, 12, 12, 12)
        .setSpacing(12)
        .addLayout("labelHBox", labelHBox, Sizing::Grow(), Sizing::Fixed(40))
        .addLayout("offsetHBox", offsetHBox, Sizing::Grow(), Sizing::Grow())
        .addSpring();
    rootVBox.render(layoutContext);
}
}  // namespace MMM::UI
