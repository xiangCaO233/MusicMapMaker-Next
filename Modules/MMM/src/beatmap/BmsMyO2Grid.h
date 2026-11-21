#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace MMM::Bms
{
/// @brief 输出小节及通道到分数位置、资源编号的有序映射。
using ExportRows =
    std::map<std::pair<int, int>, std::vector<std::pair<double, int>>>;

/// @brief 模拟 VB CInt 的中点向偶数舍入，不依赖当前浮点舍入模式。
/// @param value 待舍入的非负位置或距离。
/// @return 最近整数；恰好半格时取偶数。
inline double myO2Round(double value)
{
    // 不调用依赖环境舍入模式的 nearbyint，保持跨平台导出一致。
    // 输入在 writer 中已限制为非负小节位置，故不需要有符号分支。
    const double lower = std::floor(value);
    const double part  = value - lower;
    return lower + (part > .5 || (part == .5 && std::fmod(lower, 2) != 0));
}

/// @brief 按 iBMSC MyO2GridCheck 的建议量化导出行，不修改源模型。
/// @param rows 按各自小节长度换算为 0 到 1 分数的导出事件。
/// @return 小节未越界且玩家键位无量化冲突时为 true。
/// @note 参考 iBMSC 2d43b6f Form1.vb 的 GCD、MyO2GridCheck 和 GridAdjust。
/// @param scrollChannel 允许多行叠加的滚速通道。
/// @param cueChannel 写盘前的控制事件通道；指定时所有通道共用小节网格。
/// @note 默认 BMSGridLimit 为 1，D48/D64 每次累加都进行 CInt 舍入。
/// @note 用户明确启用吸附时才调用，原精度导出不改变格点位置。
/// @warning 低频导出路径，排序并重建输出行；不得用于播放或渲染循环。
inline bool quantizeMyO2Rows(ExportRows& rows, int scrollChannel = -1,
                             int cueChannel = -1)
{
    // 只有整份转换成功才替换事件表，失败时 writer 不打开目标文件。
    // 使用有序小节键，跨小节进位的事件可与后续行共同检查冲突。
    ExportRows result;
    // VB 逐项取整，距离统计仍按升序推进；兼容谱写盘共用小节网格，
    // 避免同一拍上的 BPM/STOP、01 和物件分别落在 48/64 的不同格。
    const auto distances = [](const auto& events) {
        double d64 = 0, d48 = 0;
        for ( const auto& [fraction, value] : events ) {
            const double position = fraction * 192;
            if ( position == 0 ) continue;
            d64 = myO2Round(d64 +
                            std::abs(position - myO2Round(position / 3) * 3));
            d48 = myO2Round(d48 +
                            std::abs(position - myO2Round(position / 4) * 4));
        }
        return std::pair{ d64, d48 };
    };
    // 各行先独立做舍入以尊重旧检查器的累计顺序，再汇总到当前小节。
    // 不在量化后重选网格，否则已进位的事件会反向影响下一小节的选择。
    std::map<int, std::pair<double, double>> totals;
    for ( auto& [key, events] : rows ) {
        std::stable_sort(events.begin(), events.end(), [](auto a, auto b) {
            return a.first < b.first;
        });
        if ( cueChannel < 0 ) continue;
        const auto [d64, d48] = distances(events);
        auto& total           = totals[key.first];
        total.first += d64;
        total.second += d48;
    }
    for ( const auto& [key, events] : rows ) {
        // 相等时选择 48 与 VB 的严格大于比较保持一致。
        const auto [d64, d48] =
            cueChannel < 0 ? distances(events) : totals.at(key.first);
        const double step = d48 > d64 ? 3 : 4;
        for ( const auto& [fraction, value] : events ) {
            // 即使整数距离列舍入为零，也必须落到实际网格上。
            // 亚 tick 残差会使原工具仍列出高分辨率行，不能用整数零代替吸附。
            const double position = myO2Round(fraction * 192 / step) * step;
            int          measure  = key.first;
            double       target   = position / 192;
            // 接近尾部的物件允许进位，不能夹到当前小节最后一个槽。
            // 进位目标是下一小节首位，因此仍属于两种网格的公共格点。
            if ( target >= 1 ) {
                ++measure;
                target = 0;
            }
            // 吸附到下一小节仍受标准三位小节号限制。
            if ( measure >= 1000 ) return false;
            auto& destination = result[{ measure, key.second }];
            // BGM、STOP 可叠加；玩家音符和长条端点不得被吞并。
            // BPM 同刻覆盖仍由序列化器按稳定顺序处理。
            if ( key.second != 1 && key.second != 8 && key.second != 9 &&
                 key.second != scrollChannel && key.second != cueChannel &&
                 std::any_of(
                     destination.begin(), destination.end(), [&](auto event) {
                         return std::abs(event.first - target) < 1e-12;
                     }) )
                return false;
            destination.emplace_back(target, value);
        }
    }
    // 移走旧表之前，所有原行都已完成吸附；错误不会留下半成品。
    // 写出阶段可单独选择行的文本长度，实际量化位置保持固定。
    rows = std::move(result);
    return true;
}
}  // namespace MMM::Bms
