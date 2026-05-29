#define IMGUI_DEFINE_MATH_OPERATORS
#include "canvas/TimeFormatUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Polyline.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/menu/MainMenuView.h"
#include <ImGuiFileDialog.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>

namespace MMM::UI
{

/// @brief 扫描当前谱面中的重叠音符并缓存检测结果。
void MainMenuView::performOverlapScan()
{
    m_overlapResults.clear();
    m_hasOverlapScan = true;

    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session ) return;

    struct CheckItem {
        ::MMM::NoteType type;
        double          start_time;
        double          end_time;
        int             track;
        entt::entity    entity;
        entt::entity    parent_polyline;
        std::string     desc;
    };

    std::vector<CheckItem> items;
    const auto&            registry = session->getContext().noteRegistry;
    auto                   view = registry.view<const Logic::NoteComponent>();

    for ( auto entity : view ) {
        const auto& nc = view.get<const Logic::NoteComponent>(entity);

        // Skip Polyline container entities because its individual subnotes are
        // separate entities checked below
        if ( nc.m_type == ::MMM::NoteType::POLYLINE ) continue;

        double startTime = nc.m_timestamp;
        double endTime   = startTime;
        if ( nc.m_type == ::MMM::NoteType::HOLD ) {
            endTime = startTime + nc.m_duration;
        }

        std::string desc = "Note";
        if ( nc.m_type == ::MMM::NoteType::HOLD ) {
            desc = nc.m_isSubNote ? "Polyline Hold" : "Hold";
        } else if ( nc.m_type == ::MMM::NoteType::FLICK ) {
            desc = nc.m_isSubNote ? "Polyline Flick" : "Flick";
        }

        items.push_back({ nc.m_type,
                          startTime,
                          endTime,
                          nc.m_trackIndex,
                          entity,
                          nc.m_parentPolyline,
                          desc });
    }

    // Sort items by track, then by start_time
    std::sort(
        items.begin(), items.end(), [](const CheckItem& x, const CheckItem& y) {
            if ( x.track != y.track ) return x.track < y.track;
            return x.start_time < y.start_time;
        });

    // DSU structure for grouping contiguous overlapping notes
    struct DSU {
        std::vector<int> parent;
        DSU(size_t n)
        {
            parent.resize(n);
            for ( size_t i = 0; i < n; ++i ) parent[i] = i;
        }
        int find(int i)
        {
            if ( parent[i] == i ) return i;
            return parent[i] = find(parent[i]);
        }
        void unite(int i, int j)
        {
            int root_i = find(i);
            int root_j = find(j);
            if ( root_i != root_j ) {
                parent[root_i] = root_j;
            }
        }
    };

    DSU               dsu(items.size());
    std::vector<bool> isDefiniteOverlap(items.size(), false);
    std::vector<bool> hasAnyOverlap(items.size(), false);

    // Sweep-line with sliding window of max 10ms (0.010s) suspicion window
    for ( size_t i = 0; i < items.size(); ++i ) {
        const auto& a              = items[i];
        double      max_check_time = std::max(a.start_time, a.end_time) + 0.010;

        for ( size_t j = i + 1; j < items.size(); ++j ) {
            const auto& b = items[j];

            // If we hit a different track, stop search since it's sorted by
            // track
            if ( a.track != b.track ) break;

            // If start_time of b is beyond max_check_time, stop search since
            // it's sorted by start_time
            if ( b.start_time > max_check_time ) break;

            // Must not belong to the same Polyline
            if ( a.parent_polyline != entt::null &&
                 a.parent_polyline == b.parent_polyline )
                continue;

            double t1_start = a.start_time;
            double t1_end   = a.end_time;
            double t2_start = b.start_time;
            double t2_end   = b.end_time;

            // Sorted order guarantees t1_start <= t2_start
            double diff_start = t2_start - t1_start;

            bool is_definite  = false;
            bool is_suspected = false;

            // Strict time check in seconds (1ms = 0.001s, 10ms = 0.010s)
            if ( diff_start < 0.001 ) {
                is_definite = true;
            } else if ( t2_start > t1_start + 0.001 &&
                        t2_start < t1_end - 0.001 ) {
                is_definite = true;
            } else if ( diff_start >= 0.001 && diff_start <= 0.010 ) {
                is_suspected = true;
            } else if ( std::abs(t1_end - t2_start) >= 0.001 &&
                        std::abs(t1_end - t2_start) <= 0.010 ) {
                is_suspected = true;
            }

            if ( is_definite || is_suspected ) {
                dsu.unite(i, j);
                hasAnyOverlap[i] = true;
                hasAnyOverlap[j] = true;
                if ( is_definite ) {
                    isDefiniteOverlap[i] = true;
                    isDefiniteOverlap[j] = true;
                }
            }
        }
    }

    // Gather items into groups by DSU root
    std::unordered_map<int, std::vector<size_t>> groups;
    for ( size_t i = 0; i < items.size(); ++i ) {
        if ( hasAnyOverlap[i] ) {
            groups[dsu.find(i)].push_back(i);
        }
    }

    // Build the finalized grouped overlap results
    for ( const auto& pair : groups ) {
        const auto& indices = pair.second;
        if ( indices.size() < 2 ) continue;

        // Find the earliest start time, track, and whether the group is
        // definite
        double min_time    = items[indices[0]].start_time;
        int    track       = items[indices[0]].track;
        bool   is_definite = false;

        for ( size_t idx : indices ) {
            min_time = std::min(min_time, items[idx].start_time);
            if ( isDefiniteOverlap[idx] ) {
                is_definite = true;
            }
        }

        std::string desc1;
        std::string desc2;
        if ( indices.size() == 2 ) {
            desc1 = items[indices[0]].desc;
            desc2 = items[indices[1]].desc;
        } else {
            desc1 = TR_FMT("ui.tools.multiple_objects", indices.size());
            desc2 = TR("ui.tools.each_other").data();
        }

        m_overlapResults.push_back({ is_definite,
                                     min_time,
                                     static_cast<uint32_t>(track),
                                     desc1,
                                     desc2 });
    }
}

/// @brief 渲染重叠检测结果窗口。
void MainMenuView::renderOverlapCheckWindow()
{
    if ( !m_showOverlapCheckWindow ) return;

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
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

    ImGui::SetNextWindowSize(ImVec2(550.0f * dpiScale, 450.0f * dpiScale),
                             ImGuiCond_FirstUseEver);

    auto&   skinMgr   = Config::SkinManager::instance();
    ImFont* titleFont = skinMgr.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont);

    bool opened = ImGui::Begin(TR("ui.tools.overlap_check_title").data(),
                               &m_showOverlapCheckWindow,
                               ImGuiWindowFlags_None);

    if ( titleFont ) ImGui::PopFont();

    if ( opened ) {
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( !session ) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "%s",
                               TR("ui.tools.no_active_session").data());
        } else {
            auto beatmap = session->getContext().currentBeatmap;
            if ( !beatmap ) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "%s",
                                   TR("ui.tools.no_active_beatmap").data());
            } else {
                if ( !m_hasOverlapScan ) {
                    if ( ImGui::Button(TR("ui.tools.scan_now").data(),
                                       ImVec2(-1.0f, 40.0f * dpiScale)) ) {
                        performOverlapScan();
                    }
                } else {
                    if ( ImGui::Button(
                             TR("ui.tools.rescan").data(),
                             ImVec2(120.0f * dpiScale, 30.0f * dpiScale)) ) {
                        performOverlapScan();
                    }

                    ImGui::SameLine();
                    int definiteCount  = 0;
                    int suspectedCount = 0;
                    for ( const auto& r : m_overlapResults ) {
                        if ( r.is_definite )
                            definiteCount++;
                        else
                            suspectedCount++;
                    }

                    std::string summaryStr = TR_FMT(
                        "ui.tools.scan_summary", definiteCount, suspectedCount);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(summaryStr.c_str());

                    ImGui::Separator();
                    ImGui::Spacing();

                    if ( m_overlapResults.empty() ) {
                        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                           "%s",
                                           TR("ui.tools.no_overlaps").data());
                    } else {
                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;
                        if ( ImGui::BeginTable("OverlapResultsTable",
                                               5,
                                               tableFlags,
                                               ImVec2(0.0f, -1.0f)) ) {
                            ImGui::TableSetupColumn(
                                TR("ui.tools.overlap_type").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                100.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                TR("ui.canvas.note_time").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                90.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                TR("ui.canvas.track").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                60.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                TR("ui.tools.overlap_detail_header").data(),
                                ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(
                                TR("ui.tools.overlap_jump_header").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                50.0f * dpiScale);

                            ImGui::TableHeadersRow();

                            ImGuiListClipper clipper;
                            clipper.Begin(
                                static_cast<int>(m_overlapResults.size()));
                            while ( clipper.Step() ) {
                                for ( int i = clipper.DisplayStart;
                                      i < clipper.DisplayEnd;
                                      ++i ) {
                                    const auto& r = m_overlapResults[i];
                                    ImGui::TableNextRow();

                                    // 1. Type
                                    ImGui::TableNextColumn();
                                    if ( r.is_definite ) {
                                        ImGui::TextColored(
                                            ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                            "%s",
                                            TR("ui.tools.definite").data());
                                    } else {
                                        ImGui::TextColored(
                                            ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                            "%s",
                                            TR("ui.tools.suspected").data());
                                    }

                                    // 2. Time
                                    ImGui::TableNextColumn();
                                    const auto timeText =
                                        Canvas::formatCanvasTime(r.timestamp);
                                    ImGui::TextUnformatted(timeText.c_str());

                                    // 3. Track
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%d", r.track + 1);

                                    // 4. Detail
                                    ImGui::TableNextColumn();
                                    std::string detailStr =
                                        TR_FMT("ui.tools.overlap_detail",
                                               r.note1_desc,
                                               r.note2_desc);
                                    ImGui::TextUnformatted(detailStr.c_str());

                                    // 5. Jump Action
                                    ImGui::TableNextColumn();
                                    ImGui::PushStyleColor(ImGuiCol_Button,
                                                          ImVec4(0, 0, 0, 0));
                                    ImGui::PushStyleColor(
                                        ImGuiCol_ButtonHovered,
                                        ImVec4(0.4f, 0.7f, 1.0f, 0.3f));
                                    if ( ImGui::Button(
                                             fmt::format(
                                                 "{}##{}", ICON_MMM_SEARCH, i)
                                                 .c_str(),
                                             ImVec2(-1, 0)) ) {
                                        float visualOffset =
                                            Config::AppConfig::instance()
                                                .getVisualConfig()
                                                .getEffectiveVisualOffset();
                                        dispatchCommand(Logic::CmdSeek{
                                            r.timestamp - visualOffset });
                                    }
                                    ImGui::PopStyleColor(2);
                                    if ( ImGui::IsItemHovered() ) {
                                        const auto timeText =
                                            Canvas::formatCanvasTime(
                                                r.timestamp);
                                        ImGui::SetTooltip(
                                            "%s",
                                            TR_FMT("canvas.preview.jump_to",
                                                   timeText)
                                                .c_str());
                                    }
                                }
                            }
                            ImGui::EndTable();
                        }
                    }
                }
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
