#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/TimingInterpolationAlgorithms.h"

#include "log/colorful-log.h"
#include <array>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace
{
/// @file MalodyInterpolationPolylineTest.cpp
/// @brief 复现函数插值导出时原生谱面的折线子物件被重复写出。
///
/// 原生 MMM 的物件由 deque 拥有，Polyline 另存对这些对象的引用。
/// 无曲线导出直接读取原模型，因此引用和实际子物件地址一致。
/// 含曲线的导出先复制 NoteData，再将连续函数展开成普通 timing。
/// 默认复制 reference_wrapper 不会将其改绑到新 deque 的元素。
/// 保存器若比较地址，副本中的 Hold/Flick 就会被误判为顶层物件。
/// 不能仅断言 seg 存在：旧实现有 seg，但还叠着独立长条和滑键。
///
/// 测试将参考 BPM、首红线 BPM 和相位设置为相同值。
/// note[] 的变化因此不能归因于参考 BPM 初始化或首拍补偿。
/// 无曲线基线明确检查四个顶层物件和三个折线端点。
/// 后续所有 SV 变体必须产生完全相同的 note[]。
/// 比较针对游戏实际消费的文件协议，不检查实现内部的地址表。
/// 源模型中的子物件标记也保留，覆盖持久化重开后的合法结构。
///
/// 时间轴矩阵包括秒域和分拍域，两者走不同的导出准备分支。
/// 位置矩阵包括折线之前、折线内部和折线之后。
/// 曲线矩阵包括预设线性和自定义表达式，防止遗漏另一种入口。
/// 198 Hz 或 1/198 拍用于生成非常规小数位置。
/// 效果样本必须确实展开，不能通过丢弃曲线让物件检查通过。
/// 相同红线下只改变 SV，音符拍位和结构都必须保持不变。
///
/// 每组先真实保存 MMM 并重开，再执行内存文档与磁盘 MC 导出。
/// 重开排除尚未持久化的编辑器状态，复现用户打包前的来源形态。
/// 同一来源重复导出用于暴露临时副本修改原模型的副作用。
/// MC 回读检查顶层数量与折线数量，和 JSON 检查互相补充。
/// 输出全部位于构建树，不读取真实项目或依赖音频设备。
/// MCZ 归档及项目资源入口另由 Logic 层集成测试覆盖。
///
/// 折线按 1→2→0→1 转向，最后一次回到根轨道。
/// 返回根轨的 seg.x 可以省略，但整个端点仍必须保留。
/// 120 BPM 下端点距根半拍、一拍、一拍半，参考结果完全确定。
/// 普通点击、独立长条与独立滑键同时存在，防止过度过滤。
/// 独立长条只有一个 seg，不应被误计为三段折线。
/// 物件按时间排列，使协议根节点在本测试中有稳定位置。
///
/// 断言不要求分拍落在较粗网格，不能靠降低插值密度修复问题。
/// 曲线定义在导出后仍应保留，而不是用采样数组替换原生段落。
/// 两次文档导出应完全一致，不允许第一次成功、第二次丢失引用。
/// 磁盘回读必须仍只有四个顶层玩家物件，不能只核对最终落轨。
/// 任何组合失败都返回非零，不以其他组合成功抵扣回归。
/// 文件创建和解析错误直接失败，不使用旧文件假装本次输出成功。
/// 所有 expected beat 和 x 均由已知谱面几何给定，而非由写出器反算。
/// 不依赖图片或人工目视判断，便于各平台 CI 重复验证。

/// @brief 失败时保留具体场景，并让发布构建也执行全部断言。
/// @param value 实际协议结果，不从复制算法反推期望。
/// @param message 区分失败的物件数量、转向或拍位契约。
/// @return 原判据，允许同一场景继续报告其余差异。
bool check(bool value, const char* message)
{
    if ( !value ) XERROR("MalodyInterpolationPolylineTest: {}", message);
    return value;
}

/// @brief 创建普通物件与三段折线共存的四轨谱面，不依赖用户项目。
/// @details 每次转向有一个 Hold 和一个 Flick；MC 应只写三个 seg。
/// 独立普通物件分别检查复制没有吞掉合法的点击、长条或滑键。
/// @return 已同步顶层索引、拥有全部折线子物件的独立模型。
MMM::BeatMap makeMap()
{
    MMM::BeatMap map;
    map.m_baseMapMetadata.track_count                                   = 4;
    map.m_baseMapMetadata.preference_bpm                                = 120;
    map.m_metadata.map_properties[MMM::MapMetadataType::MALODY]["mode"] = "7";
    // 首红线位于零且等于参考 BPM，排除首拍补偿对本例的干扰。
    MMM::Timing bpm;
    bpm.m_bpm                   = 120;
    bpm.m_beat_length           = 500;
    bpm.m_timingEffectParameter = 120;
    map.m_timings.push_back(bpm);
    auto& poly       = map.m_noteData.polylines.emplace_back();
    poly.m_timestamp = 1000;
    poly.m_track     = 1;
    constexpr std::array<int, 4> tracks{ 1, 2, 0, 1 };
    for ( std::size_t i = 0; i < 3; ++i ) {
        // 子物件由 NoteData 拥有，折线只借用；复制必须保持这一关系。
        auto& hold       = map.m_noteData.holds.emplace_back();
        hold.m_timestamp = 1000 + 250 * i;
        hold.m_track     = tracks[i];
        hold.m_duration  = 250;
        hold.m_isSubNote = true;
        poly.m_subNotes.emplace_back(hold);
        poly.m_subHolds.emplace_back(hold);
        auto& flick       = map.m_noteData.flicks.emplace_back();
        flick.m_timestamp = 1250 + 250 * i;
        flick.m_track     = tracks[i];
        flick.m_dtrack    = tracks[i + 1] - tracks[i];
        flick.m_isSubNote = true;
        poly.m_subNotes.emplace_back(flick);
        poly.m_subFlicks.emplace_back(flick);
    }
    // 这三个顶层物件不能因修复重复子物件而被误过滤。
    auto& tap         = map.m_noteData.notes.emplace_back();
    tap.m_timestamp   = 3000;
    tap.m_track       = 0;
    auto& hold        = map.m_noteData.holds.emplace_back();
    hold.m_timestamp  = 4000;
    hold.m_duration   = 500;
    hold.m_track      = 2;
    auto& flick       = map.m_noteData.flicks.emplace_back();
    flick.m_timestamp = 5000;
    flick.m_track     = 1;
    flick.m_dtrack    = 1;
    map.sync();
    return map;
}

/// @brief 独立检查游戏消费的顶层数量、相对拍位及横移终点。
/// @note 不能只用 MMM 重新加载器自洽，因为重复物件会被其完整读回。
/// @param doc 正式导出入口生成的完整 MC 文档。
/// @return 数量、各段时间和横移均符合独立参考时为真。
bool checkDocument(const nlohmann::json& doc)
{
    const auto& notes = doc.at("note");
    if ( !check(notes.size() == 4,
                "exactly four player roots; no loose children") )
        return false;
    // 120 BPM 下三个端点分别距根半拍、一拍、一拍半。
    // 坐标偏移按四轨中心差给定，避免复用写出器公式当参考。
    const auto& poly = notes[0];
    if ( !check(poly.contains("seg") && poly["seg"].size() == 3,
                "one connected three-segment polyline") )
        return false;
    bool ok =
        check(poly["beat"] == nlohmann::json::array({ 2, 0, 1 }), "root beat");
    constexpr std::array<int, 3>        offsets{ 64, -64, 0 };
    const std::array<nlohmann::json, 3> beats{
        nlohmann::json::array({ 0, 1, 2 }),
        nlohmann::json::array({ 1, 0, 1 }),
        nlohmann::json::array({ 1, 1, 2 })
    };
    for ( std::size_t i = 0; i < 3; ++i ) {
        ok &=
            check(poly["seg"][i]["beat"] == beats[i], "segment beat retained");
        ok &=
            check(poly["seg"][i].value("x", 0) == offsets[i], "turn retained");
    }
    return ok;
}

/// @brief 覆盖秒域、拍域、自定义函数及不同插值位置的原生保存重开。
/// @details 基线与各 SV 场景的 note[] 必须完全一致，密度不改变物件结构。
/// 所有文件写到构建树，重复导出还验证来源模型没有被采样副本污染。
/// @param root 当前用例独占的输出路径，产物可供失败后人工检查。
/// @return 任意变体的结构、密度或来源不变性检查失败时为假。
bool testExports(const std::filesystem::path& root)
{
    auto           baseline = makeMap();
    nlohmann::json reference;
    double         leadIn = 0;
    if ( !baseline.buildMalodyExportDocument(reference, leadIn) ||
         !checkDocument(reference) )
        return false;
    bool ok = true;
    for ( auto variable :
          { MMM::TimingVariable::Time, MMM::TimingVariable::Beat } ) {
        for ( double position : { 0.0, 1250.0, 6000.0 } ) {
            for ( bool custom : { false, true } ) {
                auto        map = makeMap();
                MMM::Timing sv;
                sv.m_timestamp             = position;
                sv.m_timingEffect          = MMM::TimingEffect::SCROLL;
                sv.m_timingEffectParameter = 1;
                sv.m_interpolation         = MMM::TimingInterpolation{};
                auto& curve                = *sv.m_interpolation;
                curve.m_variable           = variable;
                curve.m_duration           = 1;
                curve.m_endValue           = 2;
                // 198 Hz 故意生成很细且不常见的分拍，但不能拆开折线。
                curve.m_samplesPerSecond = 198;
                curve.m_beatDenominator  = 198;
                if ( variable == MMM::TimingVariable::Beat &&
                     !MMM::bindTimingInterpolationBeatAxis(
                         curve, position / 1000, map.m_timings) )
                    return false;
                if ( custom ) {
                    std::string error;
                    if ( !MMM::setTimingInterpolationFunction(
                             curve, "1+t", sv.m_timingEffectParameter, error) )
                        return false;
                }
                map.m_timings.push_back(sv);
                map.sync();
                const std::string name =
                    std::to_string(static_cast<int>(variable)) + "_" +
                    std::to_string(static_cast<int>(position)) + "_" +
                    std::to_string(custom);
                const auto native = root / (name + ".mmm");
                // 按用户的真实路径先存 MMM，再从磁盘重开导出 MC。
                if ( !map.saveToFile(native) )
                    return check(false, "save native fixture");
                auto loaded = MMM::BeatMap::loadFromFile(native);
                for ( int repeat = 0; repeat < 2; ++repeat ) {
                    nlohmann::json doc;
                    if ( !loaded.buildMalodyExportDocument(doc, leadIn) )
                        return check(false, "build interpolated MC document");
                    ok &= checkDocument(doc);
                    ok &= check(doc["note"] == reference["note"],
                                "SV leaves all notes unchanged");
                    ok &= check(
                        doc.contains("effect") && doc["effect"].size() > 100,
                        "dense timing is really expanded");
                }
                const auto mc = root / (name + ".mc");
                if ( !loaded.saveToFile(mc) )
                    return check(false, "save MC fixture");
                auto roundtrip = MMM::BeatMap::loadFromFile(mc);
                ok &= check(roundtrip.m_allNotes.size() == 4 &&
                                roundtrip.m_noteData.polylines.size() == 1,
                            "MC roundtrip has no extra loose notes");
                ok &= check(loaded.m_timings.size() == 2 &&
                                loaded.m_timings[1].m_interpolation.has_value(),
                            "source curve preserved after repeated export");
            }
        }
    }
    return ok;
}
}  // namespace

/// @brief 通过显式构建输出目录运行原生到 MC 的完整组合矩阵。
/// @param argc 必须带一个构建输出目录。
/// @param argv CTest 为本例指定的隔离路径。
/// @return 目录创建或协议验证失败时返回非零。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    XLogger::init("MalodyInterpolationPolylineTest");
    std::error_code error;
    std::filesystem::create_directories(argv[1], error);
    return !error && testExports(argv[1]) ? 0 : 1;
}
