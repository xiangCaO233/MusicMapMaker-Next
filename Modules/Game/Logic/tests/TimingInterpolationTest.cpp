#include "mmm/timing/TimingInterpolation.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "mmm/timing/Timing.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace
{
/// @brief 以明确失败日志聚合断言，不依赖发布构建中关闭的 assert。
/// @param condition 被验证的行为结果。
/// @param message 标识失败的业务边界。
/// @return 保留结果，允许一个场景报告多处不满足条件的状态。
bool check(bool condition, const char* message)
{
    if ( !condition ) XERROR("TimingInterpolationTest: {}", message);
    return condition;
}

/// @brief 双精度曲线检查，不把导出格式的毫秒量化误差混入数学断言。
bool near(double left, double right, double tolerance = 1e-7)
{
    return std::abs(left - right) < tolerance;
}

/// @brief 验证端点、函数形状、拍位积分及反解的闭环。
/// @details 线性 120→240 BPM 两秒应产生六拍，与采样次数无关。
/// 贝塞尔对角控制点应等价于直线，横向控制点的反解不能省略。
bool testCurves()
{
    // 两秒线性渐变的平均 BPM 是 180，积分应为 360 BPM·秒。
    // 测试直接使用领域函数，排除画布缩放和格式量化造成的误差。
    // 同一有效模型随后用于所有形状，端值保持一致以便比较。
    // 不通过导出后的阶梯数组推导参考值，避免测试复制采样实现。
    // 时间比例是归一化横轴，与实际时间的秒单位分开。
    // 双精度容差检查解析函数，不能套用毫秒格式的粗容差。
    MMM::TimingInterpolation curve;
    curve.m_duration = 2.0;
    curve.m_endValue = 240.0;
    bool ok          = true;
    // 枚举连续范围覆盖所有预设初等函数，自定义函数在独立场景检查。
    // 起终点必须精确到约定容差，中间曲率可以不同。
    // 首尾一致是用户编辑端值的契约，不只是某一种缓动函数的特征。
    // 每种形状随后独立检查累计拍数的反解闭环。
    // BPM 两端为正，因此拍数严格单调，理论上有唯一的时间解。
    // 查询段前与段后验证的是外推契约，而不是只在合法区间内自洽。
    for ( int type = 0;
          type <= static_cast<int>(MMM::TimingCurve::HyperbolicTangent);
          ++type ) {
        curve.m_curve = static_cast<MMM::TimingCurve>(type);
        // 每一种函数都必须准确穿过用户给定的两端，不能暗中改变端值。
        ok &= check(
            near(MMM::evaluateTimingInterpolation(curve, 120.0, 0.0), 120.0),
            "curve start");
        ok &= check(
            near(MMM::evaluateTimingInterpolation(curve, 120.0, 1.0), 240.0),
            "curve end");
        MMM::Logic::TimelineComponent timing{
            3.0, MMM::TimingEffect::BPM, 120.0, {}, curve
        };
        for ( double time : { 2.0, 3.0, 3.2, 3.75, 4.5, 5.0, 8.0 } ) {
            // 同时覆盖首点前外推、曲线内以及段尾以后继承终 BPM。
            const auto beat = MMM::Logic::timelineBeatsAt(timing, time);
            ok &=
                check(near(MMM::Logic::timelineTimeAtBeat(timing, beat), time),
                      "beat inverse");
        }
    }
    // 端点通过并不能证明积分正确，另用已知平均值检查完整累计量。
    // 再将相同定义切到对角贝塞尔，验证时间控制点反解没有被省略。
    // 对角形状应与直线完全重合，0.4 时刻不是控制点参数 0.4。
    // 如果把贝塞尔内部参数当横轴，此处的值会暴露错误。
    // 完整积分与单点求值分开验证，两者不能互相作为参考。
    // 非线性情况在后面再与独立数值积分比对。
    curve.m_curve = MMM::TimingCurve::Linear;
    ok &=
        check(near(MMM::integrateTimingInterpolation(curve, 120.0, 2.0), 360.0),
              "linear integral");
    curve.m_curve = MMM::TimingCurve::Bezier;
    ok &=
        check(near(MMM::evaluateTimingInterpolation(curve, 120.0, 0.4), 168.0),
              "Bezier time inversion");
    ok &=
        check(near(MMM::integrateTimingInterpolation(curve, 120.0, 2.0), 360.0),
              "Bezier integral");
    // 非线性的 Bézier 也要和独立数值积分吻合，避免只测对角退化曲线。
    curve.m_controlX1 = 0.1;
    curve.m_controlY1 = 0.9;
    curve.m_controlX2 = 0.6;
    curve.m_controlY2 = 0.2;
    // 中点规则在固定一万片上求数值参考，和被测解析积分不是同一算法。
    // 非对角控制点同时扰动横轴和纵轴，避免退化直线掩盖公式错误。
    // 固定预算使测试可重复且不需要依赖墙钟等待或播放线程。
    // 容差允许中点规则截断误差，但不能容忍遗漏横轴导数的误差。
    // 参考覆盖全段，积分尾部的常值延续由前面的反解闭环覆盖。
    // 导出密度不参与参考计算，修改默认密度不应改变结果。
    double sum = 0.0;
    for ( int index = 0; index < 10000; ++index )
        sum += MMM::evaluateTimingInterpolation(
                   curve, 120.0, (index + 0.5) / 10000.0) *
               2.0 / 10000.0;
    ok &= check(
        near(MMM::integrateTimingInterpolation(curve, 120.0, 2.0), sum, 1e-4),
        "nonlinear Bezier integral");
    // 自定义函数采用秒域而非比例；锚点不在零时也必须准确累计拍数。
    // 函数中途升降但 BPM 始终为正，拍位反解仍须唯一且连续。
    // 同一缓存覆盖段首以前、域内和段尾继承，不重启相位。
    MMM::TimingInterpolation custom;
    custom.m_duration       = 2;
    double      customStart = 120;
    std::string customError;
    if ( !MMM::setTimingInterpolationFunction(
             custom, "120+20*sin(pi*t)", customStart, customError) )
        return check(false, "custom compile for beat inverse");
    MMM::Logic::TimelineComponent customTiming{
        3.0, MMM::TimingEffect::BPM, customStart, {}, custom
    };
    for ( double time : { 2.0, 3.0, 3.0001, 3.137, 4.2, 5.0, 8.0 } ) {
        const auto beat = MMM::Logic::timelineBeatsAt(customTiming, time);
        ok &= check(
            near(MMM::Logic::timelineTimeAtBeat(customTiming, beat), time),
            "custom beat inverse");
    }
    // 输出 Hz 只控制写出事件，改变它不能改变自定义函数的拍位累计。
    const double originalBeat = MMM::Logic::timelineBeatsAt(customTiming, 4.2);
    customTiming.m_interpolation->m_samplesPerSecond = 1;
    ok &= check(
        near(MMM::Logic::timelineBeatsAt(customTiming, 4.2), originalBeat),
        "custom beat independent of density");
    // 输入边界必须在创建或读入前失败，不允许把 NaN 带到排序和图形投影。
    // 非法持续时间必须先拒绝，不能在后来生成样本时钳制成零。
    // 恢复时长后单独破坏 BPM，隔离每一种输入错误的判据。
    // 终值为零会破坏单调拍数反解，所以 BPM 约束要检查两端。
    // 最后只替换密度为无穷，确认预算字段也纳入有限性检查。
    // 这些失败都不得要求抛出异常，模型接口以布尔结果表达失败。
    // 每次恢复前一项参数，避免多个错误使后续断言失去针对性。
    curve.m_duration = -1.0;
    ok &= check(
        !MMM::isValidTimingInterpolation(curve, MMM::TimingEffect::BPM, 120.0),
        "negative duration rejected");
    curve.m_duration = 2.0;
    curve.m_endValue = 0.0;
    ok &= check(
        !MMM::isValidTimingInterpolation(curve, MMM::TimingEffect::BPM, 120.0),
        "zero BPM rejected");
    curve.m_endValue         = 240.0;
    curve.m_samplesPerSecond = std::numeric_limits<double>::infinity();
    ok &= check(
        !MMM::isValidTimingInterpolation(curve, MMM::TimingEffect::BPM, 120.0),
        "nonfinite density rejected");
    return ok;
}

/// @brief 验证整个段落只产生一个实体和一个撤销事务。
/// @details 相邻同效果段允许接界，内部点及内部交叠均拒绝。
/// 撤销更新恢复函数和密度，撤销创建移除整个段落而非一批样本。
bool testActionsAndCache()
{
    // 独立上下文不打开真实音频设备，不读取用户项目或配置目录。
    // 指令经过真实 ActionController，检查实际实体与撤销事务。
    // 不手工往撤销栈塞入假动作，否则无法发现创建入口拆分段落的问题。
    // 稀疏密度作为第一版本，再提高密度检验运行时行为保持稳定。
    // 曲线只存为一个实体，虚拟积分点由缓存内部管理。
    // 后续同一个 registry view 会观察动作结果，不复制实体集合。
    MMM::Logic::SessionContext         context;
    MMM::Logic::ActionController       controller(context);
    MMM::Logic::CmdCreateTimelineEvent create{ 1.0,
                                               MMM::TimingEffect::BPM,
                                               120.0 };
    create.interpolation                     = MMM::TimingInterpolation{};
    create.interpolation->m_duration         = 2.0;
    create.interpolation->m_endValue         = 240.0;
    create.interpolation->m_samplesPerSecond = 2.0;
    controller.handleCommand(create);
    auto view = context.timelineRegistry.view<MMM::Logic::TimelineComponent>();
    bool ok   = check(view.size() == 1, "one segment entity");
    if ( view.empty() ) return false;
    const auto entity = *view.begin();
    // 普通点不能切断曲线内部，但不同效果仍能同时变化。
    controller.handleCommand(MMM::Logic::CmdCreateTimelineEvent{
        2.0, MMM::TimingEffect::BPM, 180.0 });
    ok &= check(view.size() == 1, "point inside segment rejected");
    // 第二条普通点与第二个插值段分别测试内部占用的拒绝路径。
    // 两次失败都不能写入撤销栈，后面仍可直接撤销首次有效更新。
    // 交叠段使用相同效果，其他效果允许在相同时间区间独立变化。
    // 已有实体数量是可观察的判据，不仅检查命令函数有没有返回。
    // 此处不生成触及端点的场景，端点事务由另一个测试覆盖。
    // 失败的创建也不应分配带空定义的普通时间点。
    auto overlap = create;
    overlap.time = 2.0;
    controller.handleCommand(overlap);
    ok &= check(view.size() == 1, "overlapping segment rejected");
    // 低于运行时精度下限的输出密度不应降低编辑器的积分精度。
    // 两个缓存都从同一实体状态建立，唯一差异是写出采样密度。
    // 比较段内投影位置，防止低密度输出使原有积分网格退化。
    // 使用固定查询时间避免播放状态与 UI 更新影响结果。
    // 快照描述数量额外确认段落仍有可编辑的整体身份。
    // 虚拟采样不应伪造 effects 标记，表格只展示原实体。
    // 即使积分缓存有很多内部段，操作对象仍必须只有一个。
    MMM::Config::EditorConfig       config;
    MMM::Logic::System::ScrollCache first;
    first.rebuild(context.timelineRegistry, config, nullptr);
    const double                       position = first.getAbsY(2.5);
    MMM::Logic::CmdUpdateTimelineEvent update{ entity, 1.0, 120.0 };
    update.interpolationOverride                     = create.interpolation;
    update.interpolationOverride->m_samplesPerSecond = 100.0;
    controller.handleCommand(update);
    MMM::Logic::System::ScrollCache second;
    second.rebuild(context.timelineRegistry, config, nullptr);
    ok &= check(near(second.getAbsY(2.5), position),
                "runtime curve independent of export density");
    ok &= check(second.getInterpolations().size() == 1,
                "one rendered segment descriptor");
    // 虚拟积分点不生成时间线红线或实体，表格仍只有一个操作对象。
    const auto markers =
        std::count_if(second.getSegments().begin(),
                      second.getSegments().end(),
                      [](const auto& segment) { return segment.effects != 0; });
    ok &= check(markers == 1 && view.size() == 1,
                "virtual samples not editable markers");
    // 撤销更新应该还原完整定义，不只把起始参数改回去。
    // 密度字段作为非起点数据的判据，确保附加字段进入动作快照。
    // 重做恢复较密定义，之后两次撤销分别撤销更新和最初创建。
    // 如果失败创建错误增加历史，第二次撤销不会让实体集合为空。
    // 最后重做创建确认整段可重新出现，不能恢复成一串离散点。
    // 本场景不依赖实体 ID 在删除后复用，只检查定义或集合数量。
    context.actionStack.undo(context);
    ok &= check(near(view.get<MMM::Logic::TimelineComponent>(entity)
                         .m_interpolation->m_samplesPerSecond,
                     2.0),
                "undo entire curve edit");
    context.actionStack.redo(context);
    ok &= check(near(view.get<MMM::Logic::TimelineComponent>(entity)
                         .m_interpolation->m_samplesPerSecond,
                     100.0),
                "redo entire curve edit");
    context.actionStack.undo(context);
    context.actionStack.undo(context);
    ok &= check(view.empty(), "undo segment creation");
    context.actionStack.redo(context);
    ok &= check(view.size() == 1, "redo one segment");
    return ok;
}

/// @brief 节拍段落经真实命令与缓存重建，范围移动和撤销都应重新取得拍轴。
/// @details 有意传入无映射的命令副本，验证逻辑层不依赖 UI 已准备缓存。
/// 修改 BPM 后不重新编辑效果段，仍应在下一次脏重建中反映新分拍。
/// 撤销范围修改恢复整段，不能只恢复可见首值而遗留新时间轴。
/// @return 一个可编辑实体、正确分拍和缓存版本均符合预期时为真。
bool testBeatCommandsAndCache()
{
    // 当前效果段从零开始，初始红线与效果可以共享绝对时间而保持独立身份。
    // 拍域累计从效果段首起算，不把 BPM 实体编号当作相位来源。
    // 红线修改之后不手工重写效果组件，观察实际缓存重建的派生结果。
    // 撤销只针对最后一个范围动作，之前红线更新仍是有效的权威状态。
    // 固定时刻的数值检查不依赖播放线程速度或任何等待窗口。
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);
    controller.handleCommand(
        MMM::Logic::CmdCreateTimelineEvent{ 0, MMM::TimingEffect::BPM, 100 });
    // 同一个权威 registry 同时提供红线和效果，附带 BPM 字段不会参与拍轴。
    auto view = context.timelineRegistry.view<MMM::Logic::TimelineComponent>();
    const auto                         bpmEntity = *view.begin();
    MMM::Logic::CmdCreateTimelineEvent command{ 0,
                                                MMM::TimingEffect::SCROLL,
                                                1 };
    command.interpolation             = MMM::TimingInterpolation{};
    command.interpolation->m_variable = MMM::TimingVariable::Beat;
    command.interpolation->m_duration = 1.2;
    command.interpolation->m_endValue = 3;
    controller.handleCommand(command);
    bool ok = check(view.size() == 2, "beat command creates one segment");
    entt::entity effect = entt::null;
    for ( auto entity : view )
        if ( view.get<MMM::Logic::TimelineComponent>(entity).m_effect ==
             MMM::TimingEffect::SCROLL )
            effect = entity;
    if ( effect == entt::null ) return false;
    const auto& original =
        *view.get<MMM::Logic::TimelineComponent>(effect).m_interpolation;
    ok &= check(
        near(original.m_samplesPerSecond, 10.0 / 3) && original.m_beatAxis,
        "command prepares authoritative BPM cache");
    MMM::Config::EditorConfig       config;
    MMM::Logic::System::ScrollCache cache;
    cache.rebuild(context.timelineRegistry, config, nullptr);
    // ScrollCache 重建发布一段可编辑描述，不向 registry 创建虚拟事件。
    // 首次半拍 300 毫秒是独立参考，不从派生 Hz 反推期望。
    // 连续参数的运行时曲率与输出周期分离，改变分拍不重写原函数。
    // 未加载真实 BeatMap 时仍使用显式红线而非回退 BPM。
    // 运行时描述仍是一段，BPM 更新不需要手工打开并保存插值窗口。
    ok &=
        check(cache.getInterpolations().size() == 1 &&
                  near(MMM::timingInterpolationSampleElapsed(
                           cache.getInterpolations().front().interpolation, 1),
                       .3),
              "runtime half beat sample");
    controller.handleCommand(
        MMM::Logic::CmdUpdateTimelineEvent{ bpmEntity, 0, 200 });
    cache.rebuild(context.timelineRegistry, config, nullptr);
    ok &= check(near(MMM::timingInterpolationSampleElapsed(
                         cache.getInterpolations().front().interpolation, 1),
                     .15),
                "runtime BPM change rebinds beat axis");
    // 修改 BPM 与修改效果是两个历史事务，后续撤销只撤回效果范围。
    // BPM 仍为 200，所以重建后的旧范围应积累四拍。
    // 不能把撤销恢复的旧 100 BPM 缓存继续用于当前红线。
    // 命令必须保留新的拍域配置，不能降级成普通时间点通过校验。
    // 缩短范围不改变实体身份，仍由既有 TimelineAction 保存完整副本。
    // 修改范围后命令自行准备新域，撤销恢复旧范围而非一批独立样本。
    MMM::Logic::CmdUpdateTimelineEvent edit{ effect, .1, 1 };
    edit.interpolationOverride             = command.interpolation;
    edit.interpolationOverride->m_duration = .6;
    controller.handleCommand(edit);
    ok &= check(near(view.get<MMM::Logic::TimelineComponent>(effect)
                         .m_interpolation->m_beatDuration,
                     2),
                "edit command uses latest BPM");
    // 撤销后从 registry 重新借用，避免将动作前的组件引用误作恢复结果。
    // 时间锚点和持续时长各自检查，只恢复首值不应通过。
    // 旧拍域缓存与新红线不一致时，查询依赖下一次脏重建重新准备。
    // 重新准备没有写入额外动作，之后仍可独立重做本次范围修改。
    // 这个用例不以空命令或直接覆盖组件代替真实撤销栈调用。
    // 最终数学域由当前 BPM 决定，撤销不能反向撤回未指定的红线变化。
    context.actionStack.undo(context);
    const auto& restored = view.get<MMM::Logic::TimelineComponent>(effect);
    ok &= check(near(restored.m_timestamp, 0) &&
                    near(restored.m_interpolation->m_duration, 1.2),
                "undo restores whole beat segment");
    cache.rebuild(context.timelineRegistry, config, nullptr);
    ok &= check(
        near(cache.getInterpolations().front().interpolation.m_beatDuration, 4),
        "undo cache reflects current red line");
    return ok;
}

/// @brief 验证普通起点升级、共同端点和相邻段协调批量移动。
/// @details 覆盖手势从已有红线开始及后续表格批量编辑的真实事务边界。
/// @note 每次查询重新借用 registry，撤销重做可能改变实体身份。
/// @return 所有成功与拒绝路径保持预期最终状态时返回真。
bool testBoundariesAndBatch()
{
    // 从已有普通点开始模拟手势，不能在同刻创建第二条重复起点。
    // 升级使用原身份，撤销回到普通点而非删除整条旧红线。
    // 第二段在第一段尾开始，验证边界接续与内部重叠的区别。
    // 两段整体平移后依然接界，事务要检查最终位置组合。
    // 不能依赖 registry 遍历顺序，比较的是命令携带的每个实体。
    // 最后制造同起点冲突，确认失败事务不会留下半批移动。
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdCreateTimelineEvent{
        1.0, MMM::TimingEffect::BPM, 120.0 });
    auto view = context.timelineRegistry.view<MMM::Logic::TimelineComponent>();
    const auto                         original = *view.begin();
    MMM::Logic::CmdCreateTimelineEvent create{ 1.0,
                                               MMM::TimingEffect::BPM,
                                               120.0 };
    create.interpolation             = MMM::TimingInterpolation{};
    create.interpolation->m_duration = 2.0;
    create.interpolation->m_endValue = 180.0;
    controller.handleCommand(create);
    bool ok = check(
        view.size() == 1 && view.get<MMM::Logic::TimelineComponent>(original)
                                .m_interpolation.has_value(),
        "upgrade existing point");
    // 升级的撤销应退回普通点，不能同时删除原红线。
    context.actionStack.undo(context);
    ok &= check(
        view.size() == 1 &&
            !view.get<MMM::Logic::TimelineComponent>(original).m_interpolation,
        "undo point upgrade");
    context.actionStack.redo(context);
    create.time                      = 3.0;
    create.value                     = 180.0;
    create.interpolation->m_endValue = 240.0;
    controller.handleCommand(create);
    ok &= check(view.size() == 2, "touching segments allowed");
    // 两段一起平移后仍然接界，验证基于新状态而不是另一段的旧位置。
    MMM::Logic::CmdUpdateTimelineEvents updates;
    for ( auto entity : view ) {
        const auto& timing = view.get<MMM::Logic::TimelineComponent>(entity);
        updates.events.push_back(
            { entity, timing.m_timestamp + 1.0, timing.m_value });
    }
    controller.handleCommand(updates);
    for ( const auto& update : updates.events )
        ok &= check(near(view.get<MMM::Logic::TimelineComponent>(update.entity)
                             .m_timestamp,
                         update.newTime),
                    "coordinated segment move");
    // 无效最终范围使整批保持原位，不允许第一条已执行而第二条被拒绝。
    // 将两个最终起点改为相同位置，明确制造非法的内部重叠。
    // 先记录实际旧时间，再提交整批，不假定 registry 的遍历顺序。
    // 判据是失败后旧状态保持不变，不能先移动一条再拒绝另一条。
    // 此检查覆盖表格批量移动，而单实体重叠由前一个场景覆盖。
    // 相邻段同时移动成功证明合法性不是依赖执行顺序碰巧成立。
    // 拒绝无效事务也必须保持后续撤销记录与已执行动作一致。
    updates.events.front().newTime = updates.events.back().newTime;
    const auto before =
        view.get<MMM::Logic::TimelineComponent>(updates.events.front().entity)
            .m_timestamp;
    controller.handleCommand(updates);
    ok &= check(near(view.get<MMM::Logic::TimelineComponent>(
                             updates.events.front().entity)
                         .m_timestamp,
                     before),
                "overlapping batch rejected atomically");
    return ok;
}

/// @brief 覆盖全局粘贴入口的段落冲突和渐变 BPM 拍位换算。
/// @details 画布快捷键与独立表格入口必须具有相同范围规则。
/// @return 正文内粘贴被拒绝、范围外保留定义且分拍粘贴准确时返回真。
/// @note 剪贴板状态只在本进程测试中存在，场景末尾明确清空。
/// @note 拒绝场景与成功场景使用同一份载荷，只有目标时间不同。
/// @note 按拍粘贴使用另一种效果，避免同类型互斥影响换算判据。
/// @note 渐变区间的一拍时长不同于起始 BPM 的固定半秒。
/// @note 目标存在性单独断言，未找到类型不能因循环无检查而通过。
/// @note 场景不创建 Session 线程，EditorEngine 只用于共享载荷接口。
bool testClipboard()
{
    // 全局 CmdPaste 有独立代码路径，不能只测批量 Timeline 创建命令。
    // 建立一条渐变 BPM，在段内粘贴另一段同效果应保持实体数量不变。
    // 然后移动粘贴锚点到段外，检查创建实体仍带完整插值定义。
    // 第三个场景粘贴不同效果的普通点，排除同效果冲突对拍位换算的干扰。
    // 拍位参考来自解析反解，而不是用粘贴结果自身推导参考值。
    // 所有操作都经过正式命令入口，不直接改 registry 绕过验证。
    MMM::Logic::SessionContext         context;
    MMM::Logic::ActionController       controller(context);
    MMM::Logic::CmdCreateTimelineEvent create{ 1.0,
                                               MMM::TimingEffect::BPM,
                                               120.0 };
    create.interpolation             = MMM::TimingInterpolation{};
    create.interpolation->m_duration = 2.0;
    create.interpolation->m_endValue = 240.0;
    controller.handleCommand(create);
    auto view = context.timelineRegistry.view<MMM::Logic::TimelineComponent>();
    MMM::Logic::TimelineClipboardItem item;
    item.timeline = {
        0.0, MMM::TimingEffect::BPM, 120.0, {}, create.interpolation
    };
    item.timeline.m_interpolation->m_duration = 1.0;
    auto& engine = MMM::Logic::EditorEngine::instance();
    // 秒粘贴只移动首时间，保留独立的段长和函数定义。
    // 同步失败不能产生部分实体或空撤销动作。
    engine.setTimelineClipboard({ item }, &context, false);
    context.animateTime = 2.0;
    controller.handleCommand(MMM::Logic::CmdPaste{});
    bool ok = check(view.size() == 1, "global paste rejects overlap");
    context.animateTime = 4.0;
    controller.handleCommand(MMM::Logic::CmdPaste{});
    ok &= check(view.size() == 2, "global paste keeps segment");
    // 切到按拍粘贴，一拍以后在渐变曲线里不等于固定的半秒。
    // 不同效果的普通点允许在 BPM 段内部存在，不能被范围校验错误拒绝。
    item.timeline        = { 0.0, MMM::TimingEffect::SCROLL, 1.5 };
    item.hasBeatPosition = true;
    item.relativeBeat    = 1.0;
    engine.setTimelineClipboard({ item }, &context, false);
    context.lastConfig.settings.copyPasteTimeBasis =
        MMM::Config::CopyPasteTimeBasis::Beat;
    context.animateTime = 1.0;
    controller.handleCommand(MMM::Logic::CmdPaste{});
    const MMM::Logic::TimelineComponent reference{
        1.0, MMM::TimingEffect::BPM, 120.0, {}, create.interpolation
    };
    const double expected = MMM::Logic::timelineTimeAtBeat(reference, 1.0);
    bool         found    = false;
    for ( auto entity : view ) {
        const auto& timing = view.get<MMM::Logic::TimelineComponent>(entity);
        if ( timing.m_effect == MMM::TimingEffect::SCROLL ) {
            found = true;
            ok &= check(near(timing.m_timestamp, expected),
                        "beat paste follows continuous BPM");
        }
    }
    // 数量检查之外确认目标类型出现，避免遍历没有执行断言却误报成功。
    // 清空共享剪贴板后，后续场景不会消费当前会话的借用来源身份。
    ok &= check(found, "beat paste created target");
    engine.setTimelineClipboard({}, &context, false);
    return ok;
}

/// @brief 验证原生段落往返与外部格式离散化、终点采样和源数据不变。
/// @details 文件只写构建树 test_output；不读写用户配置或源码夹具。
/// 使用非整周期 1.25 秒段，确保最后一次输出精确保留段尾。
bool testStorage()
{
    // 测试输出由构建宏给出，所有临时谱面只进入 test_output。
    // 输出文件可重复覆盖，不修改受 LFS 管理的源码资源。
    // 原生、Malody、osu! 分别走真实格式选择入口，覆盖保存分流。
    // 采用段尾不落在完整周期上的范围，避免漏段尾被整数周期掩盖。
    // 文件域的时间为毫秒，模型持续时间为秒，断言明确换算单位。
    // 保存后还要查询原始内存模型，导出不得污染编辑状态。
    const std::filesystem::path output(MMM_INTERPOLATION_TEST_OUTPUT);
    std::error_code             ec;
    std::filesystem::create_directories(output, ec);
    if ( ec ) return check(false, "test output directory");
    // 段首定义同时填写参数、BPM 和拍长，符合领域模型的派生字段约定。
    // 低密度输出产生起点、两个完整周期和一个精确终点。
    // 原生格式保留这个定义，外部格式无法携带定义时再展开。
    // 选择正 BPM 避免格式 writer 的规范化掩盖端值错误。
    // 同一模型顺序保存多种格式，确保一次导出不会影响下一次。
    // sync 为元数据计算服务，不能把插值定义拆成可编辑实体。
    MMM::BeatMap map;
    map.m_baseMapMetadata.preference_bpm = 120.0;
    MMM::Timing timing;
    timing.m_timestamp             = 0.0;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = timing.m_bpm = 120.0;
    timing.m_beat_length                          = 500.0;
    timing.m_interpolation                        = MMM::TimingInterpolation{};
    timing.m_interpolation->m_duration            = 1.25;
    timing.m_interpolation->m_endValue            = 240.0;
    timing.m_interpolation->m_samplesPerSecond    = 2.0;
    map.m_timings.push_back(timing);
    map.sync();
    bool ok     = check(map.saveToFile(output / "segment.mmm"), "native save");
    auto native = MMM::BeatMap::loadFromFile(output / "segment.mmm");
    ok &= check(
        native.m_timings.size() == 1 &&
            native.m_timings.front().m_interpolation == timing.m_interpolation,
        "native keeps one editable segment");
    // 原生往返必须保留完整 optional 值，不能只保留起始参数。
    // 显式检查采样末尾的毫秒时间，确认最后短周期没有被省略。
    // 每个输出样本都应清除定义，避免递归保存重复展开。
    // 单独的采样器检查用于定位失败，格式文件断言仍不可省略。
    // 外部 writer 消费的是临时视图，下面会再次检查原对象不变。
    // 预计数量与真实输出数量同口径，界面才不会误导用户。
    const auto samples = MMM::sampleTimingInterpolations(map.m_timings);
    ok &=
        check(samples.size() == 4 && near(samples.back().m_timestamp, 1250.0) &&
                  near(samples.back().m_bpm, 240.0),
              "Hz plus precise endpoint");
    for ( const auto& event : samples )
        ok &= check(!event.m_interpolation, "discrete export samples");
    // osu! 也须经过真实格式入口展开，而不是把原生段落字段写入目标文件。
    ok &= check(map.saveToFile(output / "segment.osu"), "osu export");
    const auto osu = MMM::BeatMap::loadFromFile(output / "segment.osu");
    ok &= check(osu.m_timings.size() == 4 &&
                    std::none_of(osu.m_timings.begin(),
                                 osu.m_timings.end(),
                                 [](const auto& event) {
                                     return event.m_interpolation.has_value();
                                 }),
                "osu discrete timing count");
    // 变速副本缩放时长和 BPM 端点，但仍保留相同数量的写出样本。
    // 变速副本同时压缩时间和扩大 BPM 参数，累计拍数应保持原语义。
    // 密度按相反比例变化，时长乘密度仍得到相同输出数量。
    // 检查的是变换后的原生段落，不能仅用已展开数组证明保留定义。
    // 控制点归一化坐标无须缩放，曲线种类在变速时也不能丢失。
    // 变换对象是独立结果，原始 map 随后继续完成 Malody 写出。
    // 若变换失败，短路条件保护后续访问空 timings，报告失败而不崩溃。
    MMM::BeatmapSpeedTransformOptions options;
    options.speed = 2.0;
    const auto transformed =
        MMM::BeatmapSpeedTransform::createSpeedVersion(map, options);
    ok &= check(
        transformed.success &&
            transformed.beatmap.m_timings.front().m_interpolation &&
            near(transformed.beatmap.m_timings.front()
                     .m_interpolation->m_duration,
                 0.625) &&
            near(transformed.beatmap.m_timings.front()
                     .m_interpolation->m_endValue,
                 480.0) &&
            MMM::timingInterpolationSampleCount(
                *transformed.beatmap.m_timings.front().m_interpolation) == 4,
        "speed transform preserves interpolation");
    // Malody 的 time 数组使用格式 writer 的真实输出，不以临时展开数组代替验证。
    ok &= check(map.saveToFile(output / "segment.mc"), "Malody export");
    std::ifstream input(output / "segment.mc");
    const auto    json = nlohmann::json::parse(input, nullptr, false);
    ok &= check(!json.is_discarded() && json.contains("time") &&
                    json["time"].size() == 4,
                "Malody discrete timing count");
    ok &= check(
        map.m_timings.size() == 1 &&
            map.m_timings.front().m_interpolation == timing.m_interpolation,
        "export preserves editable source");
    // 四类效果都必须保留段落字段，不能仅给 BPM 实现序列化。
    // 非 BPM 效果允许负终值，验证序列化没有误套用正 BPM 限制。
    // 每种效果都覆盖保存与读取，不能仅测公共 JSON helper。
    // 循环复用同一输出文件，但每次读取当前效果的实际新文件。
    // optional 的完整相等包含曲线、密度、时长和贝塞尔控制点。
    // 不把外部格式的能力当作原生模型的限制，原生要保留四类定义。
    // 本循环只验证往返，Jump 的离散脉冲语义由运行时按样本应用。
    for ( auto effect : { MMM::TimingEffect::SCROLL,
                          MMM::TimingEffect::JUMP,
                          MMM::TimingEffect::HS } ) {
        map.m_timings.front().m_timingEffect              = effect;
        map.m_timings.front().m_timingEffectParameter     = 1.0;
        map.m_timings.front().m_interpolation->m_endValue = -0.5;
        ok &=
            check(map.saveToFile(output / "effect.mmm"), "effect native save");
        const auto loaded = MMM::BeatMap::loadFromFile(output / "effect.mmm");
        ok &= check(loaded.m_timings.size() == 1 &&
                        loaded.m_timings.front().m_interpolation ==
                            map.m_timings.front().m_interpolation,
                    "effect segment roundtrip");
    }
    return ok;
}
/// @brief 高频插值的运行时状态与导出网格一致，积分仍保留实际振荡。
/// @details 使用截图中的 5000 Hz 正弦定义，旧 240 Hz 网格无法通过。
/// @return 全部导出点间状态、累计距离和精确段尾符合预期时为真。
/// @note 查询落在两个样本之间，验证真实阶梯状态而非恰好命中数学端点。
/// @note 原生段落保持一个实体，虚拟采样不能增加可编辑时间线对象。
/// @note 秒域持续效果的阶梯语义与外部格式落点一致，非 Jump 的脉冲累加。
/// @note 所有查询读取已准备缓存，不用等待播放线程推进来验证精度。
bool testHighDensityRuntime()
{
    MMM::TimingInterpolation curve;
    curve.m_duration         = 2.11587;
    curve.m_samplesPerSecond = 5000;
    // 5000 Hz 指每秒输出点密度，100 指公式角频率，两者不能混为周期数。
    // 时长尾部不落在 0.2 毫秒网格，用它覆盖精确端点的独立处理。
    double      startValue = 0;
    std::string error;
    if ( !MMM::setTimingInterpolationFunction(
             curve, "sin(100*t)", startValue, error) )
        return check(false, "high density formula");
    entt::registry registry;
    const auto     entity = registry.create();
    // 单一真实 Scroll 实体足以覆盖运行时展开，不需要加载谱面文件。
    // 注册表保持稳定，缓存重建期间没有动作线程修改组件。
    registry.emplace<MMM::Logic::TimelineComponent>(entity,
                                                    1.0,
                                                    MMM::TimingEffect::SCROLL,
                                                    startValue,
                                                    MMM::TimingMetadata{},
                                                    curve);
    MMM::Config::EditorConfig config;
    // 显式开启实际滚动映射，不能用恒速显示让积分错误隐藏在配置后面。
    // 固定缩放使距离单位为每秒 500 像素，便于独立解析检查。
    config.visual.enableLinearScrollMapping = false;
    config.visual.timelineZoom              = 1;
    MMM::Logic::System::ScrollCache cache;
    cache.rebuild(registry, config, nullptr);
    // 空谱面指针选择保守参考 BPM，初始 BPM 比例仍为一。
    // 这里关注 SV 本身的积分，不混入红线重置 SV 的格式差异。
    MMM::Timing timing;
    // 领域时间戳是毫秒，运行时是秒；非零段首覆盖两套单位之间的转换。
    // 导出只展开独立时间点，不修改 registry 或原生段落定义。
    timing.m_timestamp             = 1000;
    timing.m_timingEffect          = MMM::TimingEffect::SCROLL;
    timing.m_timingEffectParameter = startValue;
    timing.m_interpolation         = curve;
    const auto output = MMM::sampleTimingInterpolations({ timing });
    bool       ok = check(output.size() == 10581, "high density output count");
    // 数量异常也继续收集其他失败，末点查询不依赖数组非空。
    // 输出模型的端值检查与运行时积分检查分别报告，便于定位退化层次。
    // 每个点到下一个点的中间时刻应使用前一个实际输出参数。
    // 独立 sin 真值保证导出和运行时不能共同用错函数而互相掩盖。
    // 最后短周期也纳入积分，不能将全段按相同点数重新均分。
    double integral = 0;
    for ( std::size_t index = 1; index < output.size(); ++index ) {
        // 时间戳已按真实落点排序，循环只扫描一次且不修改导出数组。
        // 累计量直接采用导出阶梯值，不能从被测 ScrollCache 段表反推。
        const auto&  previous = output[index - 1];
        const double begin    = previous.m_timestamp / 1000;
        const double end      = output[index].m_timestamp / 1000;
        const double query    = (begin + end) / 2;
        ok &=
            near(previous.m_timingEffectParameter, std::sin(100 * (begin - 1)));
        ok &= near(cache.getTimingStateAt(query).sv,
                   previous.m_timingEffectParameter);
        integral += (end - begin) * previous.m_timingEffectParameter;
    }
    // 段前默认流速为一，累计的初始 500 像素不属于正弦区间。
    // 左端阶梯积分允许有限截断误差，但 5000 Hz 不能仍按 240 Hz 计算。
    const double endTime = 1 + curve.m_duration;
    // 解析积分对完整时长求值，独立于两套采样器及其索引舍入。
    // 距离误差与状态误差分别检查，防止正确尾值掩盖中间漏采样。
    const double expectedIntegral =
        (1 - std::cos(100 * curve.m_duration)) / 100;
    ok &= check(near(cache.getAbsY(endTime), 500 + 500 * integral, 1e-6),
                "runtime distance matches high density output");
    ok &= check(near(cache.getAbsY(endTime), 500 + 500 * expectedIntegral, .11),
                "runtime distance retains high frequency integral");
    // 尾参数应精确保持用户指定时长，超过段尾也继承该值。
    ok &= check(near(cache.getTimingStateAt(endTime + .001).sv,
                     std::sin(100 * curve.m_duration)),
                "high density endpoint");
    ok &= check(registry.view<MMM::Logic::TimelineComponent>().size() == 1 &&
                    cache.getInterpolations().size() == 1,
                "high density retains one editable segment");
    // 内部数万次积分事件仅属于派生缓存，撤销和保存对象仍是完整段落。
    return check(ok, "high density state matches exported samples");
}
}  // namespace

/// @brief 汇总数学、事务和格式边界，任一失败使 CTest 报告失败。
/// 自定义 BPM 的积分和反解使用同一真实秒域定义。
/// 测试包含非零段落锚点，避免把局部 t 与绝对时间混用。
/// 改变导出密度不能改变编辑器的拍位映射。
/// 所有新预设均覆盖单位区间端点和解析积分。
/// 用例输出不写入源码资源目录。
int main()
{
    const bool curves      = testCurves();
    const bool actions     = testActionsAndCache();
    const bool beats       = testBeatCommandsAndCache();
    const bool boundaries  = testBoundariesAndBatch();
    const bool clipboard   = testClipboard();
    const bool storage     = testStorage();
    const bool highDensity = testHighDensityRuntime();
    return curves && actions && beats && boundaries && clipboard && storage &&
                   highDensity
               ? 0
               : 1;
}
