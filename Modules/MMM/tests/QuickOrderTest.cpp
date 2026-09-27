#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <fstream>
#include <nlohmann/json.hpp>

/// @file QuickOrderTest.cpp
/// @brief 人工诊断 osu! Timing 转换为 Malody effect 后的局部顺序。
///
/// 该程序不是 CTest 回归用例：输入与输出路径是开发现场探针，输出日志用于观察
/// 特定时间窗口内 BPM 和 Scroll 的相对顺序。若改为正式测试，应先参数化路径并
/// 为顺序建立确定断言。
/// 该探针会写 `/tmp/quick_test_order.mc`，不应在自动测试环境中并行调用。
/// 固定绝对输入路径不存在时，当前程序的输出不具备回归测试意义。
/// 保留它是为了复查最初顺序问题，不作为模块测试通过条件。

/// @brief 加载固定诊断谱面并打印目标时间窗口及 Malody 写出结果。
/// @return 探针执行完成时返回零；当前不对日志内容作自动判定。
int main()
{
    // 固定输入保留最初复现现场，仅适用于对应开发环境。
    auto bm = MMM::BeatMap::loadFromFile(
        "/home/xiang/Documents/coding/c_cpp/MusicMapMaker-Next/Modules/MMM/"
        "tests/data/"
        "DJ Grimoire - Astral Quantization (EternityHyper) [Quantization "
        "[SV]].osu");

    // 总数用于快速识别加载失败或时间点被大规模过滤。
    XINFO("Timing points: {}", bm.m_timings.size());

    // 目标窗口覆盖最初发现顺序异常的相邻 BPM 与 Scroll 事件。
    int idx = 0;
    for ( const auto& t : bm.m_timings ) {
        if ( t.m_timestamp >= 68730 && t.m_timestamp <= 68820 ) {
            // 将枚举缩写为日志标签，便于肉眼检查同时间事件的先后次序。
            const char* eff =
                (t.m_timingEffect == MMM::TimingEffect::BPM) ? "BPM" : "SCROLL";
            XINFO("  [{}] t={:.1f} {} bpm={:.1f} param={:.4f}",
                  idx,
                  t.m_timestamp,
                  eff,
                  t.m_bpm,
                  t.m_timingEffectParameter);
        }
        idx++;
    }

    // 保存前同步统一排序和派生索引，复现正常应用导出路径。
    bm.sync();
    bm.saveToFile("/tmp/quick_test_order.mc");

    // 直接查看写出的 JSON，避免重新加载时的排序再次掩盖磁盘顺序。
    std::ifstream  fs("/tmp/quick_test_order.mc");
    nlohmann::json j;
    fs >> j;

    // BPM=10000 是 osu! 效果转换使用的边界值，计数可揭示重复生成。
    int bpm10000_count = 0;
    for ( const auto& t : j["time"] ) {
        if ( std::abs(t["bpm"].get<double>() - 10000.0) < 1 ) bpm10000_count++;
    }
    XINFO("BPM=10000 in saved time: {}", bpm10000_count);
    XINFO("Effects in saved file: {}", j["effect"].size());

    // 限制日志到前十五个 effect，避免大型谱面淹没目标诊断信息。
    for ( int i = 0; i < std::min(15, (int)j["effect"].size()); i++ ) {
        auto& e    = j["effect"][i];
        auto  beat = e["beat"];
        // 三元拍位置转换为连续拍数，仅用于紧凑展示。
        double b = beat[0].get<double>() +
                   beat[1].get<double>() / beat[2].get<double>();
        XINFO("  eff[{}] beat={:.4f} {}", i, b, e.dump());
    }

    return 0;
}
