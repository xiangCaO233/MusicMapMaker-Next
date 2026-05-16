#include "ui/imgui/manager/BeatMapManagerView.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIWidgetUtils.h"

namespace MMM::UI
{

void BeatMapManagerView::onUpdate(LayoutContext& layoutContext,
                                  UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    float   dpiScale        = layoutContext.m_dpiScale;
    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) ImGui::PushFont(fileManagerFont);

    CLayVBox rootVBox;

    if ( !project ) {
        CLayHBox labelHBox;
        auto     fh = ImGui::GetFrameHeight();
        labelHBox.addSpring()
            .addElement(
                "InitialHint",
                Sizing::Grow(),
                Sizing::Fixed(fh),
                [=](Clay_BoundingBox r, bool isHovered) {
                    float offY = (r.height - ImGui::GetFontSize()) * 0.5f;
                    ImGui::SetCursorScreenPos({ r.x, r.y + offY });
                    ImVec2 textSize = ImGui::CalcTextSize(
                        TR("ui.beatmap_manager.initial_hint").data());
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                         (r.width - textSize.x) * 0.5f);
                    ImGui::TextDisabled(
                        "%s", TR("ui.beatmap_manager.initial_hint").data());
                })
            .addSpring();

        rootVBox.setPadding(12, 12, 12, 12)
            .addLayout(
                "labelHBox", labelHBox, Sizing::Grow(), Sizing::Fixed(40));
        rootVBox.addSpring();
        rootVBox.render(layoutContext);

        if ( fileManagerFont ) ImGui::PopFont();
        return;
    }

    // 已打开项目时的界面
    CLayVBox listVBox;
    listVBox.setSpacing(4);

    listVBox.addElement("BeatmapsHeader",
                        Sizing::Grow(),
                        Sizing::Fixed(ImGui::GetFrameHeight()),
                        [this](Clay_BoundingBox r, bool isHovered) {
                            Utils::renderCollapsingHeader(
                                TR("ui.beatmap_manager.beatmaps").data(),
                                &m_showBeatmapList,
                                r);
                        });

    if ( m_showBeatmapList ) {
        for ( const auto& beatmap : project->m_beatmaps ) {
            listVBox.addElement(
                "Beatmap_" + beatmap.m_filePath,
                Sizing::Grow(),
                Sizing::Fixed(28 * dpiScale),
                [=, &engine, this](Clay_BoundingBox r, bool isHovered) {
                    ImGui::Indent();
                    std::string labelStr =
                        beatmap.m_name + " - " + beatmap.m_filePath;
                    float availW = ImGui::GetContentRegionAvail().x;

                    Utils::renderScrollingSelectable(
                        beatmap.m_filePath,
                        labelStr,
                        availW,
                        28 * dpiScale,
                        [&]() {
                            XINFO("Request to load beatmap: {}",
                                  beatmap.m_name);
                            auto fullPath =
                                project->m_projectRoot /
                                std::filesystem::path(
                                    reinterpret_cast<const char8_t*>(
                                        beatmap.m_filePath.c_str()));
                            auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                                MMM::BeatMap::loadFromFile(fullPath));
                            engine.pushCommand(
                                Logic::CmdLoadBeatmap{ loadedBeatmap });
                        });

                    static int s_bmLogCounter = 0;
                    {
                        bool hovered = ImGui::IsItemHovered();
                        bool rclicked =
                            ImGui::IsMouseClicked(ImGuiMouseButton_Right);
                        if ( (hovered || rclicked) && s_bmLogCounter < 10 ) {
                            XINFO(
                                "BeatmapItem [{}]: hovered={} rclicked={} "
                                "pos=({},{}) size=({},{}) mouse=({},{})",
                                beatmap.m_name,
                                hovered,
                                rclicked,
                                r.x,
                                r.y,
                                r.width,
                                r.height,
                                ImGui::GetMousePos().x,
                                ImGui::GetMousePos().y);
                            s_bmLogCounter++;
                        }
                        if ( hovered && rclicked ) {
                            XINFO("BeatmapItem RIGHT-CLICK TRIGGERED: {}",
                                  beatmap.m_filePath);
                            m_manageBeatmapPath = beatmap.m_filePath;
                            m_openManageModal   = true;
                        }
                    }

                    if ( ImGui::IsItemHovered() ) {
                        ImGui::SetTooltip("File: %s\nTrack: %s",
                                          beatmap.m_filePath.c_str(),
                                          beatmap.m_audioTrackId.c_str());
                    }
                    ImGui::Unindent();
                });
        }
    }

    // 1. 顶部列表区域
    float footerH = 44.0f * dpiScale;
    rootVBox.setPadding(12 * dpiScale, 12 * dpiScale, 12 * dpiScale, 0)
        .setSpacing(8 * dpiScale)
        .addElement(
            "BeatmapListArea",
            Sizing::Grow(),
            Sizing::Grow(),
            [&listVBox, &layoutContext](Clay_BoundingBox r, bool isHovered) {
                ImGui::BeginChild("BeatmapListChild",
                                  { r.width, r.height },
                                  false,
                                  ImGuiWindowFlags_None);

                ImVec2 oldStartPos = layoutContext.m_startPos;
                ImVec2 oldAvail    = layoutContext.m_avail;

                layoutContext.m_startPos = ImGui::GetCursorScreenPos();
                layoutContext.m_avail    = { r.width, 10000.0f };

                listVBox.render(layoutContext);

                layoutContext.m_startPos = oldStartPos;
                layoutContext.m_avail    = oldAvail;

                ImGui::EndChild();
            });

    ImVec2 totalSize = rootVBox.renderInCurrent(
        layoutContext.m_startPos,
        { layoutContext.m_avail.x, layoutContext.m_avail.y - footerH });

    // 2. 底部按钮区域 (独立渲染，确保不被列表遮挡)
    CLayHBox bottomBtnHBox;
    float    btnSize = 32.0f * dpiScale;
    bottomBtnHBox.setPadding(12 * dpiScale, 12 * dpiScale, 0, 0)
        .setAlignment(Alignment::Center())  // 垂直居中
        .addElement(
            "Beatmap_CreateNew",
            Sizing::Grow(),  // 宽度拉满
            Sizing::Fixed(btnSize),
            [&engine, sourceManager](Clay_BoundingBox r, bool isHovered) {
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(1, 1, 1, 0.2f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

                ImGui::SetCursorScreenPos({ r.x, r.y });
                // 使用带有圆角的装饰背景 (与列表项风格统一)
                ImDrawList* dl    = ImGui::GetWindowDrawList();
                ImVec4      bgCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
                bgCol.w *= 0.5f;
                float rounding = ImGui::GetStyle().FrameRounding;

                if ( isHovered ) bgCol.w *= 1.5f;

                dl->AddRectFilled({ r.x, r.y },
                                  { r.x + r.width, r.y + r.height },
                                  ImGui::ColorConvertFloat4ToU32(bgCol),
                                  rounding);

                if ( ImGui::Button(ICON_MMM_PLUS, ImVec2(r.width, r.height)) ) {
                    auto* wizard = sourceManager->getView<NewBeatmapWizard>(
                        "NewBeatmapWizard");
                    if ( wizard ) wizard->open();
                }

                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s", TR_CACHE("ui.file.new_map").data());
                }
            });

    ImVec2 footerPos = { layoutContext.m_startPos.x,
                         layoutContext.m_startPos.y + totalSize.y };
    bottomBtnHBox.renderInCurrent(footerPos,
                                  { layoutContext.m_avail.x, footerH });

    // --- 3. 谱面管理窗口 ---
    bool showBMModal = !m_manageBeatmapPath.empty();
    if ( showBMModal ) {
        std::string windowTitle =
            fmt::format("{} {}", TR("ui.beatmap_manager.manage_title").data(),
                        m_manageBeatmapPath);
        ImGui::SetNextWindowSize({ 420 * dpiScale, 0 }, ImGuiCond_FirstUseEver);
        if ( ImGui::Begin(windowTitle.c_str(),
                          &showBMModal,
                          ImGuiWindowFlags_NoCollapse) ) {
            if ( !showBMModal ) {
                m_manageBeatmapPath = "";
            }

        // --- 使用 Clay 重构对话框内容 ---
        CLayVBox modalLayout;
        float padding = 16 * dpiScale;
        modalLayout.setPadding(padding, padding, padding, padding);
        modalLayout.setSpacing(16 * dpiScale);

        // 1. 标题与信息 (移除了冗余 Text)
        modalLayout.addElement(
            "ModalSep", Sizing::Grow(), Sizing::Fixed(1),
            [=, this](Clay_BoundingBox r, bool) {
                ImGui::GetWindowDrawList()->AddLine(
                    { r.x, r.y }, { r.x + r.width, r.y },
                    ImGui::GetColorU32(ImGuiCol_Separator));
            });

        // 2. 操作按钮区
        CLayHBox btnRow;
        btnRow.setAlignment(Alignment::Center());
        btnRow.setSpacing(12 * dpiScale);

        btnRow.addElement(
            "RemoveBtn", Sizing::Fixed(140 * dpiScale),
            Sizing::Fixed(32 * dpiScale), [=](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                if ( ImGui::Button(TR("ui.beatmap_manager.remove_beatmap").data(),
                                   { r.width, r.height }) ) {
                    ImGui::OpenPopup("RemoveBeatmapConfirm");
                }
            });

        btnRow.addElement(
            "CancelBtn", Sizing::Fixed(100 * dpiScale),
            Sizing::Fixed(32 * dpiScale), [=](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                if ( ImGui::Button(TR("ui.common.cancel").data(),
                                   { r.width, r.height }) ) {
                    m_manageBeatmapPath = "";
                }
            });

        modalLayout.addLayout("BtnRowLayout",
                              btnRow,
                              Sizing::Grow(),
                              Sizing::Fixed(32 * dpiScale));

        // 渲染布局
        ImVec2 modalSize = modalLayout.renderInCurrent(
            ImGui::GetCursorScreenPos(), { 400 * dpiScale, 0 });
        ImGui::Dummy(modalSize);

        // --- 二次确认弹窗 ---
        if ( ImGui::BeginPopupModal("RemoveBeatmapConfirm",
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize) ) {
            ImGui::Text("%s", TR("ui.beatmap_manager.remove_confirm").data());
            ImGui::Spacing();
            if ( ImGui::Button(TR("ui.common.confirm").data(),
                               { 100 * dpiScale, 0 }) ) {
                engine.pushCommand(
                    Logic::CmdRemoveBeatmap{ m_manageBeatmapPath });
                m_manageBeatmapPath = "";
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.common.cancel").data(),
                               { 100 * dpiScale, 0 }) ) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

            ImGui::End();
        }
    }

    if ( fileManagerFont ) ImGui::PopFont();
}

}  // namespace MMM::UI
