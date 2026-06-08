#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"

namespace MMM::UI
{

/// @brief 渲染项目设置页。
void SettingsView::drawProjectSettings()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();

    if ( !project ) {
        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::TextColored(
            dangerCol, "%s", TR("ui.settings.project.no_project").data());
        return;
    }

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;
    bool   changed      = false;

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "PRJ_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID id = ImGui::GetID(baseIdStr.c_str());

        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.project.info").data(), true) ) {
        std::string projPath = Config::pathToUtf8(project->m_projectRoot);
        const float labelW   = getCurrentTabLabelWidth(
            Config::AppConfig::instance().getWindowContentScale());
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.project.path").data(),
            labelW,
            [projPath](Clay_BoundingBox r, bool) {
                float textW  = ImGui::CalcTextSize(projPath.c_str()).x;
                float textH  = ImGui::CalcTextSize(projPath.c_str()).y;
                float offset = (r.height - textH) * 0.5f;

                if ( textW <= r.width ) {
                    // 不需要滚动，静态居中渲染
                    ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                    ImGui::TextUnformatted(projPath.c_str());
                } else {
                    // 需要自动滚动（跑马灯效果）
                    float scrollSpeed = 40.0f;  // 每秒滚动像素数
                    float maxScroll =
                        textW - r.width + 30.0f;  // 滚到底部，留点余量
                    float pauseTime      = 1.5f;  // 起点与终点停顿秒数
                    float scrollDuration = maxScroll / scrollSpeed;
                    float totalCycleTime = scrollDuration + pauseTime * 2.0f;

                    float time      = (float)ImGui::GetTime();
                    float cycleTime = fmodf(time, totalCycleTime);

                    float scrollX = 0.0f;
                    if ( cycleTime < pauseTime ) {
                        scrollX = 0.0f;  // 起点停顿
                    } else if ( cycleTime < pauseTime + scrollDuration ) {
                        scrollX =
                            (cycleTime - pauseTime) * scrollSpeed;  // 顺畅滑动
                    } else {
                        scrollX = maxScroll;  // 终点停顿
                    }

                    // 开启裁剪矩形防止文字超出 Widget 区域
                    ImGui::PushClipRect(
                        { r.x, r.y }, { r.x + r.width, r.y + r.height }, true);
                    ImGui::SetCursorScreenPos({ r.x - scrollX, r.y + offset });
                    ImGui::TextUnformatted(projPath.c_str());
                    ImGui::PopClipRect();
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.project.note_palette").data(),
            labelW,
            [&](Clay_BoundingBox r, bool) {
                auto& paletteConfig = Config::AppConfig::instance()
                                          .getEditorSettings()
                                          .noteColorPalettes;
                auto& projectScheme =
                    project->m_settings.m_noteColorPaletteSchemeName;

                std::string previewName;
                if ( projectScheme.empty() ) {
                    previewName =
                        TR_CACHE("ui.settings.project.note_palette.inherit")
                            .data();
                } else if ( projectScheme ==
                            Config::
                                NOTE_COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ) {
                    previewName =
                        TR_CACHE("ui.toolbar.note_palette.skin_default_scheme")
                            .data();
                } else {
                    previewName = projectScheme;
                }

                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::BeginCombo("##ProjectNotePalette",
                                       previewName.c_str()) ) {
                    const bool inheritSelected = projectScheme.empty();
                    if ( ImGui::Selectable(
                             TR_CACHE(
                                 "ui.settings.project.note_palette.inherit")
                                 .data(),
                             inheritSelected) ) {
                        projectScheme.clear();
                        changed = true;
                    }
                    if ( inheritSelected ) ImGui::SetItemDefaultFocus();

                    const bool skinSelected =
                        projectScheme ==
                        Config::NOTE_COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                    if ( ImGui::Selectable(
                             TR_CACHE(
                                 "ui.toolbar.note_palette.skin_default_scheme")
                                 .data(),
                             skinSelected) ) {
                        projectScheme =
                            Config::NOTE_COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                        changed = true;
                    }
                    if ( skinSelected ) ImGui::SetItemDefaultFocus();

                    for ( const auto& scheme : paletteConfig.schemes ) {
                        const bool selected = projectScheme == scheme.name;
                        if ( ImGui::Selectable(scheme.name.c_str(),
                                               selected) ) {
                            projectScheme = scheme.name;
                            changed       = true;
                        }
                        if ( selected ) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            });
    }

    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( changed ) {
        engine.saveProject();
    }
}

}  // namespace MMM::UI
