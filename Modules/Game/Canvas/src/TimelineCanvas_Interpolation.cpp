#include "canvas/TimelineCanvas.h"

#include "canvas/TimingInterpolationPreview.h"

#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <mutex>

namespace MMM::Canvas
{
namespace
{
/// @brief 效果枚举顺序不作为泳道索引，显式映射四个区域。
/// @param effect 段落所控制的效果，不使用枚举的数值作为坐标。
/// @return BPM、Scroll、Jump、HS 对应零到三的专业模式泳道。
/// @note 普通模式不依据此索引分割画布，但仍使用它取得参数名称。
int interpolationLane(TimingEffect effect)
{
    switch ( effect ) {
    case TimingEffect::BPM: return 0;
    case TimingEffect::SCROLL: return 1;
    case TimingEffect::JUMP: return 2;
    case TimingEffect::HS: return 3;
    }
    return 0;
}

/// @brief 编辑窗口和画布共享同一套效果名称。
// 显示顺序与专业模式四泳道一致，但不要求 TimingEffect 底层枚举相邻。
// 曲线定义属于领域模型；这个表只为用户选中区域提供效果类型。
constexpr std::array INTERPOLATION_EFFECTS{ TimingEffect::BPM,
                                            TimingEffect::SCROLL,
                                            TimingEffect::JUMP,
                                            TimingEffect::HS };
/// @brief 显示名称不参与原生序列化，文件保存稳定的曲线枚举。
constexpr std::array INTERPOLATION_LABELS{ "BPM", "Scroll", "Jump (ms)", "HS" };
/// @brief 曲线选项与 TimingCurve 的声明顺序一一对应。
// 这些名称是界面文字，不能把它们作为文件或协议中的曲线标识。
// 未来翻译名称变动不会改变已保存的曲线编号或计算结果。
/// @note 新曲线只能追加稳定枚举，不能移动已发布的贝塞尔编号。
constexpr std::array CURVE_LABELS{
    "直线",       "二次渐入", "二次渐出", "平滑阶跃", "正弦渐变", "指数渐变",
    "三次贝塞尔", "三次幂",   "四次幂",   "五次幂",   "平方根",   "立方根",
    "对数",       "有理函数", "正弦渐入", "正弦渐出", "双曲正切", "自定义 f(t)"
};

/// @brief 判定模态工作副本是否与同效果时间点冲突。
/// @details 只使用打开窗口时捕获的值副本，逻辑提交会再次检查最新注册表。
/// 两段可以共享端点，不能交叠内部；普通点不能位于段落内部。
/// @param edit 编辑中的段落和所属原实体身份。
/// @param rows 打开编辑窗口时捕获的独立时间点副本。
/// @return 合法时为空，否则返回适合显示的固定说明。
/// @note 本地提示不能代替逻辑线程的最终校验。
/// @note 不读取活动 registry，因此联机新修改不会导致 UI 帧等待长锁。
/// @note 原实体被排除，编辑自己的范围不产生自冲突。
/// @note 起点和终点可以相接，交叠判断使用严格内部区间。
const char* interpolationValidationError(
    const Common::Render::TimingInterpolationElement&              edit,
    const std::vector<Common::Render::TimelineInteractiveElement>& rows)
{
    // 先检查段落自身，再比较已有对象范围，避免 NaN 进入区间比较。
    // 终点来自起点与时长，不依赖当前播放位置或可见区域。
    // 合法 BPM 端值经过公共模型校验，曲线过冲也因此被限制。
    if ( !std::isfinite(edit.time) || edit.time < 0.0 ||
         !isValidTimingInterpolation(
             edit.interpolation, edit.effect, edit.value) )
        return "范围、数值或密度不合法：起点须非负，时长至少 1 ms，BPM "
               "须在 0.1–10000 之间，最多 65536 次切分。";
    const double end = edit.time + edit.interpolation.m_duration;
    if ( !std::isfinite(end) ) return "段落终点超出可表示范围。";
    // 和逻辑入口使用相同的开区间规则，边界不会误报为重叠。
    for ( const auto& row : rows ) {
        entt::entity entity = entt::null;
        if ( edit.effect == TimingEffect::BPM ) entity = row.bpmEntity;
        if ( edit.effect == TimingEffect::SCROLL ) entity = row.scrollEntity;
        if ( edit.effect == TimingEffect::JUMP ) entity = row.jumpEntity;
        if ( edit.effect == TimingEffect::HS ) entity = row.hsEntity;
        // 同一个时间点表格结构覆盖四类效果，但每行只有一个实体。
        // 选取当前效果列的身份，其他泳道不会构成同参数冲突。
        if ( entity == entt::null || entity == edit.entity ) continue;
        // 普通点的终点等于自身，段落则使用时长扩展区间。
        // 边界点允许存在，便于下一段在共同端点接替参数。
        // 已有普通起点可由创建动作升级，所以这里只拒绝严格内部点。
        const double otherEnd =
            row.time +
            (row.interpolation ? row.interpolation->m_duration : 0.0);
        if ( row.interpolation
                 ? std::min(end, otherEnd) - std::max(edit.time, row.time) >
                       1e-9
                 : row.time > edit.time + 1e-9 && row.time < end - 1e-9 )
            return "范围内已有同类型时间点或插值段落，请调整范围后再保存。";
    }
    return nullptr;
}
}  // namespace

/// @brief 捕获独立编辑副本和范围校验快照。
/// @note 只在用户打开窗口时持有会话锁，常规 UI 帧不等待更新长锁。
/// @param segment 快照中的段落或 Shift 放置手势产生的新定义。
/// @pre 本画布已经消费一个有效谱面快照。
/// @details 记录路径和实例令牌，切换谱面或同路径重开均禁止旧副本提交。
/// @note 只捕获合法性检查所需的时间、类型、身份和段落定义。
/// @note 不存储注册表地址或 TimelineComponent 观察指针。
/// @note 元数据仍由更新命令保留，不将其复制到 UI 工作副本。
/// @warning 用户打开编辑器的低频路径，锁内复制范围后立即释放。
void TimelineCanvas::openInterpolationEditor(
    const Common::Render::TimingInterpolationElement& segment)
{
    // 打开编辑器须绑定一个真实谱面，不为欢迎页创建悬空工作副本。
    // 表格入口和画布入口均使用这个检查，因此不存在无来源的编辑提交。
    if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) return;
    m_interpolationEdit = segment;
    m_interpolationEnd  = segment.time + segment.interpolation.m_duration;
    initializeTimingFunctionEditor();
    m_interpolationBeatmapKey = m_currentSnapshot->beatmapPathKey;
    m_interpolationInstanceId = m_currentSnapshot->beatmapInstanceId;
    // 窗口重新打开时丢弃上次副本，避免旧谱面的范围影响新谱面。
    // 实例令牌和路径一起检查，实体编号复用不能造成跨会话误写。
    m_interpolationValidationRows.clear();
    m_interpolationBpmTimings.clear();
    m_interpolationAxisStart    = -1;
    m_interpolationAxisDuration = -1;
    auto& engine                = Logic::EditorEngine::instance();
    {
        // 注册表只在锁内借用，所有绘制与验证读取随后释放锁的副本。
        std::lock_guard lock(engine.getSessionMutex());
        auto            session = engine.getActiveSession();
        if ( !session ) return;
        const auto& registry = session->getContext().timelineRegistry;
        // 此遍历仅在打开编辑器时执行一次，不能迁移到绘制或悬停分支。
        // 只复制值状态，之后的本地校验与曲线预览完全不持有锁。
        const auto view = registry.view<const Logic::TimelineComponent>();
        m_interpolationValidationRows.reserve(view.size());
        for ( auto entity : view ) {
            const auto& timing =
                view.get<const Logic::TimelineComponent>(entity);
            Common::Render::TimelineInteractiveElement row{};
            row.time          = timing.m_timestamp;
            row.interpolation = timing.m_interpolation;
            switch ( timing.m_effect ) {
            case TimingEffect::BPM: {
                row.bpmEntity = entity;
                Timing redLine;
                redLine.m_timestamp             = timing.m_timestamp * 1000;
                redLine.m_timingEffectParameter = timing.m_value;
                redLine.m_bpm                   = timing.m_value;
                redLine.m_interpolation         = timing.m_interpolation;
                m_interpolationBpmTimings.push_back(std::move(redLine));
                break;
            }
            case TimingEffect::SCROLL: row.scrollEntity = entity; break;
            case TimingEffect::JUMP: row.jumpEntity = entity; break;
            case TimingEffect::HS: row.hsEntity = entity; break;
            }
            m_interpolationValidationRows.push_back(std::move(row));
        }
    }
    // 模态弹窗的 ImGui ID 在实际绘制窗口中打开，避免调用处 ID 栈不同。
    m_requestInterpolationEditor = true;
}

/// @brief Shift 拖动放置范围；正文双击编辑，右键删除整个段落。
/// @warning 每帧输入路径，使用已发布段落描述；不扫描 ECS 或等待异步提交。
/// @param position 当前 Timeline 图像的屏幕左上角。
/// @param size 与快照换算使用相同的图像尺寸。
/// @param hovered 输入是否位于画布有效区域且未覆盖菜单按钮。
/// @return 输入属于段落交互时返回真，普通时间点逻辑应跳过本帧。
/// @pre 调用方已检查焦点、弹窗、播放状态和文本输入。
/// @details Shift 手势保留段首和当前段尾，不逐次发布逻辑命令。
/// @note 沿用时间线吸附，手势可反向拖动，松手时才排序两端时间。
/// @note 移动模式仍允许抓取原 Timing 标记来移动整个段落。
/// @note 右键正文删除单个段落实体，所有曲线字段进入已有撤销栈。
/// @note 双击正文只打开编辑副本，不在当前帧改变数据。
bool TimelineCanvas::handleInterpolationInteraction(const ImVec2& position,
                                                    const ImVec2& size,
                                                    bool          hovered)
{
    auto&      io = ImGui::GetIO();
    const bool professional =
        Config::AppConfig::instance().getEditorSettings().professionalMode;
    // 普通模式使用当前创建类型，专业模式由按下时的泳道决定类型。
    // 手势开始后不随鼠标横向越过泳道而偷偷改变正在绘制的参数。
    // 泳道边缘留出四像素，使相邻类型范围不会共享命中外观。
    const float laneWidth = professional ? size.x / 4.0f : size.x;
    const int lane = std::clamp(static_cast<int>((io.MousePos.x - position.x) /
                                                 std::max(1.0f, laneWidth)),
                                0,
                                3);
    if ( !m_isInterpolationDragging && hovered && io.KeyShift && !io.KeyCtrl &&
         m_currentSnapshot->currentTool == Logic::EditTool::Draw &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        // 新手势不继承旧的编辑实体，否则确认可能覆盖上一个段落。
        // 保留首点直到松手，连续拖动只改变当前尾点。
        // 手势也绑定谱面实例，跨标签切换后松手不能写入新的谱面。
        m_interpolationBeatmapKey = m_currentSnapshot->beatmapPathKey;
        m_interpolationInstanceId = m_currentSnapshot->beatmapInstanceId;
        m_interpolationEdit       = {};
        m_interpolationEdit.effect =
            INTERPOLATION_EFFECTS[professional
                                      ? lane
                                      : std::clamp(m_createType, 0, 3)];
        bool snapped = false;
        m_interpolationEdit.time =
            snapTimingTime(size,
                           canvasTimeAtLocalY(size, io.MousePos.y - position.y),
                           io.MousePos.y - position.y,
                           snapped);
        // 吸附可能命中首 BPM 以前的负拍，但段落创建起点不能为负。
        // 用户仍能在窗口中以秒精度进一步调整有效范围。
        m_interpolationEdit.time = std::max(0.0, m_interpolationEdit.time);
        m_interpolationEdit.value =
            m_interpolationEdit.effect == TimingEffect::BPM
                ? (m_currentSnapshot ? m_currentSnapshot->fallbackBpm : 120.0)
                : 1.0;
        // 首值继承当前有效效果；用户仍可在编辑器中修改两端参数。
        for ( const auto& state : m_currentSnapshot->scrollSegments ) {
            if ( state.time > m_interpolationEdit.time ) break;
            if ( m_interpolationEdit.effect == TimingEffect::BPM )
                m_interpolationEdit.value = state.activeBpmValue;
            if ( m_interpolationEdit.effect == TimingEffect::SCROLL )
                m_interpolationEdit.value = state.activeScrollValue;
            if ( m_interpolationEdit.effect == TimingEffect::HS )
                m_interpolationEdit.value = state.hs;
        }
        if ( m_interpolationEdit.effect == TimingEffect::JUMP )
            m_interpolationEdit.value = 0.0;
        // 新建曲线先设为恒定，用户可以明确输入所需的终值。
        // 继承状态只决定缺省参数，不在手势拖动期间生成真实事件。
        m_interpolationEdit.interpolation.m_endValue =
            m_interpolationEdit.value;
        m_interpolationEnd        = m_interpolationEdit.time;
        m_isInterpolationDragging = true;
        m_isTimingDrawPreviewing  = false;
    }
    // 活动手势优先于正文命中，向已有段落拖动时仍能完成范围选择。
    // 合法性由弹窗明确提示，不能在半途中静默切换到普通点放置。
    if ( m_isInterpolationDragging ) {
        // 手势全程要求 Shift；松开修饰键或按 Esc 取消而非写入退化时间点。
        // 取消手势后立即消费本帧输入，普通点放置不再处理同一次鼠标释放。
        // 全程 Shift 要求与主画布的长条手势一致，避免修饰键松开后误创建。
        // 文件路径相同但重新加载也需要取消，不能仅以文件名判断来源。
        // 取消只清除 UI 手势，无需提交或等待逻辑线程撤销。
        if ( !io.KeyShift || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
             m_currentSnapshot->beatmapPathKey != m_interpolationBeatmapKey ||
             m_currentSnapshot->beatmapInstanceId !=
                 m_interpolationInstanceId ) {
            // 取消后不保留半段定义作为普通时间点，不需要追加撤销动作。
            m_isInterpolationDragging = false;
            return true;
        }
        bool snapped       = false;
        m_interpolationEnd = std::max(
            0.0,
            snapTimingTime(size,
                           canvasTimeAtLocalY(size, io.MousePos.y - position.y),
                           io.MousePos.y - position.y,
                           snapped));
        // 松手是唯一的新段提交准备点；之前的连续鼠标反馈完全在 UI 内。
        // 打开弹窗只是准备工作副本，用户确认前没有任何谱面变更。
        // 单击零范围不打开段落窗口，避免与普通单点放置手势混淆。
        // 反向拖动在此处统一起终点顺序，窗口中的二次调整仍需严格校验。
        // 手势结束后立即消除预览，提交之前不会产生可撤销实体。
        if ( ImGui::IsMouseReleased(ImGuiMouseButton_Left) ) {
            m_isInterpolationDragging = false;
            // 反向拖动在松手时统一时间顺序，曲线本身仍按早到晚定义。
            // 不足一毫秒的退化范围丢弃，不制造空段落或重复起点。
            const double start =
                std::min(m_interpolationEdit.time, m_interpolationEnd);
            const double duration =
                std::abs(m_interpolationEnd - m_interpolationEdit.time);
            if ( duration >= 0.001 ) {
                m_interpolationEdit.time                     = start;
                m_interpolationEdit.interpolation.m_duration = duration;
                openInterpolationEditor(m_interpolationEdit);
            }
        }
        return true;
    }
    // 起始命中必须在画布内部，已有手势的松手可发生在图像外。
    // 因此活动手势的处理位于这个边界检查之前。
    if ( !hovered ) return false;
    // 循环遍历的是缓存段落描述，不遍历时间线 ECS，也不展开采样点。
    // 只在对应泳道正文内响应双击或删除，不抢走相邻效果区域的输入。
    for ( const auto& segment : m_currentSnapshot->timingInterpolations ) {
        if ( professional && lane != interpolationLane(segment.effect) )
            continue;
        // 直接从段落两端投影正文，拾取不要求首标记恰好可见。
        // 纵坐标可能反向，因此用最小和最大值形成屏幕命中区间。
        const double startY = canvasYAtTime(size, segment.time) + position.y;
        const double endY =
            canvasYAtTime(size,
                          segment.time + segment.interpolation.m_duration) +
            position.y;
        if ( io.MousePos.y < std::min(startY, endY) ||
             io.MousePos.y > std::max(startY, endY) )
            continue;
        // 双击的第二次按下可能遇到单点或移动预览，打开窗口前停止它们。
        // 实际数据还未修改，取消弹窗不会污染撤销历史。
        ImGui::SetTooltip(
            "%s 插值段落\n%.3f–%.3f s，%.4g Hz\n双击编辑，右键删除整个段落",
            INTERPOLATION_LABELS[interpolationLane(segment.effect)],
            segment.time,
            segment.time + segment.interpolation.m_duration,
            segment.interpolation.m_samplesPerSecond);
        if ( ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
            // 段首不必在可见区；双击正文仍能编辑整个原始段落。
            m_isTimingDragging       = false;
            m_isTimingDrawPreviewing = false;
            openInterpolationEditor(segment);
            return true;
        }
        // 段落不是一组采样点，右键只发送一次删除指令。
        // 复用常规时间线权限与联机路由，不直接调用注册表销毁。
        if ( ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdDeleteTimelineEvent{ segment.entity }));
            return true;
        }
        // 绘制工具不会在已有段落正文内部额外放置普通时间点。
        if ( m_currentSnapshot->currentTool == Logic::EditTool::Draw )
            return true;
    }
    return false;
}

/// @brief 绘制完整区间带和连续原函数，既保留整体段落又能看出变化方向。
/// @warning 每帧覆盖层，采样上限固定，不随输出 Hz 或持续时间增加。
/// @param position 图像屏幕坐标，不依赖 Vulkan 视口的原点。
/// @param size 图像实际尺寸，决定泳道与纵向时间投影。
/// @pre 当前段落描述来自稳定的只读快照。
/// @details 区间带保留整体实体的范围，曲线横轴显示归一化参数。
/// @note 正式段落与拖动预览使用同一个绘制入口。
/// @note 通过 DrawList 裁剪，段首在视口外时仍能显示经过当前视口的正文。
/// @note 横轴归一化只影响可视图示，不改变实际效果参数。
/// @note 恒定参数显示居中直线，不以零跨度作为除数。
/// @note 与原 Timing marker 共存，不生成供表格编辑的虚拟对象。
void TimelineCanvas::renderInterpolationOverlay(const ImVec2& position,
                                                const ImVec2& size)
{
    if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) return;
    const bool professional =
        Config::AppConfig::instance().getEditorSettings().professionalMode;
    auto* draw = ImGui::GetWindowDrawList();
    // 覆盖层追加到当前图像之上，裁剪与图像的边缘严格一致。
    // 曲线和范围带的屏幕坐标不进入逻辑模型或保存格式。
    draw->PushClipRect(
        position, ImVec2(position.x + size.x, position.y + size.y), true);
    // 同一绘制回调也服务活动拖动，用户能即时看到自己选中的范围。
    // 每个段落预览固定曲线顶点预算，不随写出密度无限放大。
    const auto render =
        [&](const Common::Render::TimingInterpolationElement& segment,
            double                                            end) {
            const float laneWidth = professional ? size.x / 4.0f : size.x;
            const float left =
                position.x +
                (professional ? interpolationLane(segment.effect) * laneWidth
                              : 0.0f) +
                4.0f;
            const float right = left + laneWidth - 8.0f;
            const float y1    = position.y + static_cast<float>(canvasYAtTime(
                                                 size, segment.time));
            const float y2 =
                position.y + static_cast<float>(canvasYAtTime(size, end));
            const float top = std::min(y1, y2), bottom = std::max(y1, y2);
            // 整个区间离屏时跳过曲线求值；部分离屏交由裁剪处理。
            // 反向时间投影下同样成立，不通过段首 y 判断整段可见性。
            if ( bottom < position.y || top > position.y + size.y ) return;
            const ImU32 color = timingEffectColor(segment.effect, 220);
            // 范围背景使用低透明度，让底层拍线仍可用于判断时间位置。
            // 边框和曲线使用同类 Timing 的语义色，便于区分四类效果。
            draw->AddRectFilled(ImVec2(left, top),
                                ImVec2(right, bottom),
                                timingEffectColor(segment.effect, 30));
            draw->AddRect(
                ImVec2(left, top), ImVec2(right, bottom), color, 3.0f, 0, 1.5f);
            // 用起终值的数值范围归一横轴；下降曲线依然从较大值向较小值移动。
            // 图示的较大值位于泳道右侧，下降曲线会从右向左。
            // 不使用时间终点位置替代参数的实际增减方向。
            const auto [minimum, maximum] =
                timingInterpolationRange(segment.interpolation, segment.value);
            const double span = maximum - minimum;
            ImVec2       previous{};
            for ( int index = 0; index <= 128; ++index ) {
                const double progress = static_cast<double>(index) / 128.0;
                const double value    = evaluateTimingInterpolation(
                    segment.interpolation, segment.value, progress);
                const double ratio =
                    span > 1e-12 ? (value - minimum) / span : 0.5;
                const ImVec2 point(
                    left + 3.0f +
                        static_cast<float>(ratio) *
                            std::max(0.0f, right - left - 6.0f),
                    position.y +
                        static_cast<float>(canvasYAtTime(
                            size, std::lerp(segment.time, end, progress))));
                // 首顶点没有前一段连线，从第二点开始才追加几何。
                // 正文曲线原函数求值，不使用 ScrollCache 的数值积分虚拟点。
                if ( index ) draw->AddLine(previous, point, color, 2.0f);
                previous = point;
            }
            draw->AddText(ImVec2(left + 3.0f, std::max(top, position.y) + 3.0f),
                          color,
                          "插值段落");
        };
    for ( const auto& segment : m_currentSnapshot->timingInterpolations )
        render(segment, segment.time + segment.interpolation.m_duration);
    // 活动预览只借用 UI 的两个端点，正式描述仍来自逻辑快照。
    // 松手进入窗口后不继续画出旧手势，防止范围修改显示两份结果。
    if ( m_isInterpolationDragging )
        render(m_interpolationEdit, m_interpolationEnd);
    draw->PopClipRect();
}

/// @brief 使用工作副本编辑范围、函数与采样密度，一次提交一个可撤销事务。
/// @details 模态窗口独立于 Timeline 图像的可见状态，表格也可调用。
/// @note 使用固定 ImGui 内部 ID，不随效果或实体更换而丢失窗口状态。
/// @note 每帧只修改 UI 副本，确认后才发布一次创建或更新命令。
/// @note 时长由二次调整后的终点减起点计算，不自动修复倒置范围。
/// @note 密度以每秒次数定义，同时显示写出事件总数。
/// @note 图示使用原函数曲线，黄色点仅表达外部格式采样位置。
/// @note 模态打开期间画布不消费鼠标或键盘编辑动作。
/// @note Esc、取消和标题关闭都放弃副本，不提交中间修改。
/// @note 切换谱面后仍允许取消，但保存按钮保持禁用。
/// @note 无效状态显示原因并禁用保存，不钳制用户正在输入的文本数值。
/// @warning 每帧模态路径；只使用 UI 副本与已捕获范围，禁止遍历 ECS
/// 或等待会话锁。
void TimelineCanvas::renderInterpolationEditor()
{
    // 弹窗在同一个 UI ID 栈中打开与绘制，表格和画布入口不会互相错配。
    // 请求只消费一次，后续帧沿用 ImGui 管理的模态窗口。
    if ( m_requestInterpolationEditor ) {
        ImGui::OpenPopup("插值时间点段落###TimingInterpolationEditor");
        m_requestInterpolationEditor = false;
        m_isInterpolationEditorOpen  = true;
    }
    // 未打开编辑器的常规帧不执行校验、数量计算或窗口布局。
    // 请求标记只为延迟到稳定 ID 栈，不承担时长消抖或阻塞等待。
    if ( !m_isInterpolationEditorOpen ) return;
    const auto   workSize = ImGui::GetMainViewport()->WorkSize;
    const ImVec2 maximum(std::max(1.f, workSize.x * .95f),
                         std::max(1.f, workSize.y * .95f));
    // 初次打开才给定尺寸，重开时沿用用户调整的大小；小屏幕按工作区收敛。
    ImGui::SetNextWindowSize(
        ImVec2(std::min(620.f, maximum.x), std::min(820.f, workSize.y * .85f)),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(420.f, maximum.x), std::min(320.f, maximum.y)),
        maximum);
    bool open = true;
    // 标题关闭使用独立布尔量，不能让 ImGui 修改逻辑段落状态。
    // 允许拖动边缘或右下角缩放；内容超出高度时由窗口滚动，而非强制改回尺寸。
    if ( ImGui::BeginPopupModal("插值时间点段落###TimingInterpolationEditor",
                                &open) ) {
        auto& edit  = m_interpolationEdit;
        auto& curve = edit.interpolation;
        int   lane  = interpolationLane(edit.effect);
        // 普通模式也能在创建窗口选择效果，已有实体的类型保持不变。
        // 否则 CmdUpdate 只携带参数时可能把曲线写入错误的效果泳道。
        ImGui::BeginDisabled(edit.entity != entt::null);
        if ( ImGui::Combo("类型",
                          &lane,
                          INTERPOLATION_LABELS.data(),
                          static_cast<int>(INTERPOLATION_LABELS.size())) ) {
            edit.effect = INTERPOLATION_EFFECTS[lane];
            edit.value =
                edit.effect == TimingEffect::BPM
                    ? (m_currentSnapshot ? m_currentSnapshot->fallbackBpm
                                         : 120.0)
                    : (edit.effect == TimingEffect::JUMP ? 0.0 : 1.0);
            curve.m_endValue = edit.value;
            // 新效果的领域限制不同，自定义函数须恢复真实端点后再校验。
            m_timingFunctionEditor.m_compiledDuration = -1;
        }
        ImGui::EndDisabled();
        // 时间字段统一为秒，与 TimelineComponent 一致。
        // 原生 Timing 的毫秒转换只发生在会话存取边界，UI 不混合两套单位。
        ImGui::InputDouble("起点 (s)", &edit.time, 0.0, 0.0, "%.6f");
        ImGui::InputDouble("终点 (s)", &m_interpolationEnd, 0.0, 0.0, "%.6f");
        // 用户修改任意端点都会立即刷新时长、曲线图和预计事件数量。
        // 倒置范围保留错误提示，不能用绝对值掩盖用户输入问题。
        curve.m_duration = m_interpolationEnd - edit.time;
        // 放置范围继续使用秒，轴选择只改变函数横轴含义。
        // 改变单位后旧候选失效，即使拍数与秒数恰好相等。
        // 有理分拍保留整数，不把三分之一拍提前舍入毫秒。
        // 同刻红线和跨段变化由领域映射处理，UI 不重复积分。
        // 蓝线不随分拍变成阶梯，分拍只控制黄色输出点。
        // 普通帧比较标量，真实范围或分拍变化才准备缓存。
        // 窗口快照打开时捕获，最终保存以权威红线再次校验。
        // 错误输入保留供修正，取消和 Esc 不要求映射成功。
        // 轴选择不能绕过所属类型，BPM 始终采用时间模式。
        // 原生保存保留轴配置，再次编辑仍只有一个段落实体。
        // 红线函数只能选择秒域，自身 BPM 不能同时决定自变量速度。
        int variable = static_cast<int>(curve.m_variable);
        ImGui::BeginDisabled(edit.effect == TimingEffect::BPM);
        bool axisChanged =
            ImGui::Combo("自变量", &variable, "时间（秒）\0节拍 / 分拍\0");
        ImGui::EndDisabled();
        if ( edit.effect == TimingEffect::BPM ) variable = 0;
        if ( curve.m_variable != static_cast<TimingVariable>(variable) ) {
            curve.m_variable = static_cast<TimingVariable>(variable);
            axisChanged      = true;
            m_timingFunctionEditor.m_compiledDuration = -1;
            // 单位变化会使手绘与拟合候选失效，不能把秒域候选误当拍域候选。
            m_timingFunctionEditor.m_fit.reset();
            m_timingFunctionEditor.m_fitFunction.reset();
        }
        if ( curve.m_variable == TimingVariable::Beat ) {
            axisChanged |=
                ImGui::InputInt("分拍分子", &curve.m_beatNumerator, 0, 0);
            axisChanged |=
                ImGui::InputInt("分拍分母", &curve.m_beatDenominator, 0, 0);
            // 先准备映射，再由函数编辑器重新编译源文本；非法函数也能修改范围。
            // 常规帧只比较两个标量，不排序、分配或等待逻辑线程。
            if ( axisChanged || edit.time != m_interpolationAxisStart ||
                 curve.m_duration != m_interpolationAxisDuration ) {
                const auto kind          = curve.m_curve;
                curve.m_curve            = TimingCurve::Linear;
                m_interpolationAxisValid = bindTimingInterpolationBeatAxis(
                    curve,
                    edit.time,
                    m_interpolationBpmTimings,
                    m_currentSnapshot ? m_currentSnapshot->fallbackBpm : 120);
                curve.m_curve               = kind;
                m_interpolationAxisStart    = edit.time;
                m_interpolationAxisDuration = curve.m_duration;
            }
            if ( m_interpolationAxisValid )
                ImGui::TextWrapped(
                    "每 %d/%d 拍插入一个时间点；t 为距段首的拍数，BPM "
                    "变化时采样间隔随之变化。",
                    curve.m_beatNumerator,
                    curve.m_beatDenominator);
        } else
            m_interpolationAxisValid = true;
        // 预设切换轴后重建初始表达式模板，自定义模式则保留用户源式重编译。
        if ( axisChanged && curve.m_curve != TimingCurve::Custom )
            initializeTimingFunctionEditor();
        // 绝对函数首尾由 f(0) 和 f(duration) 推导，避免双重真值来源。
        ImGui::BeginDisabled(curve.m_curve == TimingCurve::Custom);
        ImGui::InputDouble("起始参数", &edit.value, 0.0, 0.0, "%.6g");
        ImGui::InputDouble("终点参数", &curve.m_endValue, 0.0, 0.0, "%.6g");
        ImGui::EndDisabled();
        int function = static_cast<int>(curve.m_curve);
        if ( ImGui::Combo("变化函数",
                          &function,
                          CURVE_LABELS.data(),
                          static_cast<int>(CURVE_LABELS.size())) ) {
            curve.m_curve = static_cast<TimingCurve>(function);
            // 切换模式需要重编译一次，常规帧不得重复准备积分缓存。
            m_timingFunctionEditor.m_compiledDuration = -1;
        }
        // 曲线种类切换不会丢弃控制点，切回贝塞尔可继续调整原形状。
        // 横轴是当前自变量比例，纵轴是参数比例，不是屏幕像素。
        if ( curve.m_curve == TimingCurve::Bezier ) {
            // 控制点有横纵两个坐标，限制单调时间轴并允许自由调整曲率。
            ImGui::InputDouble(
                "控制点 1 X", &curve.m_controlX1, 0.0, 0.0, "%.4f");
            ImGui::InputDouble(
                "控制点 1 Y", &curve.m_controlY1, 0.0, 0.0, "%.4f");
            ImGui::InputDouble(
                "控制点 2 X", &curve.m_controlX2, 0.0, 0.0, "%.4f");
            ImGui::InputDouble(
                "控制点 2 Y", &curve.m_controlY2, 0.0, 0.0, "%.4f");
            ImGui::TextUnformatted("控制点范围 0–1，X1 ≤ X2。");
        }
        renderTimingFunctionEditor();
        // 拍域密度不接受第二份自由输入，防止保存与预览真值冲突。
        // 常 BPM 时为 BPM / 60 / 分拍，100 BPM 半拍为 3.333… Hz。
        // 变 BPM 显示平均密度，落点逐个按拍位反解。
        // 不足完整分拍的短尾仍保留原终点，预计数量包括它。
        // 锁定状态直接读取轴枚举，不用密度数值猜模式。
        // 切回秒域即可自由编辑 Hz，旧文件缺省保持秒域。
        // 输出图按真实秒数显示，变 BPM 后间距可以不均匀。
        // 手绘图按所选自变量显示，拍域 t 不能配秒域标签。
        // 代表点预算只限制绘图次数，真实输出保留全部分拍。
        // 未准备拍轴不能伪造线性映射，领域错误阻止保存。
        // 重叠规则与轴无关，不能因分拍而放宽范围限制。
        // 拍域密度由红线和分拍推导，禁用输入避免出现两个互相矛盾的真值。
        ImGui::BeginDisabled(curve.m_variable == TimingVariable::Beat);
        ImGui::InputDouble(curve.m_variable == TimingVariable::Beat
                               ? "派生平均采样率 (Hz)"
                               : "输出采样密度 (Hz)",
                           &curve.m_samplesPerSecond,
                           0.0,
                           0.0,
                           "%.6g");
        ImGui::EndDisabled();
        // 已有时间点副本用于提前解释冲突，最终命令还会校验最新数据。
        // 联机编辑或队列提交之间出现变化时，逻辑入口拥有最终决定权。
        const char* error =
            interpolationValidationError(edit, m_interpolationValidationRows);
        if ( !m_interpolationAxisValid )
            error =
                "请设置有效的时间范围和正整数分拍，单段不能超过 65536 "
                "个采样间隔。";
        if ( curve.m_curve == TimingCurve::Custom &&
             !m_timingFunctionEditor.m_error.empty() )
            error = m_timingFunctionEditor.m_error.c_str();
        // 路径相同不表示会话相同，实例令牌防止同文件重开后实体 ID 复用。
        // 无效的工作副本允许取消，不能自动提交到后来激活的谱面。
        const bool sameBeatmap =
            m_currentSnapshot && m_currentSnapshot->hasBeatmap &&
            m_currentSnapshot->beatmapPathKey == m_interpolationBeatmapKey &&
            m_currentSnapshot->beatmapInstanceId == m_interpolationInstanceId;
        if ( !sameBeatmap ) error = "当前谱面已切换，请取消后重新打开编辑器。";
        // 只有合法数据才能参与数量计算和图形归一化，防止无穷值污染 UI。
        // 预计数量包含精确段尾，与实际格式写出函数保持同一口径。
        if ( !error ) {
            ImGui::Text("预计写出 %zu 个独立 Timing；原生保存为一个段落。",
                        timingInterpolationSampleCount(curve));
            // 图形横轴为时间，纵轴为参数；点代表输出采样，线代表原函数。
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 graphSize(
                std::max(100.0f, ImGui::GetContentRegionAvail().x), 150.0f);
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(
                origin,
                ImVec2(origin.x + graphSize.x, origin.y + graphSize.y),
                IM_COL32(20, 24, 30, 255),
                4.0f);
            const auto [minimum, maximum] =
                timingInterpolationRange(curve, edit.value);
            auto& preview = m_timingFunctionEditor.m_outputPreview;
            // 缓存保留原函数与输出点真值，绘制层不解析公式或重建导出数组。
            // 源定义或拍轴变化由缓存键检测，模式切换不会沿用旧坐标映射。
            // 预览命中区仅调整本地视野，不操作段落、采样密度或逻辑事件。
            // InvisibleButton 没有普通按钮外观，拖动不需要通用按钮反馈。
            ImGui::InvisibleButton("##TimingOutputPreview", graphSize);
            if ( ImGui::IsItemHovered() ) {
                // 鼠标滚轮由图表拥有，缩放时不能同时滚动整个弹窗。
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
                const auto& io = ImGui::GetIO();
                // 锚点按图表内光标计算，放大连续多个周期时保持检查目标稳定。
                if ( io.MouseWheel != 0.0f )
                    preview.zoomAt((io.MousePos.x - origin.x) / graphSize.x,
                                   std::pow(0.75, io.MouseWheel));
                if ( ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) )
                    preview.resetView();
            }
            // 活动拖动离开图表仍继续平移，边界约束由视图状态统一维护。
            if ( ImGui::IsItemActive() &&
                 ImGui::IsMouseDragging(ImGuiMouseButton_Left) )
                preview.pan(-ImGui::GetIO().MouseDelta.x / graphSize.x);
            // 每个真实输出样本先参与缓存，再按像素保留首尾和峰谷。
            // 稳定帧只读缓存；放大后重新展现局部样本，不跳过高频波峰。
            preview.update(curve, edit.value, graphSize.x - 12.0f);
            // 留白只影响投影，横轴比例仍对应真实段落起终时间。
            // 恒定曲线的纵轴跨度为零，绘图 helper 会放在垂直中线。
            drawTimingInterpolationPreview(
                preview,
                ImVec2(origin.x + 6.0f, origin.y + 6.0f),
                ImVec2(graphSize.x - 12.0f, graphSize.y - 12.0f),
                minimum,
                maximum,
                IM_COL32(70, 195, 255, 255),
                IM_COL32(250, 205, 95, 255));
            ImGui::Text("预览：%.6g – %.6g 秒；可见 %zu 个真实采样点。",
                        curve.m_duration * preview.viewStart(),
                        curve.m_duration * preview.viewEnd(),
                        preview.visibleSampleCount());
            // 密集区域显示包围而非每个独立圆点，文字明确说明需要放大查看。
            // 可见数量来自真实样本集合，不能把屏幕代表点数说成实际输出数。
            ImGui::TextWrapped(
                "蓝线为原函数，黄色为真实输出采样。密集区域按像素保留峰谷；"
                "滚轮放大局部，拖动平移，双击恢复完整区间。");
            ImGui::TextUnformatted(
                "双击画布段落或点击表格中的“编辑段落”可再次修改。Shift "
                "拖动创建，Esc 取消。");
            if ( edit.effect == TimingEffect::JUMP )
                ImGui::TextUnformatted(
                    "Jump 为位移脉冲；采样密度也会改变脉冲次数与累计位移。");
        } else
            ImGui::TextWrapped("%s", error);
        // 禁用保存与显示原因绑定，但取消始终可用，避免错误输入困住用户。
        // 保存后关闭工作副本窗口，撤销历史由逻辑动作完整记录。
        // 保存按钮的禁用条件来自模型与范围，而不是当前画布是否可见。
        // 用户可在隐藏时间线后继续编辑，谱面切换则由实例令牌拒绝。
        // 无效输入不自动重写为合法值，保留用户修正数值的机会。
        ImGui::BeginDisabled(error != nullptr);
        // 保存是唯一发布编辑命令的入口，图形预览没有中间写入。
        if ( UI::FeedbackButton("保存段落") ) {
            // 沿用时间线命令与权限路由；不在 UI 线程直接写注册表。
            // 新段没有 ECS 身份，由创建动作分配实体；旧段保留稳定身份。
            // 两种操作都携带整段定义，不逐样本发布大量指令。
            if ( edit.entity == entt::null ) {
                Logic::CmdCreateTimelineEvent command{ edit.time,
                                                       edit.effect,
                                                       edit.value };
                command.interpolation = curve;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(std::move(command)));
            } else {
                Logic::CmdUpdateTimelineEvent command{ edit.entity,
                                                       edit.time,
                                                       edit.value };
                command.interpolationOverride = curve;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(std::move(command)));
            }
            m_isInterpolationEditorOpen = false;
            ImGui::CloseCurrentPopup();
        }
        // 禁用样式只覆盖保存按钮，不能传播到取消或 Esc 分支。
        // 每次成功或取消都同时清除本地窗口标志并关闭 ImGui 弹窗。
        ImGui::EndDisabled();
        ImGui::SameLine();
        if ( UI::FeedbackButton("取消") ||
             ImGui::IsKeyPressed(ImGuiKey_Escape) ) {
            m_isInterpolationEditorOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // 标题关闭与取消采用相同的副本丢弃语义。
    // 恢复后台画布交互只依赖本地标志，不等待逻辑线程返回或固定超时。
    if ( !open ) m_isInterpolationEditorOpen = false;
}
}  // namespace MMM::Canvas
