#define IMGUI_DEFINE_MATH_OPERATORS
#include "canvas/TimeFormatUtils.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <imgui.h>
#include <mutex>
#include <string>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 切换重叠检测窗口动作。
class ToggleOverlapCheckWindowAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 切换重叠检测窗口打开状态。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        if ( !m_showWindow ) {
            ::MMM::UI::PlayPopupOpenFeedback();
        }
        m_showWindow = !m_showWindow;
    }

    /// @brief 渲染重叠检测结果窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅在窗口打开时绘制结果表。
    void renderDeferred(MainMenuContext& context) override
    {
        (void)context;
        renderWindow();
    }

private:
    /// @brief 单条重叠检测结果。
    struct OverlapResult {
        /// @brief 是否为确定重叠；false 表示疑似重叠。
        bool isDefinite = false;
        /// @brief 重叠发生的时间戳。
        double timestamp = 0.0;
        /// @brief 重叠发生的轨道编号。
        uint32_t track = 0;
        /// @brief 第一枚音符的描述文本。
        std::string note1Desc;
        /// @brief 第二枚音符的描述文本。
        std::string note2Desc;
    };

    /// @brief 扫描当前谱面中的重叠音符并缓存检测结果。
    void performScan()
    {
        m_results.clear();
        m_hasScan = true;

        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( !session ) return;

        struct CheckItem {
            /// @brief 音符类型。
            ::MMM::NoteType type{ ::MMM::NoteType::NOTE };
            /// @brief 起始时间。
            double startTime{ 0.0 };
            /// @brief 结束时间。
            double endTime{ 0.0 };
            /// @brief 起始轨道。
            int track{ 0 };
            /// @brief Flick 轨道增量。
            int dtrack{ 0 };
            /// @brief 音符实体。
            entt::entity entity{ entt::null };
            /// @brief 所属折线实体。
            entt::entity parentPolyline{ entt::null };
            /// @brief UI 显示描述。
            std::string desc;
        };

        std::vector<CheckItem> items;
        const auto&            registry = session->getContext().noteRegistry;
        auto view = registry.view<const Logic::NoteComponent>();

        for ( auto entity : view ) {
            const auto& nc = view.get<const Logic::NoteComponent>(entity);
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) continue;

            double startTime = nc.m_timestamp;
            double endTime   = startTime;
            if ( nc.m_type == ::MMM::NoteType::HOLD ) {
                endTime = startTime + std::max(0.0, nc.m_duration);
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
                              nc.m_dtrack,
                              entity,
                              nc.m_parentPolyline,
                              desc });
        }

        struct PairHit {
            /// @brief 第一项索引。
            size_t i{ 0 };
            /// @brief 第二项索引。
            size_t j{ 0 };
            /// @brief 重叠轨道。
            int track{ 0 };
            /// @brief 重叠时间。
            double time{ 0.0 };
        };

        struct PointProbe {
            /// @brief 检测时间。
            double time{ 0.0 };
            /// @brief 检测轨道。
            int track{ 0 };
            /// @brief 是否检测 Flick 跨轨身体。
            bool testsFlickBody{ false };
        };

        const double windowSeconds =
            static_cast<double>(std::max(0.0f,
                                         Config::AppConfig::instance()
                                             .getEditorSettings()
                                             .overlapTimeWindowMs)) *
            0.001;
        constexpr double timeEpsilon = 1e-7;

        auto samePolylineParent = [](const CheckItem& a, const CheckItem& b) {
            return a.parentPolyline != entt::null &&
                   a.parentPolyline == b.parentPolyline;
        };

        auto flickMinTrack = [](const CheckItem& item) {
            return std::min(item.track, item.track + item.dtrack);
        };
        auto flickMaxTrack = [](const CheckItem& item) {
            return std::max(item.track, item.track + item.dtrack);
        };

        auto collectPoints = [&](const CheckItem& item) {
            std::vector<PointProbe> points;
            if ( item.type == ::MMM::NoteType::NOTE ) {
                points.push_back({ item.startTime, item.track, true });
            } else if ( item.type == ::MMM::NoteType::HOLD ) {
                points.push_back({ item.startTime, item.track, true });
                if ( item.endTime > item.startTime + timeEpsilon ) {
                    points.push_back({ item.endTime, item.track, true });
                }
            } else if ( item.type == ::MMM::NoteType::FLICK ) {
                points.push_back({ item.startTime, item.track, false });
                points.push_back(
                    { item.startTime, item.track + item.dtrack, true });
            }
            return points;
        };

        auto isOverlapPair = [&](const CheckItem& a,
                                 const CheckItem& b,
                                 int&             overlapTrack,
                                 double&          overlapTime) {
            if ( samePolylineParent(a, b) ) return false;

            overlapTrack = a.track;
            overlapTime  = std::min(a.startTime, b.startTime);

            auto aPoints = collectPoints(a);
            auto bPoints = collectPoints(b);
            for ( const auto& aPoint : aPoints ) {
                for ( const auto& bPoint : bPoints ) {
                    if ( aPoint.track != bPoint.track ) continue;
                    if ( std::abs(aPoint.time - bPoint.time) >
                         windowSeconds + timeEpsilon )
                        continue;

                    overlapTrack = aPoint.track;
                    overlapTime  = std::min(aPoint.time, bPoint.time);
                    return true;
                }
            }

            auto flickBodyContainsPoint = [&](const CheckItem&  flick,
                                              const PointProbe& point) {
                if ( !point.testsFlickBody ||
                     flick.type != ::MMM::NoteType::FLICK || flick.dtrack == 0 )
                    return false;
                if ( std::abs(point.time - flick.startTime) >
                     windowSeconds + timeEpsilon )
                    return false;
                return point.track >= flickMinTrack(flick) &&
                       point.track <= flickMaxTrack(flick);
            };

            for ( const auto& point : bPoints ) {
                if ( flickBodyContainsPoint(a, point) ) {
                    overlapTrack = point.track;
                    overlapTime  = point.time;
                    return true;
                }
            }
            for ( const auto& point : aPoints ) {
                if ( flickBodyContainsPoint(b, point) ) {
                    overlapTrack = point.track;
                    overlapTime  = point.time;
                    return true;
                }
            }

            if ( a.type == ::MMM::NoteType::NOTE &&
                 b.type == ::MMM::NoteType::NOTE ) {
                return a.track == b.track &&
                       std::abs(a.startTime - b.startTime) < windowSeconds;
            }

            if ( a.type == ::MMM::NoteType::HOLD &&
                 b.type == ::MMM::NoteType::HOLD && a.track == b.track ) {
                double start = std::max(a.startTime, b.startTime);
                double end   = std::min(a.endTime, b.endTime);
                overlapTime  = start;
                return end > start + timeEpsilon;
            }

            if ( a.type == ::MMM::NoteType::FLICK &&
                 b.type == ::MMM::NoteType::FLICK && a.dtrack != 0 &&
                 b.dtrack != 0 &&
                 std::abs(a.startTime - b.startTime) <=
                     windowSeconds + timeEpsilon ) {
                int minTrack = std::max(flickMinTrack(a), flickMinTrack(b));
                int maxTrack = std::min(flickMaxTrack(a), flickMaxTrack(b));
                overlapTrack = minTrack;
                return maxTrack > minTrack;
            }

            auto holdBodyContainsPoint = [&](const CheckItem&  hold,
                                             const PointProbe& point) {
                return hold.type == ::MMM::NoteType::HOLD &&
                       point.track == hold.track &&
                       point.time > hold.startTime + timeEpsilon &&
                       point.time < hold.endTime - timeEpsilon;
            };

            for ( const auto& point : bPoints ) {
                if ( holdBodyContainsPoint(a, point) ) {
                    overlapTrack = point.track;
                    overlapTime  = point.time;
                    return true;
                }
            }
            for ( const auto& point : aPoints ) {
                if ( holdBodyContainsPoint(b, point) ) {
                    overlapTrack = point.track;
                    overlapTime  = point.time;
                    return true;
                }
            }

            auto holdFlickCrosses = [&](const CheckItem& hold,
                                        const CheckItem& flick) {
                if ( hold.type != ::MMM::NoteType::HOLD ||
                     flick.type != ::MMM::NoteType::FLICK || flick.dtrack == 0 )
                    return false;
                if ( flick.startTime <= hold.startTime + timeEpsilon ||
                     flick.startTime >= hold.endTime - timeEpsilon )
                    return false;
                return hold.track >= flickMinTrack(flick) &&
                       hold.track <= flickMaxTrack(flick);
            };

            if ( holdFlickCrosses(a, b) ) {
                overlapTrack = a.track;
                overlapTime  = b.startTime;
                return true;
            }
            if ( holdFlickCrosses(b, a) ) {
                overlapTrack = b.track;
                overlapTime  = a.startTime;
                return true;
            }

            return false;
        };

        std::vector<PairHit> pairHits;

        for ( size_t i = 0; i < items.size(); ++i ) {
            for ( size_t j = i + 1; j < items.size(); ++j ) {
                int    overlapTrack = 0;
                double overlapTime  = 0.0;
                if ( !isOverlapPair(
                         items[i], items[j], overlapTrack, overlapTime) )
                    continue;

                pairHits.push_back({ i, j, overlapTrack, overlapTime });
            }
        }

        std::sort(pairHits.begin(),
                  pairHits.end(),
                  [](const PairHit& a, const PairHit& b) {
                      if ( a.track != b.track ) return a.track < b.track;
                      return a.time < b.time;
                  });

        for ( size_t i = 0; i < pairHits.size(); ) {
            size_t j = i + 1;
            while ( j < pairHits.size() &&
                    pairHits[j].track == pairHits[i].track &&
                    pairHits[j].time <=
                        pairHits[i].time + windowSeconds + timeEpsilon ) {
                ++j;
            }

            std::vector<size_t> indices;
            indices.reserve((j - i) * 2);
            auto addUniqueIndex = [&](size_t index) {
                if ( std::find(indices.begin(), indices.end(), index) ==
                     indices.end() ) {
                    indices.push_back(index);
                }
            };

            double minTime = pairHits[i].time;
            for ( size_t k = i; k < j; ++k ) {
                addUniqueIndex(pairHits[k].i);
                addUniqueIndex(pairHits[k].j);
                minTime = std::min(minTime, pairHits[k].time);
            }

            std::string desc1;
            std::string desc2;
            if ( indices.size() == 2 && j == i + 1 ) {
                desc1 = items[pairHits[i].i].desc;
                desc2 = items[pairHits[i].j].desc;
            } else {
                desc1 = TR_FMT("ui.tools.multiple_objects", indices.size());
                desc2 = TR("ui.tools.each_other").data();
            }

            m_results.push_back({ true,
                                  minTime,
                                  static_cast<uint32_t>(pairHits[i].track),
                                  desc1,
                                  desc2 });
            i = j;
        }
    }

    /// @brief 渲染重叠检测结果窗口。
    /// @warning UI 热路径：每帧执行；仅在窗口打开时绘制结果表。
    void renderWindow()
    {
        if ( !m_showWindow ) return;

        auto& editorSettings =
            Config::AppConfig::instance().getEditorSettings();
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
            ImVec2(
                std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
                std::floor(editorSettings.aesthetics.windowPadding *
                           dpiScale)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

        ImGui::SetNextWindowSize(ImVec2(550.0f * dpiScale, 450.0f * dpiScale),
                                 ImGuiCond_FirstUseEver);

        auto&   skinMgr   = Config::SkinManager::instance();
        ImFont* titleFont = skinMgr.getFont("title");
        if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

        std::string windowTitle =
            TR("ui.tools.overlap_check_title").toString() +
            "###OverlapCheckWindow";
        const bool wasOpenBeforeBegin = m_showWindow;
        bool       opened = ImGui::Begin(windowTitle.c_str(), &m_showWindow);
        FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, &m_showWindow);

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
                    renderResults(dpiScale);
                }
            }
        }
        ImGui::End();

        ImGui::PopStyleVar(6);
    }

    /// @brief 渲染重叠检测结果内容。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：每帧执行；仅在窗口打开时绘制结果表。
    void renderResults(float dpiScale)
    {
        if ( !m_hasScan ) {
            if ( ::MMM::UI::FeedbackButton(TR("ui.tools.scan_now").data(),
                                           ImVec2(-1.0f, 40.0f * dpiScale)) ) {
                performScan();
            }
            return;
        }

        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.tools.rescan").data(),
                 ImVec2(120.0f * dpiScale, 30.0f * dpiScale)) ) {
            performScan();
        }

        ImGui::SameLine();
        int definiteCount  = 0;
        int suspectedCount = 0;
        for ( const auto& result : m_results ) {
            if ( result.isDefinite )
                definiteCount++;
            else
                suspectedCount++;
        }

        std::string summaryStr =
            TR_FMT("ui.tools.scan_summary", definiteCount, suspectedCount);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(summaryStr.c_str());

        ImGui::Separator();
        ImGui::Spacing();

        if ( m_results.empty() ) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                               "%s",
                               TR("ui.tools.no_overlaps").data());
            return;
        }

        ImGuiTableFlags tableFlags =
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable;
        Utils::VerticalScrollbarStyleScope scrollbarStyle(dpiScale);
        if ( !ImGui::BeginTable(
                 "OverlapResultsTable", 5, tableFlags, ImVec2(0.0f, -1.0f)) )
            return;

        ImGui::TableSetupColumn(TR("ui.tools.overlap_type").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                100.0f * dpiScale);
        ImGui::TableSetupColumn(TR("ui.canvas.note_time").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                90.0f * dpiScale);
        ImGui::TableSetupColumn(TR("ui.canvas.track").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                60.0f * dpiScale);
        ImGui::TableSetupColumn(TR("ui.tools.overlap_detail_header").data(),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(TR("ui.tools.overlap_jump_header").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                50.0f * dpiScale);

        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_results.size()));
        while ( clipper.Step() ) {
            for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i ) {
                renderResultRow(i);
            }
        }
        ImGui::EndTable();
    }

    /// @brief 渲染单条重叠检测结果。
    /// @param index 结果索引。
    /// @warning UI 热路径：仅在结果表可见行中执行。
    void renderResultRow(int index)
    {
        const auto& result = m_results[static_cast<std::size_t>(index)];
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        if ( result.isDefinite ) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "%s",
                               TR("ui.tools.definite").data());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "%s",
                               TR("ui.tools.suspected").data());
        }

        ImGui::TableNextColumn();
        const auto timeText = Canvas::formatCanvasTime(result.timestamp);
        ImGui::TextUnformatted(timeText.c_str());

        ImGui::TableNextColumn();
        ImGui::Text("%d", result.track + 1);

        ImGui::TableNextColumn();
        std::string detailStr = TR_FMT(
            "ui.tools.overlap_detail", result.note1Desc, result.note2Desc);
        ImGui::TextUnformatted(detailStr.c_str());

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.4f, 0.7f, 1.0f, 0.3f));
        if ( ::MMM::UI::FeedbackButton(
                 fmt::format("{}##{}", ICON_MMM_SEARCH, index).c_str(),
                 ImVec2(-1, 0)) ) {
            float visualOffset = Config::AppConfig::instance()
                                     .getVisualConfig()
                                     .getEffectiveVisualOffset();
            MenuUtil::dispatchCommand(
                Logic::CmdSeek{ result.timestamp - visualOffset });
        }
        ImGui::PopStyleColor(2);
        if ( ImGui::IsItemHovered() ) {
            const auto targetTimeText =
                Canvas::formatCanvasTime(result.timestamp);
            ImGui::SetTooltip(
                "%s", TR_FMT("canvas.preview.jump_to", targetTimeText).c_str());
        }
    }

    /// @brief 是否显示重叠检测窗口。
    bool m_showWindow = false;

    /// @brief 当前重叠检测结果是否已生成。
    bool m_hasScan = false;

    /// @brief 当前缓存的重叠检测结果。
    std::vector<OverlapResult> m_results;
};
}  // namespace

/// @brief 创建切换重叠检测窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createToggleOverlapCheckWindowAction()
{
    return std::make_unique<ToggleOverlapCheckWindowAction>();
}

}  // namespace MMM::UI
