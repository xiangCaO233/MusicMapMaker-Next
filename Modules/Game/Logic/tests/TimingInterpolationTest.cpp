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
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

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
    // 枚举连续范围覆盖公开的七种函数，新增类型必须同步更新断言范围。
    // 起终点必须精确到约定容差，中间曲率可以不同。
    // 首尾一致是用户编辑端值的契约，不只是某一种缓动函数的特征。
    // 每种形状随后独立检查累计拍数的反解闭环。
    // BPM 两端为正，因此拍数严格单调，理论上有唯一的时间解。
    // 查询段前与段后验证的是外推契约，而不是只在合法区间内自洽。
    for ( int type = 0; type <= static_cast<int>(MMM::TimingCurve::Bezier);
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
    // 改变输出密度不得改变编辑器中缓存积分的形状或节拍相位。
    // 两个缓存都从同一实体状态建立，唯一差异是写出采样密度。
    // 比较段内投影位置，可以发现把输出密度错误用于运行时积分的回归。
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
}  // namespace

/// @brief 汇总数学、事务和格式边界，任一失败使 CTest 报告失败。
int main()
{
    const bool curves     = testCurves();
    const bool actions    = testActionsAndCache();
    const bool boundaries = testBoundariesAndBatch();
    const bool clipboard  = testClipboard();
    const bool storage    = testStorage();
    return curves && actions && boundaries && clipboard && storage ? 0 : 1;
}
