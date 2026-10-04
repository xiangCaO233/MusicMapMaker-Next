#include "mmm/timing/TimingInterpolation.h"

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "mmm/timing/Timing.h"
#include "mmm/timing/TimingFunction.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
/// @brief 使用发布构建也保留的断言，不让 NDEBUG 移除用户采样契约。
/// @param condition 实际结果和独立数学参考的比较。
/// @param message 失败所属的功能边界。
/// @return 原判据，场景可累积多个失败而不抛出异常。
bool check(bool condition, const char* message)
{
    if ( !condition ) XERROR("TimingBeatInterpolationTest: {}", message);
    return condition;
}
/// @brief 仅用于数学结果，外部文件量化使用单独较宽的容差。
/// @note NaN 和无穷不能通过此误差检查。
bool near(double left, double right, double tolerance = 1e-7)
{
    return std::abs(left - right) < tolerance;
}
/// @brief 创建独立红线；时间戳使用领域模型的毫秒单位。
/// @note 不把 Scroll 事件的附带 BPM 当成真实红线。
MMM::Timing bpm(double milliseconds, double value)
{
    MMM::Timing timing;
    timing.m_timestamp             = milliseconds;
    timing.m_bpm                   = value;
    timing.m_beat_length           = 60000 / value;
    timing.m_timingEffectParameter = value;
    return timing;
}
/// @brief 验证用户的 100 BPM 半拍案例，同时检查函数 t 的实际拍域含义。
/// @details 固定参考是 300 毫秒与每步增加 0.5，而非由被测 Hz 推导。
/// 时间模式仍允许独立设定密度，切换轴不能改变它的既有契约。
/// 分拍非法、未知 JSON 轴和超出预算均应在生成数组前拒绝。
bool testConstantAndValidation()
{
    // 输入的分拍采用默认 1/2，而不是预先写入 3.333 Hz。
    // 采样率必须从权威红线推导，测试不赋值覆盖派生结果。
    // 段尾恰好为两拍，含段首应得到五个事件。
    // 纯领域场景没有画布、锁或设备，失败不能归因于播放状态。
    MMM::TimingInterpolation curve;
    curve.m_duration = 1.2;
    curve.m_variable = MMM::TimingVariable::Beat;
    std::vector<MMM::Timing> lines{ bpm(0, 100) };
    if ( !MMM::bindTimingInterpolationBeatAxis(curve, 0, lines) )
        return check(false, "prepare half beat");
    bool ok = check(near(curve.m_samplesPerSecond, 10.0 / 3),
                    "100 BPM half beat is 3.333 Hz");
    ok &=
        check(near(curve.m_beatDuration, 2), "seconds converted to two beats");
    ok &= check(MMM::timingInterpolationSampleCount(curve) == 5,
                "half beat count includes endpoints");
    // 以绝对函数 1+t 区分秒域与拍域，避免归一曲线掩盖单位错误。
    // 时间比例四分之一对应 0.3 秒，即 0.5 拍。
    // 拍域正确值为 1.5；错误的秒域实现会得到 1.3。
    // 首值由真实编译结果推导，不能用手工端点伪装成功。
    double      start = 0;
    std::string error;
    if ( !MMM::setTimingInterpolationFunction(curve, "1+t", start, error) )
        return check(false, "beat function compiles");
    // t=0.5 拍时应输出 1.5，不是把 0.3 秒直接传给函数得到 1.3。
    ok &= check(near(MMM::evaluateTimingInterpolation(curve, start, .25), 1.5),
                "custom independent variable is beats");
    for ( std::size_t i = 0; i < 5; ++i )
        ok &=
            check(near(MMM::timingInterpolationSampleElapsed(curve, i), i * .3),
                  "every half beat is 300 ms");
    // 原生字段保留分拍与数学定义域，内部映射不应序列化成海量数据。
    // 序列化保存数学源式和定义域，不保存运行时共享对象地址。
    // 重新解析的对象尚无完整 BPM 数据，但定义本身应语义相等。
    // 测试比较整个定义，确保没有漏掉轴名、分拍或域长度。
    // 完整载入补齐映射的行为由后面的真实文件往返另行验证。
    nlohmann::json saved  = curve;
    const auto     loaded = MMM::readTimingInterpolation(saved);
    ok &= check(loaded && *loaded == curve, "beat definition roundtrip");
    ok &= check(
        !MMM::isValidTimingInterpolation(curve, MMM::TimingEffect::BPM, start),
        "red line rejects beat axis");
    // 各无效 JSON 都从同一成功保存对象独立复制。
    // 避免前一次损坏残留让后续测试被错误原因提前拒绝。
    // 零分母直接违反有理分拍定义，不允许进入除法。
    // 超大无符号整数先做范围检查，不能窄化后伪装成有效值。
    // 未知轴属于协议错误，不能偷偷回退时间模式。
    // 这些错误全部使用返回值，不通过异常处理不可信输入。
    auto damaged            = saved;
    damaged["beat_step"][1] = 0;
    ok &= check(!MMM::readTimingInterpolation(damaged),
                "zero subdivision rejected");
    damaged                 = saved;
    damaged["beat_step"][0] = std::numeric_limits<unsigned long long>::max();
    ok &= check(!MMM::readTimingInterpolation(damaged),
                "untrusted integer rejected before conversion");
    damaged             = saved;
    damaged["variable"] = "unknown";
    ok &=
        check(!MMM::readTimingInterpolation(damaged), "unknown axis rejected");
    // 两个拍长乘 65536 会超出单段写出的间隔预算。
    // 构建器应拒绝整个候选，而非截断输出或分配超大数组。
    // 上限不是用户可选采样率，必须由程序独立执行。
    // 此前成功缓存仍由值对象持有，失败不应该篡改其映射。
    curve.m_beatDenominator = 65536;
    ok &= check(!MMM::bindTimingInterpolationBeatAxis(curve, 0, lines),
                "excessive beat sampling rejected");
    // 老文件没有轴字段，应保持秒域 2 Hz 的固定时间间隔。
    // 旧文件只包含 duration、Hz 和预设，不带任何拍轴字段。
    // 选择 1.2 秒与 2 Hz，使尾部不足完整周期。
    // 第二个输出点仍为 0.5 秒，而不是平均划分总时长。
    // 旧语义与拍域新语义在同一程序中共存，不依赖数据迁移。
    MMM::TimingInterpolation time;
    time.m_duration         = 1.2;
    time.m_samplesPerSecond = 2;
    saved                   = time;
    const auto old          = MMM::readTimingInterpolation(saved);
    ok &= check(old && old->m_variable == MMM::TimingVariable::Time,
                "legacy interpolation remains time based");
    ok &= check(near(MMM::timingInterpolationSampleElapsed(time, 1), .5),
                "time sampling remains independent of BPM");
    // 毫秒下限属于放置范围，拍域长度不能错误套用同一个单位阈值。
    // 一毫秒在 1 BPM 下只有 1/60000 拍，绝对函数仍有合法正定义域。
    // 这个场景防止 UI 看似支持拍轴，但数学编译器仍按毫秒拒绝它。
    MMM::TimingInterpolation tiny;
    tiny.m_variable = MMM::TimingVariable::Beat;
    tiny.m_duration = .001;
    if ( !MMM::bindTimingInterpolationBeatAxis(tiny, 0, { bpm(0, 1) }) )
        return false;
    ok &= check(MMM::setTimingInterpolationFunction(tiny, "1+t", start, error),
                "short positive beat domain compiles");
    return ok;
}
/// @brief 在变 BPM 和连续红线下检查真实分拍落点，不能用平均 Hz 均分。
/// @details 100→200 BPM 的后半段半拍间隔是 150 毫秒，前半段是 300 毫秒。
/// 连续红线从 60→120 的解析拍数为 t+t²/4，可独立反解半拍。
/// 段首位于红线内部时仍须保留原积分相位，不能从段首重启 BPM。
/// 末尾不足一个完整分拍时额外保存精确段尾，不重复产生端点。
bool testChangingBpm()
{
    MMM::TimingInterpolation curve;
    curve.m_duration = 1.2;
    curve.m_variable = MMM::TimingVariable::Beat;
    curve.m_endValue = 3;
    std::vector<MMM::Timing> lines{ bpm(0, 100), bpm(600, 200) };
    if ( !MMM::bindTimingInterpolationBeatAxis(curve, 0, lines) )
        return check(false, "prepare BPM change");
    // 前 600 毫秒使用 100 BPM，随后使用 200 BPM。
    // 期望数组由每半拍 300/150 毫秒直接列出。
    // 平均 Hz 均分不能产生此数组，因此能发现错误的密度替代映射。
    // 时间恰好落在新红线起点时，前后区间共用唯一拍位边界。
    constexpr std::array expected{ 0., .3, .6, .75, .9, 1.05, 1.2 };
    bool                 ok =
        check(MMM::timingInterpolationSampleCount(curve) == expected.size(),
              "changed BPM sample count");
    for ( std::size_t i = 0; i < expected.size(); ++i )
        ok &= check(
            near(MMM::timingInterpolationSampleElapsed(curve, i), expected[i]),
            "subdivision follows each active BPM");
    // 整段时间比例的一半只积累三分之一拍长，线性预设也必须按拍域推进。
    ok &= check(near(MMM::evaluateTimingInterpolation(curve, 0, .5), 1),
                "preset curve uses beat progress");
    // 第二个场景只保留一条连续 BPM 段，与离散红线切换隔离。
    // 60→120 BPM 两秒给出 BPM(t)=60+30t。
    // 其积分拍数为 t+t²/4，不使用被测积分函数生成参考答案。
    // 半拍反解使用独立二次方程，验证采样周期与 BPM 插值密度无关。
    auto red                        = bpm(0, 60);
    red.m_interpolation             = MMM::TimingInterpolation{};
    red.m_interpolation->m_duration = 2;
    red.m_interpolation->m_endValue = 120;
    lines                           = { red };
    curve.m_duration                = 2;
    if ( !MMM::bindTimingInterpolationBeatAxis(curve, 0, lines) )
        return check(false, "prepare continuous red line");
    // 由二次方程独立求根，输出密度不参与这个参考值。
    ok &= check(near(MMM::timingInterpolationSampleElapsed(curve, 1),
                     -2 + std::sqrt(6.)),
                "half beat under continuous BPM");
    // 将非 BPM 段首移入同一红线曲线的中间。
    // 一至两秒的拍数为三拍减去 1.25 拍，即 1.75 拍。
    // 段首后的 BPM 从 90 开始，不能从 60 重新执行曲线。
    // 首个半拍反解为 -3+sqrt(11)，与全段的首次采样不同。
    // 1.75 拍不是半拍的整数倍，输出应保留第四个短间隔。
    // 最后一次落在原结束秒数，不补齐到两拍而越出范围。
    curve.m_duration = 1;
    if ( !MMM::bindTimingInterpolationBeatAxis(curve, 1, lines) )
        return check(false, "prepare clipped red line");
    ok &= check(near(curve.m_beatDuration, 1.75),
                "clipped curve retains BPM phase");
    ok &= check(near(MMM::timingInterpolationSampleElapsed(curve, 1),
                     -3 + std::sqrt(11.)),
                "clipped half beat inverse");
    ok &= check(MMM::timingInterpolationSampleCount(curve) == 5 &&
                    near(MMM::timingInterpolationSampleElapsed(curve, 4), 1),
                "partial subdivision retains exact end");
    return ok;
}
/// @brief 检查真实原生保存、外部展开和倍速副本，不只验证编辑器局部状态。
/// @param output 测试专用构建输出目录，禁止写用户文件或 tests/data。
/// @details 原生只存一个段落；倍速改变秒数而保持拍数、分拍和源表达式。
/// 重开后重新绑定 BPM 缓存，不依赖之前窗口或会话的任何指针。
/// 外部格式消费当前红线，源谱面的段落与函数不得被采样器修改。
bool testPersistenceAndSpeed(const std::filesystem::path& output)
{
    // 原生回环使用实际 BeatMap 公共入口，不单独模拟 JSON writer。
    // 谱面只有一条 BPM 和一条效果段，重新打开仍应只有两个 Timing。
    // 输出目录由 CTest 传入，文件操作不污染源码静态资源。
    // 参考 BPM 与实际红线一致，避免无红线回退影响持久化断言。
    MMM::BeatMap map;
    map.m_baseMapMetadata.preference_bpm = 100;
    map.m_baseMapMetadata.track_count    = 4;
    map.m_timings                        = { bpm(0, 100) };
    MMM::Timing scroll;
    scroll.m_timingEffect  = MMM::TimingEffect::SCROLL;
    scroll.m_interpolation = MMM::TimingInterpolation{};
    auto& curve            = *scroll.m_interpolation;
    curve.m_variable       = MMM::TimingVariable::Beat;
    curve.m_duration       = 1.2;
    if ( !MMM::bindTimingInterpolationBeatAxis(curve, 0, map.m_timings) )
        return false;
    std::string error;
    if ( !MMM::setTimingInterpolationFunction(
             curve, "1+t", scroll.m_timingEffectParameter, error) )
        return false;
    map.m_timings.push_back(scroll);
    map.sync();
    const auto native = output / "half_beat.mmm";
    if ( !map.saveToFile(native) ) return check(false, "native beat save");
    const auto loaded = MMM::BeatMap::loadFromFile(native);
    const auto found  = std::find_if(loaded.m_timings.begin(),
                                     loaded.m_timings.end(),
                                     [](const MMM::Timing& timing) {
                                        return timing.m_timingEffect ==
                                               MMM::TimingEffect::SCROLL;
                                     });
    if ( found == loaded.m_timings.end() || !found->m_interpolation )
        return check(false, "native beat reopened as one segment");
    bool ok = check(loaded.m_timings.size() == 2 &&
                        near(MMM::evaluateTimingInterpolation(
                                 *found->m_interpolation, 1, .25),
                             1.5),
                    "reopened beat function mapping");
    // 采样列表检查实际毫秒时间戳和参数，不以样本数量代替正确性。
    // 每个半拍输出值递增 0.5，时间递增 300 毫秒。
    // 原始列表的 BPM 与效果独立共存，同刻不能互相去重。
    // 展开仅发生在临时视图，来源仍保留可再编辑的完整段落。
    const auto  samples = MMM::sampleTimingInterpolations(loaded.m_timings);
    std::size_t index   = 0;
    for ( const auto& timing : samples ) {
        if ( timing.m_timingEffect != MMM::TimingEffect::SCROLL ) continue;
        ok &= check(near(timing.m_timestamp, index * 300.) &&
                        near(timing.m_timingEffectParameter, 1 + index * .5),
                    "external sampling actual time and value");
        ++index;
    }
    ok &= check(index == 5 && map.m_timings.back().m_interpolation.has_value(),
                "samples preserve source segment");
    // 倍速测试在真实模型变换入口进行，不只手工改一个 duration 字段。
    // 两倍速同时将红线乘二、全部秒数减半，拍数应该保持不变。
    // 拍域源式 1+t 不应改成 1+2t，否则会双重加速参数变化。
    // 分拍也不能乘速度，仍须每半拍输出一次。
    // 150 毫秒只是变速后的秒域落点，不是新的分拍配置。
    MMM::BeatmapSpeedTransformOptions options;
    options.speed = 2;
    const auto faster =
        MMM::BeatmapSpeedTransform::createSpeedVersion(map, options);
    if ( !faster.success ) return check(false, "beat speed version succeeds");
    const auto& scaled = *faster.beatmap.m_timings.back().m_interpolation;
    ok &= check(near(scaled.m_duration, .6) && near(scaled.m_beatDuration, 2),
                "speed changes seconds but retains beats");
    ok &= check(MMM::timingFunctionExpression(*scaled.m_function) == "1+t" &&
                    near(MMM::evaluateTimingInterpolation(scaled, 1, .25), 1.5),
                "speed keeps beat variable function");
    ok &= check(near(MMM::timingInterpolationSampleElapsed(scaled, 1), .15),
                "speed halves output time interval");
    // 保存发生在 BPM 编辑后，即使定义还持有旧缓存也必须使用新 BPM。
    // 有意不调用 sync，也不重新打开效果编辑窗口。
    // 当前定义仍持有原 100 BPM 缓存，保存必须重新取得权威红线。
    // 1.2 秒在 200 BPM 下应为四拍，定义域须随之重新编译。
    // 重新载入的函数终值和拍域映射一起更新，不能只更新 Hz 展示。
    // 这个场景覆盖外部协作或后续红线编辑带来的缓存过期。
    map.m_timings.front() = bpm(0, 200);
    if ( !map.saveToFile(native) )
        return check(false, "save rebinds modified BPM");
    const auto rebound = MMM::BeatMap::loadFromFile(native);
    ok &=
        check(near(rebound.m_timings.back().m_interpolation->m_beatDuration, 4),
              "native save uses current BPM domain");
    return ok;
}
}  // namespace
/// @brief 构建树中创建独立输出目录，所有失败以进程退出状态反馈。
/// @note 测试不访问设备、线程或用户配置，三个场景共享一个明确输出路径。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 2;
    const std::filesystem::path output(argv[1]);
    std::error_code             error;
    std::filesystem::create_directories(output, error);
    if ( error ) return 2;
    // 每个场景都执行，前一项失败不短路掩盖其他功能边界。
    // 数学契约、变 BPM 和持久化分别报告，不用整体成功状态替代细节。
    // 输出目录属于构建树，重新运行可以覆盖派生文件但不触碰原始资源。
    // 没有额外设备或网络依赖，跨平台结果只容许合理浮点误差。
    // 最终退出码聚合所有检查，使 CTest 能阻止不完整实现发布。
    // 测试读取的是当前编译实现，不依赖曾生成的 UI 图片。
    bool ok = testConstantAndValidation();
    ok &= testChangingBpm();
    ok &= testPersistenceAndSpeed(output);
    return ok ? 0 : 1;
}
