#include "ui/imgui/manager/BeatMapManagerView.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace MMM::UI
{
namespace
{
/// @brief 谱面表格列编号。
enum BeatmapTableColumn : int {
    /// @brief 名称列。
    BeatmapTableColumnName = 0,

    /// @brief 类型列。
    BeatmapTableColumnType = 1,

    /// @brief 文件路径列。
    BeatmapTableColumnPath = 2
};

/// @brief 将 ASCII 字符串转换为小写，用于类型归一化和排序。
/// @param value 输入字符串。
/// @return 小写后的字符串。
std::string toLowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            if ( ch >= 'A' && ch <= 'Z' ) {
                return static_cast<char>(ch - 'A' + 'a');
            }
            return static_cast<char>(ch);
        });
    return value;
}

/// @brief 根据谱面路径推导显示类型。
/// @param filePath 项目内谱面相对路径。
/// @return 面向谱面管理器的短类型文本。
std::string beatmapTypeFromPath(const std::string& filePath)
{
    auto extension = toLowerAscii(
        Config::pathToUtf8(Config::utf8ToPath(filePath).extension()));
    if ( extension == ".osu" ) {
        return "osu";
    }
    if ( extension == ".mc" ) {
        return "malody";
    }
    if ( extension == ".imd" ) {
        return "rm";
    }
    if ( extension == ".mmm" ) {
        return "mmm";
    }
    if ( extension.size() > 1 && extension.front() == '.' ) {
        extension.erase(extension.begin());
    }
    return extension.empty() ? TR("ui.file_manager.value_unknown").data()
                             : extension;
}

/// @brief 绘制可裁剪、超宽自动滚动的表格单元格文本。
/// @param text 需要绘制的文本。
/// @param cursorPos 单元格起始屏幕坐标。
/// @param width 单元格可用宽度。
/// @param height 行高。
void renderScrollingTableText(const std::string& text, ImVec2 cursorPos,
                              float width, float height)
{
    const float  textWidth = std::max(0.0f, width);
    const ImVec2 textSize  = ImGui::CalcTextSize(text.c_str());
    const float  textH     = ImGui::GetFontSize();
    const float  offsetY   = (height - textH) * 0.5f;

    float offset = 0.0f;
    if ( textSize.x > textWidth ) {
        const float scrollRange = textSize.x - textWidth + 40.0f;
        const float time        = static_cast<float>(ImGui::GetTime());
        float       t           = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
        t                       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
        offset                  = t * scrollRange;
    }

    const ImVec2 textStartPos = cursorPos;
    ImGui::PushClipRect(
        textStartPos,
        ImVec2(textStartPos.x + textWidth, textStartPos.y + height),
        true);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(textStartPos.x - offset, textStartPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    ImGui::PopClipRect();
}

}  // namespace

/// @brief 获取谱面管理器中不可再换行控件所需的最小内容尺寸。
/// @warning UI 热路径：子视图可见时每帧查询；仅保留轻量文本测量。
ImVec2 BeatMapManagerView::getMinContentSize(float dpiScale) const
{
    auto&       engine    = Logic::EditorEngine::instance();
    auto*       project   = engine.getCurrentProject();
    const float scale     = std::max(1.0f, dpiScale);
    const float panelPad  = 4.0f * scale;
    const float rowHeight = 28.0f * scale;
    const float footerH   = 44.0f * scale;
    const float headerWidth =
        ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x +
        ImGui::CalcTextSize(TR("ui.beatmap_manager.beatmaps").data()).x;

    if ( !project ) {
        const char* hint = TR("ui.beatmap_manager.initial_hint").data();
        return ImVec2(
            std::ceil(ImGui::CalcTextSize(hint).x + panelPad * 2.0f),
            std::ceil(ImGui::GetTextLineHeightWithSpacing() + panelPad * 2.0f));
    }

    const float visibleRows =
        m_showBeatmapList ? std::min<size_t>(project->m_beatmaps.size(), 1) : 0;
    const float minHeight = panelPad * 2.0f + ImGui::GetFrameHeight() +
                            visibleRows * rowHeight + footerH;
    return ImVec2(std::ceil(headerWidth + panelPad * 2.0f),
                  std::ceil(minHeight));
}

void BeatMapManagerView::onUpdate(LayoutContext& layoutContext,
                                  UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    float   dpiScale        = layoutContext.m_dpiScale;
    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) {
        ImGui::PushFont(fileManagerFont, fileManagerFont->LegacySize);
    }

    const float panelPadding = std::min(
        4.0f * dpiScale, std::max(0.0f, layoutContext.m_avail.x) * 0.02f);
    const float rowHeight = 28.0f * dpiScale;

    if ( !project ) {
        const char* hint     = TR("ui.beatmap_manager.initial_hint").data();
        ImVec2      textSize = ImGui::CalcTextSize(hint);
        ImVec2      textPos  = {
            layoutContext.m_startPos.x +
                std::max(0.0f, layoutContext.m_avail.x - textSize.x) * 0.5f,
            layoutContext.m_startPos.y +
                std::max(0.0f, layoutContext.m_avail.y - textSize.y) * 0.5f
        };
        ImGui::SetCursorScreenPos(textPos);
        ImGui::TextDisabled("%s", hint);

        if ( fileManagerFont ) ImGui::PopFont();
        return;
    }

    float  footerH  = 44.0f * dpiScale;
    float  listH    = std::max(0.0f, layoutContext.m_avail.y - footerH);
    ImVec2 listPos  = { layoutContext.m_startPos.x + panelPadding,
                        layoutContext.m_startPos.y + panelPadding };
    ImVec2 listSize = { std::max(0.0f,
                                 layoutContext.m_avail.x - panelPadding * 2.0f),
                        std::max(0.0f, listH - panelPadding) };

    ImGui::SetCursorScreenPos(listPos);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(
        "BeatmapListChild", listSize, false, ImGuiWindowFlags_None);

    ImVec2           headerPos = ImGui::GetCursorScreenPos();
    Clay_BoundingBox headerBox{ .x      = headerPos.x,
                                .y      = headerPos.y,
                                .width  = ImGui::GetContentRegionAvail().x,
                                .height = ImGui::GetFrameHeight() };
    Utils::renderCollapsingHeader(TR("ui.beatmap_manager.beatmaps").data(),
                                  &m_showBeatmapList,
                                  headerBox);

    if ( m_showBeatmapList ) {
        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp;

        if ( ImGui::BeginTable("BeatmapManagerTable",
                               3,
                               tableFlags,
                               ImGui::GetContentRegionAvail()) ) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_name").data(),
                ImGuiTableColumnFlags_DefaultSort |
                    ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_type").data(),
                ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(76.0f, 86.0f * ImGui::GetFontSize() / 17.0f));
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_path").data(),
                ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            ImGui::TableHeadersRow();

            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if ( sortSpecs && sortSpecs->SpecsCount > 0 &&
                 sortSpecs->SpecsDirty ) {
                const ImGuiTableColumnSortSpecs& primarySpec =
                    sortSpecs->Specs[0];
                BeatmapSortKey newSortKey = BeatmapSortKey::Name;
                if ( primarySpec.ColumnIndex == BeatmapTableColumnType ) {
                    newSortKey = BeatmapSortKey::Type;
                } else if ( primarySpec.ColumnIndex ==
                            BeatmapTableColumnPath ) {
                    newSortKey = BeatmapSortKey::Path;
                }

                const SortDirection newDirection =
                    primarySpec.SortDirection == ImGuiSortDirection_Descending
                        ? SortDirection::Descending
                        : SortDirection::Ascending;
                if ( newSortKey != m_beatmapSortKey ||
                     newDirection != m_beatmapSortDirection ) {
                    m_beatmapSortKey        = newSortKey;
                    m_beatmapSortDirection  = newDirection;
                    m_beatmapSortCacheDirty = true;
                }
                sortSpecs->SpecsDirty = false;
            }

            auto rebuildSortCache = [&]() {
                const auto& beatmaps = project->m_beatmaps;
                m_sortedBeatmapIndices.resize(beatmaps.size());
                std::iota(m_sortedBeatmapIndices.begin(),
                          m_sortedBeatmapIndices.end(),
                          size_t{ 0 });
                std::stable_sort(
                    m_sortedBeatmapIndices.begin(),
                    m_sortedBeatmapIndices.end(),
                    [&](size_t lhsIndex, size_t rhsIndex) {
                        const auto& lhs           = beatmaps[lhsIndex];
                        const auto& rhs           = beatmaps[rhsIndex];
                        int         compareResult = 0;
                        switch ( m_beatmapSortKey ) {
                        case BeatmapSortKey::Name:
                            compareResult =
                                toLowerAscii(lhs.m_name)
                                    .compare(toLowerAscii(rhs.m_name));
                            break;
                        case BeatmapSortKey::Type:
                            compareResult = beatmapTypeFromPath(lhs.m_filePath)
                                                .compare(beatmapTypeFromPath(
                                                    rhs.m_filePath));
                            break;
                        case BeatmapSortKey::Path:
                            compareResult =
                                toLowerAscii(lhs.m_filePath)
                                    .compare(toLowerAscii(rhs.m_filePath));
                            break;
                        }

                        if ( compareResult == 0 ) {
                            compareResult =
                                toLowerAscii(lhs.m_filePath)
                                    .compare(toLowerAscii(rhs.m_filePath));
                        }
                        if ( m_beatmapSortDirection ==
                             SortDirection::Descending ) {
                            compareResult = -compareResult;
                        }
                        return compareResult < 0;
                    });
                m_cachedBeatmapCount    = beatmaps.size();
                m_cachedBeatmapProject  = project;
                m_beatmapSortCacheDirty = false;
            };

            if ( m_beatmapSortCacheDirty ||
                 m_cachedBeatmapCount != project->m_beatmaps.size() ||
                 m_cachedBeatmapProject != project ) {
                rebuildSortCache();
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_sortedBeatmapIndices.size()),
                          rowHeight);
            while ( clipper.Step() ) {
                for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                      ++row ) {
                    const size_t beatmapIndex =
                        m_sortedBeatmapIndices[static_cast<size_t>(row)];
                    if ( beatmapIndex >= project->m_beatmaps.size() ) {
                        continue;
                    }

                    const auto& beatmap = project->m_beatmaps[beatmapIndex];
                    const auto  typeText =
                        beatmapTypeFromPath(beatmap.m_filePath);

                    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                    ImGui::TableNextColumn();

                    const ImVec2 nameCellPos = ImGui::GetCursorScreenPos();
                    const float  nameCellWidth =
                        ImGui::GetContentRegionAvail().x;
                    const std::string rowId =
                        "##BeatmapRow_" + beatmap.m_filePath;
                    const bool clicked = ::MMM::UI::FeedbackSelectable(
                        rowId.c_str(),
                        false,
                        ImGuiSelectableFlags_SpanAllColumns,
                        { 0.0f, rowHeight });
                    const bool hovered = ImGui::IsItemHovered();
                    if ( clicked ) {
                        XINFO("Request to load beatmap: {}", beatmap.m_name);
                        auto fullPath = project->m_projectRoot /
                                        Config::utf8ToPath(beatmap.m_filePath);
                        auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                            MMM::BeatMap::loadFromFile(fullPath));
                        engine.createSession(loadedBeatmap, beatmap.m_name);
                    }

                    if ( hovered &&
                         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
                        m_manageBeatmapPath = beatmap.m_filePath;
                        m_openManageModal   = true;
                    }
                    if ( hovered ) {
                        ImGui::SetTooltip("File: %s\nType: %s\nTrack: %s",
                                          beatmap.m_filePath.c_str(),
                                          typeText.c_str(),
                                          beatmap.m_audioTrackId.c_str());
                    }

                    const std::string nameText =
                        std::string(ICON_MMM_FILE) + "  " + beatmap.m_name;
                    renderScrollingTableText(
                        nameText, nameCellPos, nameCellWidth, rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(typeText,
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(beatmap.m_filePath,
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    float  btnSize    = 32.0f * dpiScale;
    ImVec2 footerPos  = { layoutContext.m_startPos.x + panelPadding,
                          layoutContext.m_startPos.y + listH };
    ImVec2 footerSize = {
        std::max(0.0f, layoutContext.m_avail.x - panelPadding * 2.0f), footerH
    };
    ImVec2 buttonPos = {
        footerPos.x, footerPos.y + std::max(0.0f, footerSize.y - btnSize) * 0.5f
    };

    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
    Utils::pushFixedButtonStyleVars();

    ImGui::SetCursorScreenPos(buttonPos);
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    ImVec4      bgCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    bgCol.w *= 0.5f;
    float rounding = ImGui::GetStyle().FrameRounding;
    if ( ImGui::IsMouseHoveringRect(
             buttonPos,
             { buttonPos.x + footerSize.x, buttonPos.y + btnSize }) ) {
        bgCol.w *= 1.5f;
    }

    dl->AddRectFilled(buttonPos,
                      { buttonPos.x + footerSize.x, buttonPos.y + btnSize },
                      ImGui::ColorConvertFloat4ToU32(bgCol),
                      rounding);

    if ( ::MMM::UI::FeedbackButton(ICON_MMM_PLUS,
                                   ImVec2(footerSize.x, btnSize)) ) {
        auto* wizard =
            sourceManager->getView<NewBeatmapWizard>("NewBeatmapWizard");
        if ( wizard ) wizard->open();
    }

    Utils::popFixedButtonStyleVars();
    ImGui::PopStyleColor(4);
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", TR_CACHE("ui.file.new_map").data());
    }

    // --- 3. 谱面管理窗口 ---
    bool showBMModal = !m_manageBeatmapPath.empty();
    if ( showBMModal ) {
        std::string windowTitle =
            fmt::format("{} {}",
                        TR("ui.beatmap_manager.manage_title").data(),
                        m_manageBeatmapPath);
        if ( m_openManageModal ) {
            m_openManageModal = false;
        }
        Utils::CenteredModalPopupScope manageWindowScope(dpiScale);
        if ( manageWindowScope.beginWindow(windowTitle.c_str(),
                                           &showBMModal,
                                           ImGuiWindowFlags_NoCollapse,
                                           { 420 * dpiScale, 0.0f }) ) {
            if ( !showBMModal ) {
                m_manageBeatmapPath = "";
            }

            // --- 使用 Clay 重构对话框内容 ---
            CLayVBox    modalLayout;
            const auto& modalStyle     = ImGui::GetStyle();
            auto        toLayoutPixels = [](float value) {
                return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
            };
            float padding =
                std::max(16.0f * dpiScale, modalStyle.WindowPadding.x);
            const float modalGap =
                std::max(16.0f * dpiScale, modalStyle.ItemSpacing.y);
            const float buttonGap =
                std::max(12.0f * dpiScale, modalStyle.ItemSpacing.x);
            const float buttonH =
                std::max(32.0f * dpiScale, ImGui::GetFrameHeight());
            const float removeButtonW =
                std::max(140.0f * dpiScale,
                         ImGui::CalcTextSize(
                             TR("ui.beatmap_manager.remove_beatmap").data())
                                 .x +
                             modalStyle.FramePadding.x * 2.0f);
            const float cancelButtonW =
                std::max(100.0f * dpiScale,
                         ImGui::CalcTextSize(TR("ui.common.cancel").data()).x +
                             modalStyle.FramePadding.x * 2.0f);
            const uint16_t modalPaddingPx = toLayoutPixels(padding);
            modalLayout.setPadding(
                modalPaddingPx, modalPaddingPx, modalPaddingPx, modalPaddingPx);
            modalLayout.setSpacing(toLayoutPixels(modalGap));

            // 1. 标题与信息 (移除了冗余 Text)
            modalLayout.addElement(
                "ModalSep",
                Sizing::Grow(),
                Sizing::Fixed(1),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::GetWindowDrawList()->AddLine(
                        { r.x, r.y },
                        { r.x + r.width, r.y },
                        ImGui::GetColorU32(ImGuiCol_Separator));
                });

            // 2. 操作按钮区
            CLayHBox btnRow;
            btnRow.setAlignment(Alignment::Center());
            btnRow.setSpacing(toLayoutPixels(buttonGap));

            btnRow.addElement(
                "RemoveBtn",
                Sizing::Fixed(removeButtonW),
                Sizing::Fixed(buttonH),
                [=](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.beatmap_manager.remove_beatmap").data(),
                             { r.width, r.height }) ) {
                        ImGui::OpenPopup("RemoveBeatmapConfirm");
                    }
                });

            btnRow.addElement("CancelBtn",
                              Sizing::Fixed(cancelButtonW),
                              Sizing::Fixed(buttonH),
                              [=, this](Clay_BoundingBox r, bool) {
                                  ImGui::SetCursorScreenPos({ r.x, r.y });
                                  if ( ::MMM::UI::FeedbackButton(
                                           TR("ui.common.cancel").data(),
                                           { r.width, r.height }) ) {
                                      m_manageBeatmapPath = "";
                                  }
                              });

            modalLayout.addLayout(
                "BtnRowLayout", btnRow, Sizing::Grow(), Sizing::Fixed(buttonH));

            // 渲染布局
            ImVec2 modalSize = modalLayout.renderInCurrent(
                ImGui::GetCursorScreenPos(),
                { ImGui::GetContentRegionAvail().x, 0 });
            ImGui::Dummy(modalSize);

            // --- 二次确认弹窗 ---
            {
                Utils::CenteredModalPopupScope removeModalScope(dpiScale);
                if ( removeModalScope.begin("RemoveBeatmapConfirm") ) {
                    ImGui::Text("%s",
                                TR("ui.beatmap_manager.remove_confirm").data());
                    ImGui::Spacing();
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.confirm").data(),
                             { 100 * dpiScale, 0 }) ) {
                        engine.pushCommand(
                            Logic::CmdRemoveBeatmap{ m_manageBeatmapPath });
                        m_manageBeatmapPath = "";
                    }
                    ImGui::SameLine();
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.cancel").data(),
                             { 100 * dpiScale, 0 }) ) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }

            ImGui::End();
        }
    }

    if ( fileManagerFont ) ImGui::PopFont();
}

}  // namespace MMM::UI
