#include "canvas/ComposeEditProgress.h"
#include <cstdint>

/// 测试直接构造逻辑线程已经发布的轻量快照，不建立窗口或谱面 ECS。
/// 这样可以分别控制步骤基线、当前几何与最后一次结束事务。
/// 鼠标按下或拖动预览不会生成结束事务，不能独自满足任何步骤。
/// 根实体在 Shift 续接时可以替换，因此场景同时覆盖旧来源和新令牌。
/// 编辑动作的类型、部件、索引和几何差异都应参与成功判定。
/// 失败场景尽量只改动其中一个条件，以定位误报来源。
/// @warning 测试数据只模仿快照字段，不绕过正式逻辑线程执行编辑命令。
/// @brief 验证编辑演练只由目标物件的正式释放结果推进。
/// @note 预览中的几何变化不伴随新的结束事务，因此不能完成步骤。
/// @note 续接会替换根实体，但沿用原创建令牌和练习槽位。
/// @note 每一种编辑形态使用对应部件与字段，避免普通整体移动冒充局部编辑。
/// @return 首个不满足操作契约的场景编号，全部通过时为零。
int main()
{
    using MMM::Canvas::ComposeEditStep;
    using MMM::Canvas::composeEditSatisfied;
    using MMM::Common::Render::HoverPart;
    using Snapshot = MMM::Common::Render::RenderSnapshot;
    using State    = Snapshot::WalkthroughPracticeNoteState;
    using Event    = Snapshot::WalkthroughEditEvent;

    // 本轮创建令牌是识别教程对象的稳定标识。
    // entt 句柄是事务起笔处的来源标识，不能单独跨替换复用。
    // 先用普通 Note 建立最小有效基线，再逐类替换几何。
    State before{};
    before.token          = 42;
    before.entity         = static_cast<entt::entity>(11);
    before.alive          = true;
    before.root.type      = MMM::NoteType::NOTE;
    before.root.track     = 2;
    before.root.timestamp = 1.0;
    State after           = before;
    after.root.track      = 3;
    Event event{};
    event.revision     = 6;
    event.sourceEntity = before.entity;
    event.kind         = Event::Kind::Drag;
    event.part         = HoverPart::Head;

    // 新拖动和真实根位置变化共同构成单 Note 完成条件。
    // 相同几何或旧序号都必须拒绝，鼠标释放本身不是编辑结果。
    // 第一次调用验证有效拖动；第二次验证事件已在步骤开始前发生。
    // 第三次复用事件但不改变几何，证明空拖动不能完成步骤。
    if ( !composeEditSatisfied(
             ComposeEditStep::MoveNote, before, after, event, 5) ||
         composeEditSatisfied(
             ComposeEditStep::MoveNote, before, after, event, 6) ||
         composeEditSatisfied(
             ComposeEditStep::MoveNote, before, before, event, 5) )
        return 1;

    // 尾部和箭头采用专有命中部位；拖动整个物件不算改尺寸。
    // 先从 Hold 的正式时长变化验证尾部拉伸。
    // 随后只把命中部件改为 Head，模拟整体移动对本步骤的误报。
    // Flick 使用有符号轨差，时间和起轨保持原值。
    before.root.type     = MMM::NoteType::HOLD;
    before.root.duration = 0.5;
    after                = before;
    after.root.duration  = 0.75;
    event.part           = HoverPart::HoldEnd;
    if ( !composeEditSatisfied(
             ComposeEditStep::ResizeHold, before, after, event, 5) )
        return 2;
    event.part = HoverPart::Head;
    if ( composeEditSatisfied(
             ComposeEditStep::ResizeHold, before, after, event, 5) )
        return 3;
    before.root.type   = MMM::NoteType::FLICK;
    before.root.dtrack = 1;
    after              = before;
    after.root.dtrack  = 2;
    event.part         = HoverPart::FlickArrow;
    if ( !composeEditSatisfied(
             ComposeEditStep::ResizeFlick, before, after, event, 5) )
        return 4;

    // 中间段分别按轨道与时间变化验收，索引必须和本次抓取一致。
    // 保留五段数量证明这两步并非提前发生退化合并。
    // 教程生成的路线第三段是 Hold，第四段是 Flick。
    // Hold 横移使用 track，Flick 纵移使用 timestamp。
    // 两段均以 HoldBody 命中，但事件中的 subIndex 必须区分它们。
    // before 和 after 的数组是根对象的结构快照，不是独立 ECS 子实体。
    before.root.type             = MMM::NoteType::POLYLINE;
    before.subNoteCount          = 5;
    before.capturedSubNoteCount  = 5;
    before.subNotes[2].type      = MMM::NoteType::HOLD;
    before.subNotes[2].track     = 3;
    before.subNotes[3].type      = MMM::NoteType::FLICK;
    before.subNotes[3].timestamp = 2.0;
    after                        = before;
    after.subNotes[2].track      = 4;
    event.part                   = HoverPart::HoldBody;
    event.subIndex               = 2;
    if ( !composeEditSatisfied(
             ComposeEditStep::MoveSubHold, before, after, event, 5) )
        return 5;
    after                       = before;
    after.subNotes[3].timestamp = 2.25;
    event.subIndex              = 3;
    if ( !composeEditSatisfied(
             ComposeEditStep::MoveSubFlick, before, after, event, 5) )
        return 6;

    // 合并必须来自内部子段拖动，且正式子段数减少。
    // 退化清理可能继续让折线降级为单个 Hold，不能要求结果仍为 Polyline。
    // 结构变化由发布后的 subNoteCount 证明，不读取拖动中预览列表。
    // 最终根句柄仍可保持，但根类型变化不影响教程身份。
    after              = before;
    after.subNoteCount = 0;
    after.root.type    = MMM::NoteType::HOLD;
    event.subIndex     = 3;
    if ( !composeEditSatisfied(
             ComposeEditStep::MergeSubNote, before, after, event, 5) )
        return 7;

    // Shift 续接替换原根实体时，来源实体仍取旧句柄，令牌保持不变。
    // 身份属于别的练习物件时，即使新对象也是折线也不能抵扣。
    // 演练在合并后改为从独立 Hold/Flick 扩展，不要求旧五段折线存活。
    // 新根实体号不同是正常结果，直接比较前后句柄会误拒绝有效编辑。
    // 变更 after.token 后必须失败，防止无关折线占据练习槽。
    before.root.type    = MMM::NoteType::HOLD;
    before.subNoteCount = 0;
    after               = before;
    after.entity        = static_cast<entt::entity>(12);
    after.root.type     = MMM::NoteType::POLYLINE;
    after.subNoteCount  = 2;
    event.kind          = Event::Kind::Brush;
    event.part          = HoverPart::None;
    if ( !composeEditSatisfied(
             ComposeEditStep::ExtendNote, before, after, event, 5) )
        return 8;
    after.token = 99;
    if ( composeEditSatisfied(
             ComposeEditStep::ExtendNote, before, after, event, 5) )
        return 9;

    // 原折线尾部可新增子段，也可保留数量但改变末段以完成回写。
    // 此时基线表示刚扩展出的折线，而不是被合并步骤改写的旧根。
    // 增加数量属于续写；保持数量但改时长属于回写。
    // 两种情况都应接受 Brush 事务，并允许根再次替换。
    before.root.type            = MMM::NoteType::POLYLINE;
    before.subNoteCount         = 3;
    before.capturedSubNoteCount = 3;
    before.subNotes[2].type     = MMM::NoteType::HOLD;
    before.subNotes[2].duration = 0.5;
    after                       = before;
    after.entity                = static_cast<entt::entity>(13);
    after.subNoteCount          = 4;
    if ( !composeEditSatisfied(
             ComposeEditStep::ResumePolyline, before, after, event, 5) )
        return 10;
    after.subNoteCount         = 3;
    after.subNotes[2].duration = 0.75;
    if ( !composeEditSatisfied(
             ComposeEditStep::ResumePolyline, before, after, event, 5) )
        return 11;
    // 只有拖动预览或无变化的替换，都不能借用一次 Brush 事务完成练习。
    // 这一步确保重复点击 Shift 但没有形成新几何时教程仍停留。
    // 未完成状态会继续显示尾部入口，用户可重新尝试。
    // after 回到 before 时，根实体也回到原句柄；仅事件类型不应通过。
    // 退化清理若没有留下新段或修改末段，同样不会被视为续写。
    // 固定数组的末段比较与实体替换无关，因此这一判断稳定可复现。
    // 测试保持与 UI 的步骤顺序一致，最后一个场景覆盖空续接。
    // 返回值用于 CTest 报告首个失败阶段，避免依赖外部日志配置。
    after = before;
    if ( composeEditSatisfied(
             ComposeEditStep::ResumePolyline, before, after, event, 5) )
        return 12;
    return 0;
}
