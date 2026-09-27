/// @file Basic2DCanvas_WalkthroughEditing.cpp
/// @brief 为创作教程中的局部编辑操作定位目标并验证提交后的结果。
///
/// 画布只能读取逻辑线程已经发布的渲染快照，不能从鼠标按键推断编辑成功。
/// 固定四个练习槽位追踪教程中创建的实体；画笔续接会替换根实体，
/// 因此创建 Action 会把原槽位令牌继承给新根，避免引导丢失目标。
/// 每个步骤首次进入时保存几何基线和编辑事务序号，后续只比较已提交结果。
/// 瞬时拖动预览、错误工具和其它实体上的动作均不推进步骤。
///
/// 热路径只查询固定步骤表、四个槽位及快照已有命中框，
/// 不读取 ECS，也不触发额外的跨线程等待或全谱扫描。
#include "canvas/Basic2DCanvas.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/ComposeEditProgress.h"
#include "imgui.h"
#include "ui/UIManager.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include <array>
#include <string_view>

namespace MMM::Canvas
{
/// @brief 检查编辑教程目标是否已经由逻辑线程正式提交。
/// @param sourceManager 提供当前步骤与聚光灯状态。
/// @param snapshot 包含当前物件几何、命中框及最新编辑事务。
/// @param canvasScreenPosition 画布屏幕位置，用于转换命中框坐标。
/// @param canvasSize 画布尺寸，用于目标暂不可见时提示导航。
/// @return 当前正在处理编辑步骤时返回 true，供其它绘制步骤跳过。
/// @warning UI 热路径：每帧仅检查固定教程目标和快照已剔除的命中框。
bool Basic2DCanvas::updateComposeEditWalkthrough(
    UI::UIManager*                        sourceManager,
    const Common::Render::RenderSnapshot& snapshot,
    const ImVec2& canvasScreenPosition, const ImVec2& canvasSize)
{
    if ( !sourceManager ) return false;
    auto& spotlight = sourceManager->walkthroughSpotlight();
    /// @brief 编辑步骤只观察已创建的四个练习物件与一次正式释放事务。
    /// @note 多目标扩展允许从 Hold 或 Flick 任一尾部起笔，其余步骤固定槽位。
    /// @note slot 对应创建步骤登记的身份，不靠拍位重新搜索目标。
    /// @note subIndex 的 -1 表示独立尾部，-2 表示当前折线的末段。
    /// @note 指定工具可防止把不可操作的部件突出给用户。
    struct ComposeEditGuideTarget {
        std::string_view          id;
        ComposeEditStep           step;
        int                       slot;
        Common::Render::HoverPart part;
        int                       subIndex;
        Logic::EditTool           tool;
    };
    // 前六项使用 Move 工具；后续续接需要 Draw 工具和 Shift 手势。
    // 同名双目标表示 Hold/Flick 任一尾部均可选择，并非两次操作。
    // 续写也可能从任意新折线的尾部开始，需要运行时识别实际根类型。
    // 表只保存常量索引和部件；真实物件身份始终来自发布后的快照。
    // 因此谱面替换时不会在表中留下悬空的 ECS 指针或旧坐标。
    constexpr std::array editTargets{
        // 单键仅以 Head 拖动改变位置，不能用右键擦除或画笔落点代替。
        // 槽位零来自教程第一次创建的 Note，后续空间移动不改变身份。
        ComposeEditGuideTarget{ "compose.canvas.move-note",
                                ComposeEditStep::MoveNote,
                                0,
                                Common::Render::HoverPart::Head,
                                -1,
                                Logic::EditTool::Move },
        // Hold 尾部独立于身体命中框，拉伸只改变时长而不移动头部。
        // 缩短和延长都合法，完成条件比较正式的 duration。
        ComposeEditGuideTarget{ "compose.canvas.resize-hold",
                                ComposeEditStep::ResizeHold,
                                1,
                                Common::Render::HoverPart::HoldEnd,
                                -1,
                                Logic::EditTool::Move },
        // Flick 箭头指向终点轨，拖动它改变带符号的 dtrack。
        // 根头部仍由原创建令牌定位，横移整个 Flick 不算改距离。
        ComposeEditGuideTarget{ "compose.canvas.resize-flick",
                                ComposeEditStep::ResizeFlick,
                                2,
                                Common::Render::HoverPart::FlickArrow,
                                -1,
                                Logic::EditTool::Move },
        // 折线路线的第三子段是中间 Hold，横移会带着后缀同向移动。
        // 此步骤保持子段数量，避免误把结构合并提前算作位置练习。
        ComposeEditGuideTarget{ "compose.canvas.move-sub-hold",
                                ComposeEditStep::MoveSubHold,
                                3,
                                Common::Render::HoverPart::HoldBody,
                                2,
                                Logic::EditTool::Move },
        // 第四子段是中间 Flick，纵移会同时改变上一 Hold 的长度。
        // 命中框沿用 HoldBody，但索引必须准确匹配到该子段。
        ComposeEditGuideTarget{ "compose.canvas.move-sub-flick",
                                ComposeEditStep::MoveSubFlick,
                                3,
                                Common::Render::HoverPart::HoldBody,
                                3,
                                Logic::EditTool::Move },
        // 再次拖动第四段至相邻 Hold 起点，会触发退化清理。
        // 清理后的根可能降级为独立 Hold，验收仍以原槽位为准。
        ComposeEditGuideTarget{ "compose.canvas.merge-sub-note",
                                ComposeEditStep::MergeSubNote,
                                3,
                                Common::Render::HoverPart::HoldBody,
                                3,
                                Logic::EditTool::Move },
        // 独立 Hold 可以在尾部起笔，改变轨道后生成新的折线根。
        // 旧 Hold 的创建身份会通过 Action 传给这个新根。
        ComposeEditGuideTarget{ "compose.canvas.extend-note",
                                ComposeEditStep::ExtendNote,
                                1,
                                Common::Render::HoverPart::HoldEnd,
                                -1,
                                Logic::EditTool::Draw },
        // 独立 Flick 也可从箭头终点起笔，沿时间加上 Hold 段。
        // 两个扩展候选共享步骤 ID，任选其一即完成。
        ComposeEditGuideTarget{ "compose.canvas.extend-note",
                                ComposeEditStep::ExtendNote,
                                2,
                                Common::Render::HoverPart::FlickArrow,
                                -1,
                                Logic::EditTool::Draw },
        // 扩展 Hold 后的新折线保留在原 Hold 槽位，而非旧折线槽位。
        // 末段类型稍后由快照确认，此处只标记动态末段索引。
        ComposeEditGuideTarget{ "compose.canvas.resume-polyline",
                                ComposeEditStep::ResumePolyline,
                                1,
                                Common::Render::HoverPart::HoldEnd,
                                -2,
                                Logic::EditTool::Draw },
        // 扩展 Flick 后的新折线保留在原 Flick 槽位。
        // 两个候选中只会高亮实际成为折线且仍可见的尾部。
        ComposeEditGuideTarget{ "compose.canvas.resume-polyline",
                                ComposeEditStep::ResumePolyline,
                                2,
                                Common::Render::HoverPart::HoldEnd,
                                -2,
                                Logic::EditTool::Draw },
    };
    // 有合法候选命中框时不使用整画布提示，以免淹没另一候选尾部。
    bool reportedAlternative = false;
    for ( const auto& guide : editTargets ) {
        // 非当前步骤在此短路，避免每帧处理所有编辑目标的命中框。
        // 前置次序由 Spotlight 管理，这里只核对当前可执行动作。
        if ( !spotlight.awaitingTarget(guide.id) ) continue;
        // 独立绘制步骤的限制不能阻止从已有物件尾部开始续接。
        // 该标志仅控制教程放置目标，不替代逻辑线程的正式操作。
        if ( m_interaction ) m_interaction->setWalkthroughPlacement(false, 0);
        // 每一步只在进入时采样基线；拖动中的临时快照不得重新定义原位置。
        // 谱面实例切换后即使实体数值相同，也必须重取这一步的基线。
        // 基线和事务序号来自同一快照，避免跨线程发布出现混合状态。
        if ( m_walkthroughEditStepToken != spotlight.stepToken() ||
             m_walkthroughEditBeatmapInstanceId !=
                 snapshot.beatmapInstanceId ) {
            m_walkthroughEditStepToken         = spotlight.stepToken();
            m_walkthroughEditBeatmapInstanceId = snapshot.beatmapInstanceId;
            m_walkthroughEditFirstRevision =
                snapshot.walkthroughEditEvent.revision;
            m_walkthroughEditBaselines = snapshot.walkthroughPracticeNotes;
        }
        const auto& before = m_walkthroughEditBaselines[guide.slot];
        const auto& after  = snapshot.walkthroughPracticeNotes[guide.slot];
        // 空令牌表示创建步骤曾跳过；其它同类谱面物件不能冒充练习对象。
        // 只对创建时登记的谱面实例验收，切换标签后旧状态不再有效。
        // 纯函数检查几何与手势事件，视图仅负责报告步骤完成。
        if ( snapshot.hasBeatmap &&
             snapshot.beatmapInstanceId ==
                 m_walkthroughPracticeBeatmapInstanceId &&
             before.token == m_walkthroughPracticeTokens[guide.slot] &&
             composeEditSatisfied(guide.step,
                                  before,
                                  after,
                                  snapshot.walkthroughEditEvent,
                                  m_walkthroughEditFirstRevision) ) {
            spotlight.completeTarget(guide.id, true);
            return true;
        }
        // 合并练习可让原折线降级。续写因此改指刚由 Hold 或 Flick 扩展出的折线。
        // 两个候选只有一个会变成折线，不让未扩展者的整画布提示遮住真正尾部。
        // 独立旧物件仍可存在，但它没有折线尾部，不能作为本步起点。
        if ( guide.step == ComposeEditStep::ResumePolyline &&
             after.root.type != ::MMM::NoteType::POLYLINE )
            continue;
        // 目标来自正式物件的可交互部件，位置随滚动和局部编辑更新。
        // 末段索引在续写后变化，因此 -2 动态表示当前最后一个子段。
        // 命中框由渲染系统完成剔除，这里无需重算 SV 变换和拾取几何。
        const int subIndex = guide.subIndex == -2 && after.subNoteCount > 0
                                 ? static_cast<int>(after.subNoteCount - 1)
                                 : guide.subIndex;
        // 原独立物件的尾部类型不决定新折线末段；以实际末段挑选拾取部位。
        // Hold 对应 HoldEnd，Flick 对应 FlickArrow，错误部位无法起笔。
        // 若固定容量快照未包含尾段，则退回预设部件及导航提示。
        const auto part =
            guide.step == ComposeEditStep::ResumePolyline && subIndex >= 0 &&
                    subIndex < after.capturedSubNoteCount &&
                    after.subNotes[subIndex].type == ::MMM::NoteType::FLICK
                ? Common::Render::HoverPart::FlickArrow
                : guide.part;
        bool reported = false;
        // 工具不匹配时不提示一个目前无法执行的入口。
        // 相同根可以有多个可见命中框，只接受目标部件与目标子索引。
        if ( snapshot.currentTool == guide.tool && after.alive &&
             after.token == m_walkthroughPracticeTokens[guide.slot] ) {
            for ( const auto& box : snapshot.hitboxes ) {
                if ( box.entity != after.entity ||
                     box.kind != Logic::ChartObjectKind::PlayerNote ||
                     box.part != part || box.subIndex != subIndex )
                    continue;
                const ImVec2 minimum{ canvasScreenPosition.x + box.x,
                                      canvasScreenPosition.y + box.y };
                spotlight.reportTarget(guide.id,
                                       minimum,
                                       { minimum.x + box.w, minimum.y + box.h },
                                       ImGui::GetWindowViewport(),
                                       false);
                reported            = true;
                reportedAlternative = true;
            }
        }
        // 单目标在视野外时保留画布导航；双候选的兜底在循环后统一处理。
        // 画布矩形只负责提示滚动，不参与编辑成功判定。
        if ( !reported && guide.step != ComposeEditStep::ExtendNote &&
             guide.step != ComposeEditStep::ResumePolyline )
            spotlight.reportTarget(guide.id,
                                   canvasScreenPosition,
                                   { canvasScreenPosition.x + canvasSize.x,
                                     canvasScreenPosition.y + canvasSize.y },
                                   ImGui::GetWindowViewport(),
                                   false);
        // 同名双目标继续上报另一尾部；单目标到此结束本帧检查。
        // 用户任选一个合法尾部即可继续，不能被第一个候选提前截断。
        if ( guide.step != ComposeEditStep::ExtendNote &&
             guide.step != ComposeEditStep::ResumePolyline )
            return true;
    }
    if ( spotlight.awaitingTarget("compose.canvas.extend-note") ) {
        // Hold 与 Flick 可以一个在视野外、一个仍可见；优先显示可见尾部。
        // 两个都不可见时才提示在画布中滚动查找，不要求删除任一物件。
        if ( !reportedAlternative )
            spotlight.reportTarget("compose.canvas.extend-note",
                                   canvasScreenPosition,
                                   { canvasScreenPosition.x + canvasSize.x,
                                     canvasScreenPosition.y + canvasSize.y },
                                   ImGui::GetWindowViewport(),
                                   false);
        return true;
    }
    if ( spotlight.awaitingTarget("compose.canvas.resume-polyline") ) {
        // 刚扩展的根可能还未进入快照，也可能已滚动到画布外。
        // 只有两个候选都没有实际命中框时，才显示整画布导航提示。
        if ( !reportedAlternative )
            spotlight.reportTarget("compose.canvas.resume-polyline",
                                   canvasScreenPosition,
                                   { canvasScreenPosition.x + canvasSize.x,
                                     canvasScreenPosition.y + canvasSize.y },
                                   ImGui::GetWindowViewport(),
                                   false);
        return true;
    }
    return false;
}
}  // namespace MMM::Canvas
