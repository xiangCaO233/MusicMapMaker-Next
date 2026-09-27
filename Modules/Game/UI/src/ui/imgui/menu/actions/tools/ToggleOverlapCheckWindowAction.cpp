#define IMGUI_DEFINE_MATH_OPERATORS
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/TimeFormatUtils.h"
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
/// @details 显式扫描当前音符 Registry，缓存成可跳转的重叠结果表。
/// @warning 扫描为平方复杂度低频操作，只能由用户按钮触发。
class ToggleOverlapCheckWindowAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 切换重叠检测窗口打开状态。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变窗口切换语义。
    /// @note 关闭窗口不会清除上次扫描结果，重新打开可继续查看。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        if ( !m_showWindow ) {
            // 只在从关闭切换到打开时播放弹窗反馈。
            ::MMM::UI::PlayPopupOpenFeedback();
        }
        // 单一布尔值同时由菜单动作和窗口关闭按钮维护。
        m_showWindow = !m_showWindow;
    }

    /// @brief 渲染重叠检测结果窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅在窗口打开时绘制结果表。
    /// @note 窗口从 AppConfig 自行读取 DPI 与圆角设置，无需菜单上下文。
    void renderDeferred(MainMenuContext& context) override
    {
        (void)context;
        renderWindow();
    }

private:
    /// @brief 单条重叠检测结果。
    /// @details 将扫描内部索引转换为稳定显示数据，结果不持有 ECS 实体引用。
    struct OverlapResult {
        /// @brief 是否为确定重叠；false 表示疑似重叠。
        /// @note 当前扫描算法生成的命中均标记为确定重叠。
        bool isDefinite = false;
        /// @brief 重叠发生的时间戳。
        /// @note 使用命中端点或区间交集的最早时间。
        double timestamp = 0.0;
        /// @brief 重叠发生的轨道编号。
        /// @note 内部从零开始，表格显示时转换为从一开始。
        uint32_t track = 0;
        /// @brief 第一枚音符的描述文本。
        /// @note 多对象聚类时改为本地化数量摘要。
        std::string note1Desc;
        /// @brief 第二枚音符的描述文本。
        /// @note 多对象聚类时使用“互相重叠”说明。
        std::string note2Desc;
    };

    /// @brief 扫描当前谱面中的重叠音符并缓存检测结果。
    /// @warning 低频用户路径：锁定 Session、遍历完整音符
    /// Registry、排序并执行平方配对。
    /// @note 扫描开始即清除旧结果；无活动会话时保留已扫描空状态。
    /// @note 时间窗口在扫描开始时读取一次，保证本轮全部配对使用同一阈值。
    void performScan()
    {
        // 新扫描不与旧谱面结果混合。
        m_results.clear();
        // 即使会话为空也记录已扫描，窗口随后显示无结果而非首次按钮。
        m_hasScan = true;

        // 扫描期间通过 EditorEngine 取得当前活动 Session。
        auto& engine = Logic::EditorEngine::instance();
        // 互斥锁保护 Session 切换和完整 Registry 读取。
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        // 没有活动会话时安全结束，窗口层会显示会话提示。
        if ( !session ) return;

        // 内部条目将不同音符类型归一为时间区间、轨道范围和显示描述。
        struct CheckItem {
            /// @brief 音符类型。
            /// @note 只保留 NOTE、HOLD 与 FLICK，POLYLINE 容器本身被排除。
            ::MMM::NoteType type{ ::MMM::NoteType::NOTE };
            /// @brief 起始时间。
            /// @note 来自 NoteComponent 的谱面时间戳。
            double startTime{ 0.0 };
            /// @brief 结束时间。
            /// @note 普通音符和 Flick 与 startTime 相同，Hold 为区间末端。
            double endTime{ 0.0 };
            /// @brief 起始轨道。
            /// @note 对 Flick 表示滑动起点轨道。
            int track{ 0 };
            /// @brief Flick 轨道增量。
            /// @note 非 Flick 条目保持零值。
            int dtrack{ 0 };
            /// @brief 音符实体。
            /// @note 仅用于扫描内部身份记录，结果表不保存该句柄。
            entt::entity entity{ entt::null };
            /// @brief 所属折线实体。
            /// @note 同一折线的子物件对被排除，避免把连续结构报告为冲突。
            entt::entity parentPolyline{ entt::null };
            /// @brief UI 显示描述。
            /// @note 区分独立音符与折线子音符类型。
            std::string desc;
        };

        std::vector<CheckItem> items;
        // Registry 引用仅在 Session 锁保护范围内使用。
        const auto& registry = session->getContext().noteRegistry;
        // 视图只选取具有 NoteComponent 的实体。
        auto view = registry.view<const Logic::NoteComponent>();

        // 此处是用户显式扫描路径，允许完整遍历当前音符 Registry。
        for ( auto entity : view ) {
            const auto& nc = view.get<const Logic::NoteComponent>(entity);
            // 折线容器没有独立可判定几何，其子音符会作为普通条目参与扫描。
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) continue;

            // 所有条目至少具有一个起始时刻。
            double startTime = nc.m_timestamp;
            double endTime   = startTime;
            if ( nc.m_type == ::MMM::NoteType::HOLD ) {
                // 负持续时间按零处理，避免构造反向区间。
                endTime = startTime + std::max(0.0, nc.m_duration);
            }

            // 默认描述覆盖普通点击音符。
            std::string desc = "Note";
            if ( nc.m_type == ::MMM::NoteType::HOLD ) {
                // 子 Hold 保留所属折线语义，便于结果辨识。
                desc = nc.m_isSubNote ? "Polyline Hold" : "Hold";
            } else if ( nc.m_type == ::MMM::NoteType::FLICK ) {
                // Flick 同样区分独立对象和折线子物件。
                desc = nc.m_isSubNote ? "Polyline Flick" : "Flick";
            }

            // 聚合条目按值保存，后续配对不再访问 Registry 组件。
            items.push_back({ nc.m_type,
                              startTime,
                              endTime,
                              nc.m_trackIndex,
                              nc.m_dtrack,
                              entity,
                              nc.m_parentPolyline,
                              desc });
        }

        // PairHit 保存一次两两检测的最小结果，随后按轨道与时间聚类。
        struct PairHit {
            /// @brief 第一项索引。
            /// @note 始终小于 j，来自无序对遍历的外层索引。
            size_t i{ 0 };
            /// @brief 第二项索引。
            /// @note 始终大于 i，避免相同对象对重复出现。
            size_t j{ 0 };
            /// @brief 重叠轨道。
            /// @note 对跨轨 Flick 使用实际交叉或命中轨道。
            int track{ 0 };
            /// @brief 重叠时间。
            /// @note 用于相邻命中按配置时间窗合并。
            double time{ 0.0 };
        };

        // PointProbe 表示可与另一对象端点或跨轨身体比较的离散判定点。
        struct PointProbe {
            /// @brief 检测时间。
            /// @note 对 Hold 可表示起点或终点，对其他类型表示发生时刻。
            double time{ 0.0 };
            /// @brief 检测轨道。
            /// @note 对 Flick 终点探针包含 dtrack 偏移。
            int track{ 0 };
            /// @brief 是否检测 Flick 跨轨身体。
            /// @note Flick 起点为 false，避免自身端点语义重复覆盖。
            bool testsFlickBody{ false };
        };

        // 用户配置以毫秒存储，扫描统一换算为非负秒数。
        const double windowSeconds =
            static_cast<double>(std::max(0.0f,
                                         Config::AppConfig::instance()
                                             .getEditorSettings()
                                             .overlapTimeWindowMs)) *
            0.001;
        // 极小容差用于区分严格区间内部与浮点边界接触。
        constexpr double timeEpsilon = 1e-7;

        // 同一折线容器中的子物件是设计连接关系，不报告互相重叠。
        auto samePolylineParent = [](const CheckItem& a, const CheckItem& b) {
            return a.parentPolyline != entt::null &&
                   a.parentPolyline == b.parentPolyline;
        };

        // Flick 横跨轨道可能为负方向，先规范化最小端点。
        auto flickMinTrack = [](const CheckItem& item) {
            return std::min(item.track, item.track + item.dtrack);
        };
        // 最大端点与最小端点共同定义闭合轨道范围。
        auto flickMaxTrack = [](const CheckItem& item) {
            return std::max(item.track, item.track + item.dtrack);
        };

        // 将音符类型映射为有限个需要比较的端点探针。
        auto collectPoints = [&](const CheckItem& item) {
            std::vector<PointProbe> points;
            if ( item.type == ::MMM::NoteType::NOTE ) {
                // 点击音符只有起点，且可检测 Flick 身体覆盖。
                points.push_back({ item.startTime, item.track, true });
            } else if ( item.type == ::MMM::NoteType::HOLD ) {
                // Hold 起点总是参与端点比较。
                points.push_back({ item.startTime, item.track, true });
                if ( item.endTime > item.startTime + timeEpsilon ) {
                    // 只有非零长度 Hold 才追加独立终点。
                    points.push_back({ item.endTime, item.track, true });
                }
            } else if ( item.type == ::MMM::NoteType::FLICK ) {
                // Flick 起点只参与普通端点比较，不反向检测自身身体。
                points.push_back({ item.startTime, item.track, false });
                // Flick 终点轨道可检测另一 Flick 的跨轨身体。
                points.push_back(
                    { item.startTime, item.track + item.dtrack, true });
            }
            // 每个音符最多生成两个探针，向量规模保持常量级。
            return points;
        };

        // 两两判定返回是否命中，并通过引用输出最有代表性的轨道和时间。
        auto isOverlapPair = [&](const CheckItem& a,
                                 const CheckItem& b,
                                 int&             overlapTrack,
                                 double&          overlapTime) {
            // 判定顺序从端点到类型专用身体规则，命中后立即返回。
            // 同一折线内的结构性相邻子音符不作为错误报告。
            if ( samePolylineParent(a, b) ) return false;

            // 默认输出保证任何后续特殊命中都具有合法初值。
            overlapTrack = a.track;
            overlapTime  = std::min(a.startTime, b.startTime);

            // 首先检测同轨端点在配置时间窗内的接近关系。
            auto aPoints = collectPoints(a);
            auto bPoints = collectPoints(b);
            for ( const auto& aPoint : aPoints ) {
                for ( const auto& bPoint : bPoints ) {
                    // 端点轨道不同则不可能直接重合。
                    if ( aPoint.track != bPoint.track ) continue;
                    if ( std::abs(aPoint.time - bPoint.time) >
                         windowSeconds + timeEpsilon )
                        // 时间差超过容差窗时继续比较其他端点组合。
                        continue;

                    // 命中使用共同轨道和两个端点中更早的时间。
                    overlapTrack = aPoint.track;
                    overlapTime  = std::min(aPoint.time, bPoint.time);
                    return true;
                }
            }

            // 判断某一探针是否位于同时间 Flick 的跨轨身体范围内。
            auto flickBodyContainsPoint = [&](const CheckItem&  flick,
                                              const PointProbe& point) {
                // 只有允许身体检测的探针和非零跨度 Flick 才继续。
                if ( !point.testsFlickBody ||
                     flick.type != ::MMM::NoteType::FLICK || flick.dtrack == 0 )
                    return false;
                // 探针时间必须落在配置的同拍窗口内。
                if ( std::abs(point.time - flick.startTime) >
                     windowSeconds + timeEpsilon )
                    return false;
                // 轨道范围包含两端，使正向和负向 Flick 语义一致。
                return point.track >= flickMinTrack(flick) &&
                       point.track <= flickMaxTrack(flick);
            };

            // 使用 b 的全部探针检测 a 的 Flick 身体。
            for ( const auto& point : bPoints ) {
                if ( flickBodyContainsPoint(a, point) ) {
                    overlapTrack = point.track;
                    overlapTime  = point.time;
                    return true;
                }
            }
            // 对称检测 a 的探针是否落入 b 的 Flick 身体。
            for ( const auto& point : aPoints ) {
                if ( flickBodyContainsPoint(b, point) ) {
                    overlapTrack = point.track;
                    overlapTime  = point.time;
                    return true;
                }
            }

            // 两个普通点击音符按同轨和严格时间窗判定。
            if ( a.type == ::MMM::NoteType::NOTE &&
                 b.type == ::MMM::NoteType::NOTE ) {
                return a.track == b.track &&
                       std::abs(a.startTime - b.startTime) < windowSeconds;
            }

            // 两个 Hold 在同轨时检查开放区间是否具有正长度交集。
            if ( a.type == ::MMM::NoteType::HOLD &&
                 b.type == ::MMM::NoteType::HOLD && a.track == b.track ) {
                double start = std::max(a.startTime, b.startTime);
                // 交集终点取两个 Hold 结束时刻的较早值。
                double end  = std::min(a.endTime, b.endTime);
                overlapTime = start;
                return end > start + timeEpsilon;
            }

            // 两条非零跨度 Flick 在相近时间检查轨道区间交叉。
            if ( a.type == ::MMM::NoteType::FLICK &&
                 b.type == ::MMM::NoteType::FLICK && a.dtrack != 0 &&
                 b.dtrack != 0 &&
                 std::abs(a.startTime - b.startTime) <=
                     windowSeconds + timeEpsilon ) {
                // 交叉区间起点取两个最小轨道的较大值。
                int minTrack = std::max(flickMinTrack(a), flickMinTrack(b));
                // 交叉区间终点取两个最大轨道的较小值。
                int maxTrack = std::min(flickMaxTrack(a), flickMaxTrack(b));
                overlapTrack = minTrack;
                return maxTrack > minTrack;
            }

            // 判断离散探针是否严格位于 Hold 身体内部。
            auto holdBodyContainsPoint = [&](const CheckItem&  hold,
                                             const PointProbe& point) {
                // 排除起止边界，端点接触已由前面的时间窗规则处理。
                return hold.type == ::MMM::NoteType::HOLD &&
                       point.track == hold.track &&
                       point.time > hold.startTime + timeEpsilon &&
                       point.time < hold.endTime - timeEpsilon;
            };

            // 检查 b 的端点是否进入 a 的 Hold 身体。
            for ( const auto& point : bPoints ) {
                if ( holdBodyContainsPoint(a, point) ) {
                    overlapTrack = point.track;
                    overlapTime  = point.time;
                    return true;
                }
            }
            // 对称检查 a 的端点是否进入 b 的 Hold 身体。
            for ( const auto& point : aPoints ) {
                if ( holdBodyContainsPoint(b, point) ) {
                    overlapTrack = point.track;
                    overlapTime  = point.time;
                    return true;
                }
            }

            // 判断 Flick 在 Hold 持续区间内部横跨其所在轨道。
            auto holdFlickCrosses = [&](const CheckItem& hold,
                                        const CheckItem& flick) {
                // 只接受 Hold 与非零跨度 Flick 的类型组合。
                if ( hold.type != ::MMM::NoteType::HOLD ||
                     flick.type != ::MMM::NoteType::FLICK || flick.dtrack == 0 )
                    return false;
                // Flick 必须严格发生在 Hold 身体内部，端点接触留给前述规则。
                if ( flick.startTime <= hold.startTime + timeEpsilon ||
                     flick.startTime >= hold.endTime - timeEpsilon )
                    return false;
                // Hold 固定轨道落入 Flick 闭合跨轨范围即构成相交。
                return hold.track >= flickMinTrack(flick) &&
                       hold.track <= flickMaxTrack(flick);
            };

            // 先按 a 为 Hold、b 为 Flick 的方向检测。
            if ( holdFlickCrosses(a, b) ) {
                overlapTrack = a.track;
                overlapTime  = b.startTime;
                return true;
            }
            // 再检测对称类型排列。
            if ( holdFlickCrosses(b, a) ) {
                overlapTrack = b.track;
                overlapTime  = a.startTime;
                return true;
            }

            // 所有端点、身体与区间规则均未命中时不是重叠对。
            return false;
        };

        // 原始配对命中保留索引，便于后续合并同一时间窗内的多对象冲突。
        std::vector<PairHit> pairHits;

        // 每个无序对象对只比较一次，避免自比较和重复结果。
        for ( size_t i = 0; i < items.size(); ++i ) {
            for ( size_t j = i + 1; j < items.size(); ++j ) {
                int    overlapTrack = 0;
                double overlapTime  = 0.0;
                // 判定函数同时返回用于排序和跳转的代表位置。
                if ( !isOverlapPair(
                         items[i], items[j], overlapTrack, overlapTime) )
                    continue;

                // 命中按值追加，实体组件不再参与后续阶段。
                pairHits.push_back({ i, j, overlapTrack, overlapTime });
            }
        }

        // 先按轨道、再按时间排序，使相邻窗口命中可以线性聚类。
        std::sort(pairHits.begin(),
                  pairHits.end(),
                  [](const PairHit& a, const PairHit& b) {
                      if ( a.track != b.track ) return a.track < b.track;
                      // 同轨命中按最早时间升序排列。
                      return a.time < b.time;
                  });

        // 将同轨且时间接近的配对命中折叠为单条用户结果。
        for ( size_t i = 0; i < pairHits.size(); ) {
            size_t j = i + 1;
            // 聚类锚定首条时间，避免链式相邻把窗口无限扩张。
            while ( j < pairHits.size() &&
                    pairHits[j].track == pairHits[i].track &&
                    pairHits[j].time <=
                        pairHits[i].time + windowSeconds + timeEpsilon ) {
                ++j;
            }

            // 一个聚类最多涉及每个 PairHit 的两个对象索引。
            std::vector<size_t> indices;
            indices.reserve((j - i) * 2);
            auto addUniqueIndex = [&](size_t index) {
                // 小型聚类使用线性去重，避免额外哈希分配。
                if ( std::find(indices.begin(), indices.end(), index) ==
                     indices.end() ) {
                    indices.push_back(index);
                }
            };

            // 跳转时间取聚类中最早命中位置。
            double minTime = pairHits[i].time;
            for ( size_t k = i; k < j; ++k ) {
                // 同时收集每对两端，形成真实参与对象集合。
                addUniqueIndex(pairHits[k].i);
                addUniqueIndex(pairHits[k].j);
                minTime = std::min(minTime, pairHits[k].time);
            }

            std::string desc1;
            std::string desc2;
            if ( indices.size() == 2 && j == i + 1 ) {
                // 单独一对保留两枚音符的具体类型描述。
                desc1 = items[pairHits[i].i].desc;
                desc2 = items[pairHits[i].j].desc;
            } else {
                // 多对聚类改用数量摘要，避免结果行列出过长对象清单。
                desc1 = TR_FMT("ui.tools.multiple_objects", indices.size());
                desc2 = TR("ui.tools.each_other").data();
            }

            // 聚类结果不保留实体索引，只保存表格与跳转所需值。
            m_results.push_back({ true,
                                  minTime,
                                  static_cast<uint32_t>(pairHits[i].track),
                                  desc1,
                                  desc2 });
            // 跳过本聚类已消费的全部 PairHit。
            i = j;
        }
    }

    /// @brief 渲染重叠检测结果窗口。
    /// @warning UI 热路径：每帧执行；仅在窗口打开时绘制结果表。
    /// @note 样式值按当前 DPI 取整，保持像素边界清晰。
    /// @warning 切换活动谱面不会自动重扫，结果刷新必须由用户显式触发。
    void renderWindow()
    {
        // 窗口关闭时不读取配置、不锁 Session，也不绘制结果。
        if ( !m_showWindow ) return;

        // 引用编辑器外观配置，避免复制完整设置对象。
        auto& editorSettings =
            Config::AppConfig::instance().getEditorSettings();
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        // 圆角与间距转换到物理像素后取整，减少亚像素抖动。
        float windowRound =
            std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
        float frameRound =
            std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
        ImVec2 itemSpacing = {
            std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
            std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
        };

        // 六个样式变量只覆盖重叠检查窗口，并在末尾一次性恢复。
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

        // 首次使用时提供合理尺寸，之后尊重用户调整。
        ImGui::SetNextWindowSize(ImVec2(550.0f * dpiScale, 450.0f * dpiScale),
                                 ImGuiCond_FirstUseEver);

        // 标题字体只应用于 Begin 创建的窗口标题区域。
        auto&   skinMgr   = Config::SkinManager::instance();
        ImFont* titleFont = skinMgr.getFont("title");
        if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

        // 固定 ### ID 保证语言切换时窗口状态不丢失。
        std::string windowTitle =
            TR("ui.tools.overlap_check_title").toString() +
            "###OverlapCheckWindow";
        const bool wasOpenBeforeBegin = m_showWindow;
        // Begin 返回内容可见性；无论返回值如何都必须 End。
        bool opened = ImGui::Begin(windowTitle.c_str(), &m_showWindow);
        // 统一处理窗口关闭按钮的音效与状态反馈。
        FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, &m_showWindow);

        // 仅在此前成功压入标题字体时恢复字体栈。
        if ( titleFont ) ImGui::PopFont();

        if ( opened ) {
            // 结果绘制前验证活动会话与谱面仍存在。
            auto& engine = Logic::EditorEngine::instance();
            // 锁覆盖会话获取和当前谱面指针读取。
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();
            if ( !session ) {
                // 会话关闭后保留窗口并显示明确错误状态。
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "%s",
                                   TR("ui.tools.no_active_session").data());
            } else {
                // 谱面指针只在 Session 锁保护期间读取。
                auto beatmap = session->getContext().currentBeatmap;
                if ( !beatmap ) {
                    // Session 存在但无活动谱面时不展示旧扫描结果。
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                       "%s",
                                       TR("ui.tools.no_active_beatmap").data());
                } else {
                    // 有效谱面时展示扫描入口或缓存结果。
                    renderResults(dpiScale);
                }
            }
        }
        // 与 Begin 无条件配对。
        ImGui::End();

        // 一次性恢复本函数压入的六个样式变量。
        ImGui::PopStyleVar(6);
    }

    /// @brief 渲染重叠检测结果内容。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：每帧执行；仅在窗口打开时绘制结果表。
    /// @note 扫描只由按钮点击调用，缓存结果通过 clipper 按可见行绘制。
    /// @warning 重扫按钮会同步执行完整扫描，结果量大时可能产生可见停顿。
    void renderResults(float dpiScale)
    {
        if ( !m_hasScan ) {
            // 首次打开只显示全宽扫描按钮，不自动执行高成本检测。
            if ( ::MMM::UI::FeedbackButton(TR("ui.tools.scan_now").data(),
                                           ImVec2(-1.0f, 40.0f * dpiScale)) ) {
                performScan();
            }
            // 未扫描状态没有摘要或结果表。
            return;
        }

        // 已扫描后提供显式重扫入口，允许用户在谱面修改后刷新结果。
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.tools.rescan").data(),
                 ImVec2(120.0f * dpiScale, 30.0f * dpiScale)) ) {
            performScan();
        }

        // 摘要与重扫按钮同行显示。
        ImGui::SameLine();
        // 计数从缓存结果计算，不访问谱面 Registry。
        int definiteCount  = 0;
        int suspectedCount = 0;
        for ( const auto& result : m_results ) {
            if ( result.isDefinite )
                // 确定命中与疑似命中分别统计。
                definiteCount++;
            else
                suspectedCount++;
        }

        // 本地化摘要同时展示两类结果数量。
        std::string summaryStr =
            TR_FMT("ui.tools.scan_summary", definiteCount, suspectedCount);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(summaryStr.c_str());

        // 分隔顶部扫描控制区与结果正文。
        ImGui::Separator();
        ImGui::Spacing();

        if ( m_results.empty() ) {
            // 空结果以绿色成功状态呈现，并省略空表格。
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                               "%s",
                               TR("ui.tools.no_overlaps").data());
            return;
        }

        // 结果表支持纵向滚动、交替行背景、边框与用户调列宽。
        ImGuiTableFlags tableFlags =
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable;
        // 滚动条样式作用域只影响结果表。
        Utils::VerticalScrollbarStyleScope scrollbarStyle(dpiScale);
        if ( !ImGui::BeginTable(
                 "OverlapResultsTable", 5, tableFlags, ImVec2(0.0f, -1.0f)) )
            return;

        // 类型、时间、轨道和跳转列固定宽度，详情列吸收剩余空间。
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

        // 表头使用已本地化列名。
        ImGui::TableHeadersRow();

        // clipper 将大结果集限制为当前可见行绘制。
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_results.size()));
        while ( clipper.Step() ) {
            // DisplayStart 与 DisplayEnd 已由 ImGui 裁剪到合法可见范围。
            for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i ) {
                renderResultRow(i);
            }
        }
        // 与成功 BeginTable 配对。
        ImGui::EndTable();
    }

    /// @brief 渲染单条重叠检测结果。
    /// @param index 结果索引。
    /// @warning UI 热路径：仅在结果表可见行中执行。
    /// @pre index 必须来自针对 m_results 大小初始化的 ImGuiListClipper。
    /// @note 该函数不修改缓存结果，只可能投递画布跳转命令。
    void renderResultRow(int index)
    {
        // 安全前置条件由 clipper 保证，按 size_t 访问缓存结果。
        const auto& result = m_results[static_cast<std::size_t>(index)];
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        if ( result.isDefinite ) {
            // 确定重叠使用红色高优先级提示。
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "%s",
                               TR("ui.tools.definite").data());
        } else {
            // 疑似重叠使用橙色，区别于确定错误。
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "%s",
                               TR("ui.tools.suspected").data());
        }

        // 时间列使用画布统一格式化规则。
        ImGui::TableNextColumn();
        const auto timeText =
            MMM::UI::Utils::formatCanvasTime(result.timestamp);
        ImGui::TextUnformatted(timeText.c_str());

        // 轨道内部从零开始，显示给用户时加一。
        ImGui::TableNextColumn();
        ImGui::Text("%d", result.track + 1);

        // 详情文本由两端描述通过翻译模板组合。
        ImGui::TableNextColumn();
        std::string detailStr = TR_FMT(
            "ui.tools.overlap_detail", result.note1Desc, result.note2Desc);
        ImGui::TextUnformatted(detailStr.c_str());

        // 跳转按钮使用透明底色，仅在悬停时显示蓝色反馈。
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.4f, 0.7f, 1.0f, 0.3f));
        if ( ::MMM::UI::FeedbackButton(
                 fmt::format("{}##{}", ICON_MMM_SEARCH, index).c_str(),
                 ImVec2(-1, 0)) ) {
            // 跳转时补偿当前有效视觉偏移，使画布显示落在实际判定点。
            float visualOffset = Config::AppConfig::instance()
                                     .getVisualConfig()
                                     .getEffectiveVisualOffset();
            MenuUtil::dispatchCommand(
                // 命令层负责更新播放位置和相关视图同步。
                Logic::CmdSeek{ result.timestamp - visualOffset });
        }
        // 恢复按钮和悬停颜色，不影响后续表格行。
        ImGui::PopStyleColor(2);
        if ( ImGui::IsItemHovered() ) {
            // Tooltip 展示未扣除视觉偏移的谱面目标时间。
            const auto targetTimeText =
                MMM::UI::Utils::formatCanvasTime(result.timestamp);
            ImGui::SetTooltip(
                "%s", TR_FMT("canvas.preview.jump_to", targetTimeText).c_str());
        }
    }

    /// @brief 是否显示重叠检测窗口。
    /// @note execute 与窗口关闭按钮共同维护该值。
    /// @warning 仅在 UI 线程读写。
    bool m_showWindow = false;

    /// @brief 当前重叠检测结果是否已生成。
    /// @note 首次用户扫描后保持 true，直到处理器销毁。
    /// @warning 不代表结果仍与当前谱面版本同步。
    bool m_hasScan = false;

    /// @brief 当前缓存的重叠检测结果。
    /// @note 只在 performScan 中整体重建，绘制阶段只读。
    /// @warning 缓存不持有实体 ID，无法在对象删除后自动失效。
    std::vector<OverlapResult> m_results;
};
}  // namespace

/// @brief 创建切换重叠检测窗口动作处理器。
/// @return 独占所有权的重叠检测窗口处理器。
/// @warning 处理器必须在 EditorEngine 和 ImGui 所属 UI 线程使用。
/// @note 新处理器初始不扫描谱面，等待用户明确点击扫描按钮。
std::unique_ptr<IMainMenuItemActionHandler>
createToggleOverlapCheckWindowAction()
{
    return std::make_unique<ToggleOverlapCheckWindowAction>();
}

}  // namespace MMM::UI
