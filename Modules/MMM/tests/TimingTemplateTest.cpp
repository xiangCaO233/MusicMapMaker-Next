#include "mmm/timing/TimingTemplate.h"

#include "log/colorful-log.h"
#include "mmm/timing/TimingFunction.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
/// @brief 失败诊断使用项目日志，返回状态汇总所有边界场景。
bool check(bool condition, const char* message)
{
    if ( !condition ) XERROR("TimingTemplateTest: {}", message);
    return condition;
}

/// @brief 拍轴反解容忍二分和浮点末位误差，不放宽分拍位置。
bool near(double a, double b)
{
    return std::abs(a - b) < 1e-7;
}

/// @brief 用毫秒领域模型构造测试点，不读取用户谱面或设备。
MMM::Timing point(double seconds, MMM::TimingEffect type, double value)
{
    MMM::Timing line;
    line.m_timestamp             = seconds * 1000;
    line.m_timingEffect          = type;
    line.m_timingEffectParameter = value;
    return line;
}

/// @brief 后一点作基准，验证提前 1/192 拍的 SV 可跨谱面复用。
// 参考坐标来自独立数学计算，不由被测代码生成断言目标。
// 两个 SV 参数差异明显，基准身份不能通过默认参数猜测。
// 目标恢复点在新红线之后半拍，提前点落在旧速区间。
// 两段拍长共同决定时间，恒定 BPM 换算不能通过这个场景。
// 秒轴对照则应保持来源毫秒间距，即使目标 BPM 改变。
// 这些场景只操作值模型，没有设备或窗口生命周期干扰。
bool testAnchorAndTempo()
{
    // 来源 100 BPM，每拍 600 ms；负分拍偏移不能被夹到零。
    // 选择恢复普通 SV 的第二点为基准，符合整排线的真实用法。
    const std::vector tempo{ point(0, MMM::TimingEffect::BPM, 100) };
    const std::vector selected{
        point(2 - .6 / 192, MMM::TimingEffect::SCROLL, 100),
        point(2, MMM::TimingEffect::SCROLL, 1)
    };
    auto draft =
        MMM::makeTimingTemplate(selected, 1, MMM::TimingVariable::Beat, tempo);
    if ( !check(draft.has_value(), "capture anchored subdivision") )
        return false;
    bool ok = check(near(draft->m_points[0].m_offset, -1.0 / 192),
                    "negative fractional offset");
    // 目标先 120 后 240 BPM，偏移跨红线时须逐段积分反解。
    // 不是把整个组乘基准 BPM，也不是把来源毫秒间距原样粘贴。
    // 三秒切换到 240 BPM，三点一二五秒对应新速半拍。
    // 再向前一拍须经过旧速半拍，该段耗时 250 ms。
    // 因此前置点最终位于二点七五秒。
    // 同时比较基准点，避免一组坐标被错误整体平移。
    // 只验证先后顺序无法发现恒定 BPM 的错误换算。
    const std::vector target{ point(0, MMM::TimingEffect::BPM, 120),
                              point(3, MMM::TimingEffect::BPM, 240) };
    draft->m_points[0].m_offset = -1;
    auto placed = MMM::placeTimingTemplate(*draft, 3.125, target);
    ok &= check(placed && near((*placed)[0].m_timestamp, 2750) &&
                    near((*placed)[1].m_timestamp, 3125),
                "cross tempo reverse mapping");
    // 任意基准切换平移坐标，不改变两点距离。
    // 零秒之前的实际落点拒绝整组，保留原模板用于再次放置。
    ok &= check(MMM::reanchorTimingTemplate(*draft, 0) &&
                    near(draft->m_points[1].m_offset, 1),
                "reanchor preserves distance");
    MMM::reanchorTimingTemplate(*draft, 1);
    ok &= check(!MMM::placeTimingTemplate(*draft, .1, target),
                "negative absolute placement rejected");
    // 时间模板保存秒差，复用不应受目标 BPM 影响。
    // 与上面的拍域结果形成同来源、不同定位依据的对照。
    // 断言直接使用 100 BPM 的每拍 600 ms。
    // 捕获失败时保留 expected 错误值，不解引用空结果。
    auto timed =
        MMM::makeTimingTemplate(selected, 1, MMM::TimingVariable::Time, tempo);
    auto seconds = timed ? MMM::placeTimingTemplate(*timed, 4, target)
                         : std::expected<std::vector<MMM::Timing>, std::string>(
                               std::unexpected("capture"));
    ok &= check(
        seconds && near((*seconds)[1].m_timestamp - (*seconds)[0].m_timestamp,
                        600.0 / 192),
        "timestamp offsets independent of target BPM");
    // 首条红线可晚于基准，首值向前外推而不是丢弃前置分拍。
    const std::vector delayed{ point(10, MMM::TimingEffect::BPM, 100) };
    draft->m_points[0].m_offset = -1.0 / 192;
    placed                      = MMM::placeTimingTemplate(*draft, 2, delayed);
    ok &= check(placed && near((*placed)[0].m_timestamp, (2 - .6 / 192) * 1000),
                "before first BPM extrapolation");
    return ok;
}

/// @brief 连续 BPM 积分反解、曲线保存和不可信数据使用生产入口验证。
// 连续直线 BPM 的累计拍数可按梯形面积直接计算。
// 拍域捕获和反解均使用生产实现，参考值独立计算。
// 段落应保存为一个点，不在任何模板步骤展开采样。
// 元数据检查使用真实来源属性表，验证复制不会丢导出属性。
// 读取经过文本 dump 和 parse，索引类型与实际配置文件一致。
// 每个坏例从有效数据重新复制，不借用先前污染字段。
// 模板规则和数学曲线规则必须分别验证。
bool testCurvesAndStorage()
{
    auto redline                        = point(0, MMM::TimingEffect::BPM, 120);
    redline.m_interpolation             = MMM::TimingInterpolation{};
    redline.m_interpolation->m_duration = 2;
    redline.m_interpolation->m_endValue = 240;
    const std::vector tempo{ redline };
    const std::vector selected{ point(0, MMM::TimingEffect::HS, 2),
                                point(2, MMM::TimingEffect::HS, .5) };
    auto              draft =
        MMM::makeTimingTemplate(selected, 0, MMM::TimingVariable::Beat, tempo);
    if ( !check(draft.has_value(), "capture on continuous BPM") ) return false;
    // 120→240 BPM 两秒累计六拍；反解应精确回到段尾。
    bool ok =
        check(near(draft->m_points[1].m_offset, 6), "integrated beat offset");
    auto placed = MMM::placeTimingTemplate(*draft, 0, tempo);
    ok &= check(placed && near((*placed)[1].m_timestamp, 2000),
                "continuous tempo inverse");
    auto curve                        = point(2, MMM::TimingEffect::SCROLL, 1);
    curve.m_interpolation             = MMM::TimingInterpolation{};
    curve.m_interpolation->m_duration = 1;
    curve.m_interpolation->m_endValue = 2;
    curve.m_interpolation->m_curve    = MMM::TimingCurve::Bezier;
    curve.m_metadata.timing_properties[MMM::TimingMetadataType::OSU]["volume"] =
        "42";
    const std::vector curves{ curve };
    const std::vector source{ point(0, MMM::TimingEffect::BPM, 120) };
    auto              saved =
        MMM::makeTimingTemplate(curves, 0, MMM::TimingVariable::Beat, source);
    if ( !check(saved.has_value(), "capture interpolation as one point") )
        return false;
    nlohmann::json serialized = *saved;
    auto           loaded     = MMM::readTimingTemplate(
        nlohmann::json::parse(serialized.dump(), nullptr, false));
    ok &= check(loaded && loaded->m_points.size() == 1 &&
                    loaded->m_points[0].m_timing.m_interpolation &&
                    loaded->m_points[0].m_timing.m_metadata.timing_properties ==
                        curve.m_metadata.timing_properties,
                "template definition and metadata roundtrip");
    // 来源两拍持续一秒，目标两拍仅持续半秒。
    // 段落范围改变但贝塞尔形状类型保持不变。
    // 这个断言区分按拍复用与复制来源秒时长。
    // 输出仍是一条带段落定义的 Timing。
    // 若保存成许多普通点，此处的可选段落检查会失败。
    const std::vector faster{ point(0, MMM::TimingEffect::BPM, 240) };
    placed = loaded ? MMM::placeTimingTemplate(*loaded, 4, faster)
                    : std::expected<std::vector<MMM::Timing>, std::string>(
                          std::unexpected("load"));
    ok &= check(
        placed && near((*placed)[0].m_interpolation->m_duration, .5) &&
            (*placed)[0].m_interpolation->m_curve == MMM::TimingCurve::Bezier,
        "beat template rescales segment range");
    // 保存的是定义而非运行时缓存；损坏字段不能回退成别的类型。
    // 分别从有效数据构造每个坏例，确保拒绝原因不依赖先前污染。
    // 这些坏例仍是合法 JSON，不能仅靠 JSON 语法错误拒绝。
    // 未知轴不能回退为默认时间轴。
    // 超大索引须先检查数组范围，再窄化成本机索引。
    // 未知效果不能沿用旧格式的 Scroll 回退行为。
    // 未知来源不能转换成未定义元数据枚举。
    // 每次检查都运行完整领域读取器。
    auto broken        = serialized;
    broken["variable"] = "unknown";
    ok &= check(!MMM::readTimingTemplate(broken), "unknown axis rejected");
    broken           = serialized;
    broken["anchor"] = std::numeric_limits<std::uint64_t>::max();
    ok &= check(!MMM::readTimingTemplate(broken),
                "anchor checked before narrowing");
    broken                        = serialized;
    broken["points"][0]["effect"] = "other";
    ok &= check(!MMM::readTimingTemplate(broken), "unknown effect rejected");
    broken                          = serialized;
    broken["points"][0]["metadata"] = { { 90, { { "invalid", "data" } } } };
    ok &= check(!MMM::readTimingTemplate(broken),
                "unknown metadata source rejected");
    // BPM 可以形成秒域模板，但任何含 BPM 的拍域组必须整体拒绝。
    // 含 BPM 的拍域定义会形成定位循环，必须在领域层拒绝。
    // 同样输入使用秒域应该成功，不能误伤整个红线类型。
    // 两次调用只改变模板定位轴，不修改曲线数值。
    // 这证明限制来自定位规则，而非曲线恰好不合法。
    // 测试不会为失败路径生成谱面文件。
    const std::vector bpm{ redline };
    ok &= check(
        !MMM::makeTimingTemplate(bpm, 0, MMM::TimingVariable::Beat, tempo),
        "BPM template rejects beat axis");
    ok &=
        check(MMM::makeTimingTemplate(bpm, 0, MMM::TimingVariable::Time, tempo)
                  .has_value(),
              "BPM template supports time axis");
    return ok;
}
/// @brief 自定义函数随段长缩放，混合秒域组中的新 BPM 参与拍轴绑定。
/// @note 秒函数应保留归一化形状，拍函数应保留实际拍数语义。
/// @note 使用公共表达式编译器，不伪造函数缓存或手工设置终值。
bool testFunctionRebinding()
{
    auto              sourceBpm = point(0, MMM::TimingEffect::BPM, 120);
    const std::vector source{ sourceBpm };
    auto              line           = point(2, MMM::TimingEffect::SCROLL, 1);
    line.m_interpolation             = MMM::TimingInterpolation{};
    line.m_interpolation->m_duration = 1;
    std::string error;
    // 一秒域公式首值一、终值二；目标倍速时应该压缩相同形状。
    // 源函数对象不可变，复制模板不应该改写来源曲线或表达式。
    // 编译失败须直接终止本场景，不能让空函数缓存参与断言。
    if ( !MMM::setTimingInterpolationFunction(*line.m_interpolation,
                                              "1+t^2",
                                              line.m_timingEffectParameter,
                                              error) )
        return false;
    // 来源一秒段尾转换为两拍，放到新速后应保持两拍范围。
    // 目标基准四秒与来源两秒不同，防止原地复制掩盖定位错误。
    // 源时间戳不是最终输出位置，最终只依据模板偏移求解。
    // 公式尺度按原时长除新时长，压缩区间需要增大变量倍率。
    // 同一个源公式对象在目标转换后仍保持原有定义域。
    auto draft = MMM::makeTimingTemplate(
        std::vector{ line }, 0, MMM::TimingVariable::Beat, source);
    if ( !draft ) return false;
    const std::vector faster{ point(0, MMM::TimingEffect::BPM, 240) };
    auto              placed = MMM::placeTimingTemplate(*draft, 4, faster);
    // 两拍在目标仅半秒，四分之一秒对应原函数的中点。
    // 若只改 duration 而不重写变量，中点会误变成一点零六二五。
    bool ok =
        check(placed && near((*placed)[0].m_interpolation->m_duration, .5) &&
                  near(MMM::evaluateTimingInterpolation(
                           *(*placed)[0].m_interpolation, 1, .5),
                       1.25),
              "custom time function rescaled with segment");
    // 按拍公式先绑定来源红线，定义域为两拍。
    // 混合组使用秒轴，新增红线固定在基准时间，不参与定位循环。
    // 拍公式和采样密度则必须依据同组即将写入的新红线重建。
    line.m_interpolation             = MMM::TimingInterpolation{};
    line.m_interpolation->m_duration = 1;
    line.m_interpolation->m_variable = MMM::TimingVariable::Beat;
    line.m_timingEffectParameter     = 1;
    if ( !MMM::bindTimingInterpolationBeatAxis(
             *line.m_interpolation, 2, source) ||
         !MMM::setTimingInterpolationFunction(*line.m_interpolation,
                                              "1+t^2",
                                              line.m_timingEffectParameter,
                                              error) )
        return false;
    auto newBpm = point(2, MMM::TimingEffect::BPM, 240);
    auto mixed  = MMM::makeTimingTemplate(
        std::vector{ newBpm, line }, 0, MMM::TimingVariable::Time, source);
    if ( !mixed ) return false;
    // 保存只包含稳定定义，往返后没有来源运行时拍轴缓存。
    // 重新绑定目标应恢复四拍域与十七的终值。
    auto restored = MMM::readTimingTemplate(
        nlohmann::json::parse(nlohmann::json(*mixed).dump(), nullptr, false));
    // 文件读取器会重新编译函数，但不会建立目标 BPM 缓存。
    // 只恢复函数定义域不代表目标节拍映射已经准备完成。
    // place 必须在同组红线位置确定后重新绑定全部拍域段。
    // 所有条目转换成功才允许返回完整放置列表。
    if ( !restored ) return false;
    placed = MMM::placeTimingTemplate(*restored, 4, source);
    ok &=
        check(placed && near((*placed)[1].m_interpolation->m_beatDuration, 4) &&
                  near((*placed)[1].m_interpolation->m_endValue, 17),
              "mixed template uses candidate BPM");
    // 无元数据的普通点也必须能保存与读取，空属性表是正常定义。
    // 不能因为只测了带属性曲线而漏掉全新模板的默认保存路径。
    auto plain = MMM::makeTimingTemplate(
        std::vector{ sourceBpm }, 0, MMM::TimingVariable::Time, source);
    ok &= check(
        plain && MMM::readTimingTemplate(nlohmann::json(*plain)).has_value(),
        "empty metadata roundtrip");
    return ok;
}

/// @brief 文件添加支持单模板和整库，损坏后项不能产生部分导入。
/// @note 候选内容与个人库没有共享所有权，失败后源 JSON 保持原样。
bool testImportBundle()
{
    MMM::TimingTemplate first;
    first.m_name = "提前 SV";
    first.m_points.push_back({});
    first.m_points[0].m_timing = point(0, MMM::TimingEffect::SCROLL, 100);
    auto second                = first;
    second.m_name              = "恢复 SV";
    second.m_points[0].m_timing.m_timingEffectParameter = 1;
    // 实际文件会经过文本解析；解析后的 anchor 必须保留无符号类型。
    // 不直接把库对象的内部数字类型当作文件读取场景。
    const auto decode = [](const nlohmann::json& value) {
        return nlohmann::json::parse(value.dump(), nullptr, false);
    };
    // 测试定义的字段通过实际序列化产生，不维护第二份格式构造器。
    // 单文件读取也验证返回值数量，避免被错当成外层模板库。
    auto single = MMM::readTimingTemplateBundle(decode(first));
    bool ok     = check(
        single && single->size() == 1 && (*single)[0].m_name == first.m_name,
        "single template file");
    nlohmann::json bundle   = { { "version", 1 },
                                { "templates", { first, second } } };
    auto           imported = MMM::readTimingTemplateBundle(decode(bundle));
    // 多模板保留不同效果参数，不能只校验名称或导入数量。
    // 与目标库合并属于确认步骤，读取阶段不改变任何库。
    ok &= check(
        imported && imported->size() == 2 &&
            (*imported)[1].m_points[0].m_timing.m_timingEffectParameter == 1,
        "multiple template file");
    // 重复名称必须明确拒绝，不允许最后一项静默覆盖第一项。
    // 两项内容相同不是重点；名称在个人库中的唯一键不能发生歧义。
    // 即使参数不同，同名文件项也不得依赖数组顺序决定覆盖结果。
    bundle["templates"][1] = first;
    ok &= check(!MMM::readTimingTemplateBundle(decode(bundle)),
                "reject duplicate names in file");
    // 文件前项有效而后项无效时，也必须拒绝整个文件。
    // 后项的未知版本不能被解释成一个空模板。
    bundle["templates"][1]            = second;
    bundle["templates"][1]["version"] = 99;
    ok &= check(!MMM::readTimingTemplateBundle(decode(bundle)),
                "reject partially valid file");
    // 空库没有可确认添加的对象，应留在来源选择而非假报添加成功。
    // 读取失败使用 expected，不需要通过日志文本判断结果。
    bundle["templates"] = nlohmann::json::array();
    ok &= check(!MMM::readTimingTemplateBundle(bundle), "reject empty library");
    // 数量预算在外层执行，不因每个子定义单独有效而放宽。
    // 这个场景先命中数量预算，不能为了查重而先遍历所有子定义。
    // 数量限制和每模板条目预算是两个独立约束。
    bundle["templates"] = std::vector<MMM::TimingTemplate>(257, first);
    ok &= check(!MMM::readTimingTemplateBundle(decode(bundle)),
                "reject oversized library");
    ok &= check(!MMM::readTimingTemplateBundle(nlohmann::json::array()),
                "reject unversioned array");
    // 不支持的外层版本不能被子定义当前版本掩盖。
    bundle["templates"] = { first };
    bundle["version"]   = 99;
    ok &= check(!MMM::readTimingTemplateBundle(decode(bundle)),
                "reject library version");
    return ok;
}
}  // namespace

/// @brief 纯领域回归不访问文件系统、音频设备和用户配置。
/// @note 各测试返回值在退出前汇总，不把日志输出当成成功判据。
/// @note 测试不创建模板库，序列化往返仅经过内存文本。
/// @note 数学误差容差独立于 UI 显示的小数位数。
/// @note 跨红线与连续红线分别覆盖离散和连续拍轴。
/// @note 含 BPM 模板限制同时检查成功秒域与失败拍域。
/// @note 提前分拍检查实际时间，不仅检查偏移字段。
/// @note 所有错误用 expected 表达，不依赖异常机制。
int main()
{
    // 先验证位置映射再验证保存和函数，最终退出码汇总全部约束。
    // 不使用 UI 显示的圆整数字作为精度参考。
    // 每个场景在独立值容器中运行，避免修改前一模板影响下一场景。
    const bool anchors = testAnchorAndTempo();
    const bool curves  = testCurvesAndStorage();
    return anchors && curves && testFunctionRebinding() && testImportBundle()
               ? 0
               : 1;
}
