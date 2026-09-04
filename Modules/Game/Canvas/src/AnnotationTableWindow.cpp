#include "canvas/AnnotationTableWindow.h"

#include "canvas/AuxiliaryWindowState.h"
#include "canvas/AuxiliaryWindowUi.h"
#include "config/AppConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <imgui_internal.h>

namespace MMM::Canvas
{
namespace
{
/// @brief 批注表读取逻辑缓存的最小间隔，单位秒。
constexpr double ANNOTATION_TABLE_REFRESH_INTERVAL_SECONDS = 0.1;

/// @brief 获取批注目标类型对应的翻译键。
/// @param targetKind 批注目标类型。
/// @return 可交给翻译系统的稳定键。
const char* annotationTargetLabelKey(
    ::MMM::BeatmapAnnotationTargetKind targetKind)
{
    switch ( targetKind ) {
    case ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT:
        return "ui.annotation.target.player_object";
    case ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE:
        return "ui.annotation.target.audio_sample";
    case ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP:
    default: return "ui.annotation.target.timestamp";
    }
}
}  // namespace

AnnotationTableWindow::AnnotationTableWindow(const std::string& name)
    : UI::IUIView(name)
{
}

bool AnnotationTableWindow::isWindowOpen() const
{
    return m_isWindowOpen;
}

void AnnotationTableWindow::setWindowOpen(bool open)
{
    m_isWindowOpen          = open;
    m_shouldRecoverWindow   = open;
    m_shouldFocusWindow     = false;
    m_isFocusedAndReachable = false;
    m_nextDataRefreshTime   = 0.0;
    if ( !open ) resetData();
}

void AnnotationTableWindow::activateWindow()
{
    const auto activation = resolveAuxiliaryWindowActivation(
        isWindowOpen(), m_isFocusedAndReachable);
    m_isWindowOpen        = activation.open;
    m_shouldFocusWindow   = activation.requestFocus;
    m_shouldRecoverWindow = activation.requestRecovery;
    m_nextDataRefreshTime = 0.0;
    if ( !activation.open ) {
        m_isFocusedAndReachable = false;
        resetData();
    }
}

void AnnotationTableWindow::restoreSelection(const std::string& selectedId)
{
    m_selectedRow.reset();
    const auto& rows = m_data.rows();
    if ( !selectedId.empty() ) {
        const auto selected =
            std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
                return row.id == selectedId;
            });
        if ( selected != rows.end() ) {
            m_selectedRow =
                static_cast<std::size_t>(std::distance(rows.begin(), selected));
        }
    }
    if ( !m_selectedRow && !rows.empty() ) m_selectedRow = 0U;
}

void AnnotationTableWindow::resetData()
{
    m_data.reset();
    m_dataStatus = AnnotationTableDataStatus::Close;
    m_selectedRow.reset();
}

void AnnotationTableWindow::closeWindow()
{
    m_isWindowOpen          = false;
    m_shouldRecoverWindow   = false;
    m_shouldFocusWindow     = false;
    m_isFocusedAndReachable = false;
    m_nextDataRefreshTime   = 0.0;
    resetData();
}

void AnnotationTableWindow::update(UI::UIManager* sourceManager)
{
    (void)sourceManager;
    if ( !isWindowOpen() ) {
        m_shouldRecoverWindow   = false;
        m_shouldFocusWindow     = false;
        m_isFocusedAndReachable = false;
        resetData();
        return;
    }

    const double now = ImGui::GetTime();
    if ( now >= m_nextDataRefreshTime ) {
        std::string selectedId;
        const auto& rowsBeforeRefresh = m_data.rows();
        if ( m_selectedRow && *m_selectedRow < rowsBeforeRefresh.size() ) {
            selectedId = rowsBeforeRefresh[*m_selectedRow].id;
        }
        const auto refreshResult = m_data.refresh();
        m_dataStatus             = refreshResult.status;
        m_nextDataRefreshTime = now + ANNOTATION_TABLE_REFRESH_INTERVAL_SECONDS;
        if ( m_dataStatus == AnnotationTableDataStatus::Close ) {
            closeWindow();
            return;
        }
        if ( refreshResult.rowsChanged ) restoreSelection(selectedId);
    }

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    const float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    const ImVec2 itemSpacing{
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
    };

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    ImGui::SetNextWindowSize(ImVec2(860.0F, 560.0F), ImGuiCond_FirstUseEver);
    if ( m_shouldFocusWindow ) {
        ImGui::SetNextWindowFocus();
    }
    std::string windowTitle =
        TR("ui.annotation.table.title").toString() + "###AnnotationTableWindow";
    bool&      windowOpen         = m_isWindowOpen;
    const bool wasOpenBeforeBegin = windowOpen;
    const bool opened = ImGui::Begin(windowTitle.c_str(), &windowOpen);
    ::MMM::UI::FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin,
                                                &windowOpen);
    if ( windowOpen ) {
        recoverCurrentAuxiliaryWindow(m_shouldRecoverWindow, dpiScale);
        const bool focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool reachable = isCurrentAuxiliaryWindowReachable(dpiScale);
        const bool popupOpen = ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        m_isFocusedAndReachable = resolveAuxiliaryWindowFocusedAndReachable(
            m_isFocusedAndReachable, reachable, focused, popupOpen);
    } else {
        m_isFocusedAndReachable = false;
    }
    m_shouldRecoverWindow = false;
    m_shouldFocusWindow   = false;

    if ( opened ) {
        if ( m_dataStatus != AnnotationTableDataStatus::Ready ) {
            ImGui::TextDisabled("%s", TR("ui.annotation.table.syncing").data());
        } else {
            const auto& rows = m_data.rows();
            ImGui::Text(
                "%s", TR_FMT("ui.annotation.table.count", rows.size()).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TR("ui.annotation.table.hint").data());

            const float availableHeight = ImGui::GetContentRegionAvail().y;
            const float detailHeight =
                std::clamp(availableHeight * 0.38F, 150.0F, 260.0F);
            const float tableHeight =
                std::max(160.0F,
                         availableHeight - detailHeight -
                             ImGui::GetStyle().ItemSpacing.y - 4.0F);
            constexpr ImGuiTableFlags tableFlags =
                ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            if ( ImGui::BeginTable("AnnotationTableRows",
                                   6,
                                   tableFlags,
                                   ImVec2(0.0F, tableHeight)) ) {
                if ( ImGuiTable* table = ImGui::GetCurrentTable() ) {
                    table->DisableDefaultContextMenu = true;
                }
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(TR("ui.annotation.table.index").data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        52.0F * dpiScale);
                ImGui::TableSetupColumn(TR("ui.annotation.timestamp").data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        125.0F * dpiScale);
                ImGui::TableSetupColumn(TR("ui.annotation.target").data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        140.0F * dpiScale);
                ImGui::TableSetupColumn(TR("ui.annotation.author").data(),
                                        ImGuiTableColumnFlags_WidthStretch,
                                        0.75F);
                ImGui::TableSetupColumn(
                    TR("ui.annotation.table.content").data(),
                    ImGuiTableColumnFlags_WidthStretch,
                    1.7F);
                const ImGuiStyle& style = ImGui::GetStyle();
                const float       actionButtonWidth =
                    std::max(
                        ImGui::CalcTextSize(
                            TR("ui.annotation.table.jump").data())
                            .x,
                        ImGui::CalcTextSize(TR("ui.common.delete").data()).x) +
                    style.FramePadding.x * 2.0F;
                const float actionColumnWidth = actionButtonWidth * 2.0F +
                                                style.ItemSpacing.x +
                                                8.0F * dpiScale;
                ImGui::TableSetupColumn(TR("ui.annotation.table.action").data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        actionColumnWidth);
                ImGui::TableHeadersRow();

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(rows.size()));
                while ( clipper.Step() ) {
                    for ( int rowIndex = clipper.DisplayStart;
                          rowIndex < clipper.DisplayEnd;
                          ++rowIndex ) {
                        const auto  index = static_cast<std::size_t>(rowIndex);
                        const auto& row   = rows[index];
                        const bool  selected = m_selectedRow == index;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        const std::string rowLabel = fmt::format(
                            "#{}###AnnotationTableRow_{}", index + 1U, index);
                        const bool rowClicked = ::MMM::UI::FeedbackSelectable(
                            rowLabel.c_str(),
                            selected,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0.0F, ImGui::GetFrameHeight()));
                        const bool rowDoubleClicked =
                            ImGui::IsItemHovered() &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                        if ( rowClicked ) {
                            m_selectedRow = index;
                        }

                        const auto seekToRow = [&row]() {
                            const float visualOffset =
                                Config::AppConfig::instance()
                                    .getVisualConfig()
                                    .getEffectiveVisualOffset();
                            Event::EventBus::instance().publish(
                                Event::LogicCommandEvent(Logic::CmdSeek{
                                    row.timestamp - visualOffset }));
                        };
                        if ( rowDoubleClicked ) seekToRow();

                        ImGui::TableSetColumnIndex(1);
                        ImGui::AlignTextToFramePadding();
                        const auto timeText = MMM::UI::Utils::formatCanvasTime(
                            row.timestamp, m_data.timeFormatContext());
                        ImGui::TextUnformatted(timeText.c_str());

                        ImGui::TableSetColumnIndex(2);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(
                            TR(annotationTargetLabelKey(row.targetKind))
                                .data());
                        if ( row.track >= 0 ) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("#%d", row.track + 1);
                        }
                        if ( row.targetMissing ) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0F, 0.42F, 0.32F, 1.0F),
                                               "!");
                        }

                        ImGui::TableSetColumnIndex(3);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(
                            row.author.empty()
                                ? TR("ui.annotation.unknown_author").data()
                                : row.author.c_str());

                        ImGui::TableSetColumnIndex(4);
                        ImGui::AlignTextToFramePadding();
                        const auto        firstLineEnd = row.content.find('\n');
                        const std::size_t firstLineLength =
                            firstLineEnd == std::string::npos
                                ? row.content.size()
                                : firstLineEnd;
                        ImGui::TextUnformatted(
                            row.content.data(),
                            row.content.data() + firstLineLength);

                        ImGui::TableSetColumnIndex(5);
                        const float rowActionWidth =
                            std::max(1.0F,
                                     (ImGui::GetContentRegionAvail().x -
                                      ImGui::GetStyle().ItemSpacing.x) /
                                         2.0F);
                        const std::string jumpLabel =
                            fmt::format("{}##AnnotationTableJump_{}",
                                        TR("ui.annotation.table.jump").view(),
                                        index);
                        if ( ::MMM::UI::FeedbackButton(
                                 jumpLabel.c_str(),
                                 ImVec2(rowActionWidth,
                                        ImGui::GetFrameHeight())) ) {
                            seekToRow();
                        }
                        ImGui::SameLine();
                        const std::string deleteLabel =
                            fmt::format("{}##AnnotationTableDelete_{}",
                                        TR("ui.common.delete").view(),
                                        index);
                        if ( ::MMM::UI::FeedbackButton(
                                 deleteLabel.c_str(),
                                 ImVec2(rowActionWidth,
                                        ImGui::GetFrameHeight())) ) {
                            Event::EventBus::instance().publish(
                                Event::LogicCommandEvent(
                                    Logic::CmdRemoveBeatmapAnnotation{
                                        row.id }));
                        }
                    }
                }
                ImGui::EndTable();
            }

            if ( rows.empty() ) {
                ImGui::TextDisabled("%s",
                                    TR("ui.annotation.table.empty").data());
            } else if ( m_selectedRow && *m_selectedRow < rows.size() ) {
                const auto& selected = rows[*m_selectedRow];
                ImGui::BeginChild("AnnotationTableDetail",
                                  ImVec2(0.0F, detailHeight),
                                  ImGuiChildFlags_Borders);
                const auto timeText = MMM::UI::Utils::formatCanvasTime(
                    selected.timestamp, m_data.timeFormatContext());
                ImGui::Text("%s: %s",
                            TR("ui.annotation.timestamp").data(),
                            timeText.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "· %s",
                    selected.author.empty()
                        ? TR("ui.annotation.unknown_author").data()
                        : selected.author.c_str());
                ImGui::Text(
                    "%s: %s",
                    TR("ui.annotation.target").data(),
                    TR(annotationTargetLabelKey(selected.targetKind)).data());
                if ( selected.track >= 0 ) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("#%d", selected.track + 1);
                }
                if ( selected.targetMissing ) {
                    ImGui::SameLine();
                    ImGui::TextColored(
                        ImVec4(1.0F, 0.42F, 0.32F, 1.0F),
                        "%s",
                        TR("ui.annotation.target_missing").data());
                }
                ImGui::Separator();
                UI::renderMarkdown(selected.content);
                ImGui::EndChild();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(6);

    if ( !windowOpen ) resetData();
}

}  // namespace MMM::Canvas
