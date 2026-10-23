#pragma once

#include "mmm/beatmap/BeatFraction.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>

namespace MMM::Internal
{
/// @file MalodyInitialTiming.h
/// @brief MC 首拍参考锚点的纯文档变换，不访问音频设备或项目资源表。
///
/// 原导出器已完成 delay 包装与物件类型降级；此处只调整首段拍轴。
/// 输出参数是必须追加的音频静音毫秒数，由应用层与 MC 一起提交。
/// 同参考 BPM 的回卷锚点可直接承载单位 Scroll，无须重写后续时序。
/// 原零点被用户占用时，单独增加参考拍，为原红绿线完整保留位置关系。
/// 对不同 BPM 的前导段，按绝对时间重新映射拍位，后续 BPM 段仍保持间距。
/// 主音频文件补头与普通物件平移分别承担音乐和谱面的同一段时间位移。
/// @brief 从已生成的标准三元组读取拍位，不接受格式私有字段代替结构。
/// @param beat 当前导出器生成的合法 beat 数组。
/// @return 三元组对应的连续拍数。
inline double initialTimingBeatValue(const nlohmann::json& beat)
{
    return beat[0].get<double>() +
           beat[1].get<double>() / beat[2].get<double>();
}

/// @brief 平移绝对拍位，继续使用导出器统一的有限分母拟合规则。
/// @param beat 需要平移的绝对拍位；seg 内的相对拍位不能传入。
/// @param delta 统一拍轴位移，不是毫秒偏移。
inline void shiftInitialTimingBeat(nlohmann::json& beat, double delta)
{
    const auto fitted =
        fitMalodyBeatFraction(initialTimingBeatValue(beat) + delta);
    beat = nlohmann::json::array(
        { fitted.beatIndex, fitted.numerator, fitted.denominator });
}

/// @brief 为最终 MC 拍轴建立参考 BPM 与单位 Scroll 的首个配对锚点。
/// @param document 已经完成原有回卷、物件和自动采样序列化的文档。
/// @param referenceBpm 谱面参考 BPM，无效时使用原首红线 BPM。
/// @param sourceFirstTime 用户首红线的原始时间，区分已有零点与回卷零点。
/// @param rewound 原导出器是否生成了回卷首红线。
/// @param userScrollAtFirst 用户首红线是否带有同刻的显式 Scroll。
/// @param pairedMainNode 零点主音轨在 note 数组中的索引，没有配对时为空。
/// @param primaryMainNode 主音轨节点，可带有独立的 offset 而不满足首拍配对。
/// @param primaryMainTimestamp 主音轨原始毫秒锚点，不含独立 offset。
/// @param audioLeadInMilliseconds 接收必须加到主音频前面的静音长度。
/// @return BPM 和拍轴计算均有限时返回 true。
/// @note 后续红绿线只接受统一拍轴变换，不添加或覆盖其效果参数。
/// @warning 低频导出路径：仅遍历已生成的 JSON，不访问文件或共享模型。
inline bool normalizeMalodyInitialTiming(
    nlohmann::json& document, double referenceBpm, double sourceFirstTime,
    bool rewound, bool userScrollAtFirst,
    std::optional<std::size_t> pairedMainNode,
    std::optional<std::size_t> primaryMainNode, double primaryMainTimestamp,
    double& audioLeadInMilliseconds)
{
    audioLeadInMilliseconds = 0;
    const auto timesIt      = document.find("time");
    if ( timesIt == document.end() || !timesIt->is_array() || timesIt->empty() )
        return true;
    auto& times = *timesIt;
    // 此处输入来自正式构建器，三元组结构已确定；无 BPM 的文档保持原状。
    // BPM 非有限或非正数时拒绝导出，不能将错误数值写成初始化红线。
    // BPM 容器允许来源顺序不同；稳定排序不改变同拍红线的先后关系。
    std::stable_sort(
        times.begin(), times.end(), [](const auto& lhs, const auto& rhs) {
            return initialTimingBeatValue(lhs["beat"]) <
                   initialTimingBeatValue(rhs["beat"]);
        });
    auto&        first    = times.front();
    const double firstBpm = first["bpm"].get<double>();
    if ( !std::isfinite(firstBpm) || !(firstBpm > 0) ) return false;
    if ( !std::isfinite(referenceBpm) || !(referenceBpm > 0) )
        referenceBpm = firstBpm;
    const bool sameBpm    = std::abs(firstBpm - referenceBpm) <= 1.0e-6;
    const bool userAtZero = std::abs(sourceFirstTime) <= 1.0e-6;
    // 判断依据是用户原始红线，不是排序后位于首项的合成回卷锚点。
    // 两者 BPM 一致，但零点归属不同，决定是否需要给用户绿线腾位置。
    // 回卷锚点可直接承载单位 Scroll；没有用户绿线的原零点也无需腾位置。
    const bool useExistingAnchor =
        sameBpm && (rewound || (userAtZero && !userScrollAtFirst));
    auto& effects = document["effect"];
    if ( !effects.is_array() ) effects = nlohmann::json::array();
    if ( useExistingAnchor ) {
        // 已有锚点仍由原先的配对 delay 表示，不重新拟合其非整数毫秒相位。
        // 置于用户效果之前，后续显式绿线仍按原顺序接管滚速。
        effects.insert(
            effects.begin(),
            nlohmann::json{ { "beat", first["beat"] }, { "scroll", 1.0 } });
        return true;
    }

    const double firstBeat = initialTimingBeatValue(first["beat"]);
    const auto   delay     = first.find("delay");
    // firstDelay 仍是旧首段的表示，后续不能直接叠加到新增参考段上。
    const double firstDelay =
        delay != first.end() && delay->is_number() ? delay->get<double>() : 0.0;
    const double firstBeatLength     = 60000.0 / firstBpm;
    double       firstTime           = firstBeat * firstBeatLength + firstDelay;
    double       wrappedContentShift = 0;
    if ( pairedMainNode ) {
        // 只移除旧协议的包装进位，不能把用户红线的整拍前导一并丢掉。
        // 配对导出将负 delay 包装到正相位；原有普通内容多出一拍，首红线例外。
        // 新零点锚点会解除这层包装，必须恢复绝对相位而非直接使用 delay。
        firstTime = std::fmod(-firstDelay, firstBeatLength);
        if ( firstTime < 0 ) firstTime += firstBeatLength;
        if ( std::abs(firstTime) <= 1.0e-6 ||
             std::abs(firstTime - firstBeatLength) <= 1.0e-6 )
            firstTime = 0;
        wrappedContentShift = firstTime > 1.0e-6 ? 1 : 0;
    }
    const double referenceBeatLength = 60000.0 / referenceBpm;
    if ( !std::isfinite(firstTime) || !std::isfinite(referenceBeatLength) ||
         !(referenceBeatLength > 0) )
        return false;
    // 零点已有用户绿线或不同 BPM 时必须整体后移，为初始化配对留出一拍。
    // 所用拍长始终是参考 BPM；同 BPM 情况下也就是用户首红线的一拍。
    const bool needsLeadIn = std::abs(firstTime) <= 1.0e-6;
    // 不在零点的原首段可以直接在前面补参考红绿线，音频无需重编码。
    if ( needsLeadIn ) audioLeadInMilliseconds = referenceBeatLength;
    // 新参考段只占首红线之前的区间，原红线之后继续沿用旧拍间距。
    // delay 已吸收到新首段的拍位中，原首红线不能再次附加相同延迟。
    const double firstTargetBeat =
        (firstTime + audioLeadInMilliseconds) / referenceBeatLength;
    const auto   fittedFirst = fitMalodyBeatFraction(firstTargetBeat);
    const double fittedFirstBeat =
        fittedFirst.beatIndex +
        static_cast<double>(fittedFirst.numerator) / fittedFirst.denominator;
    const double beatDelta = fittedFirstBeat - firstBeat - wrappedContentShift;
    if ( !std::isfinite(beatDelta) ) return false;
    // 连续拍数有限后才接触各对象，错误数据不会留下半变换的输出文档。
    /// @brief 将原普通内容拍位映射到含参考前导段的新拍轴。
    /// @param beat 已包含原导出包装进位的连续拍位。
    /// @return 首锚点之前按参考 BPM 换算，之后保留原 BPM 段的相对间距。
    const auto remappedBeat = [&](double beat) {
        const double anchor = firstBeat + wrappedContentShift;
        if ( beat < anchor - 1e-6 ) {
            // 早于原首红线的物件也可能存在，不能把新参考段误当成旧首 BPM。
            // 只有这段需要改变拍间距；后半段继续统一平移，避免重积分所有红线。
            const double time = firstTime + (beat - anchor) * firstBeatLength;
            return (time + audioLeadInMilliseconds) / referenceBeatLength;
        }
        return beat + beatDelta;
    };
    /// @brief 对绝对 beat 应用分段映射，并复用统一的三元组拟合。
    const auto remapAbsoluteBeat = [&](nlohmann::json& beat) {
        const double oldBeat = initialTimingBeatValue(beat);
        shiftInitialTimingBeat(beat, remappedBeat(oldBeat) - oldBeat);
    };
    // 有限分母无法精确表达任意毫秒相位；残差留在原红线 delay，后续拍间距
    // 使用同一个拟合锚点，避免把首段量化误差传播到整条时间线。
    const double delayResidual = firstTime + audioLeadInMilliseconds -
                                 fittedFirstBeat * referenceBeatLength;
    // AppleClang 可将乘减融合，数学上为零的残差可能留下极小负值。
    // 只归零纳秒以下误差，真实首段相位仍由原红线 delay 精确保留。
    first["delay"] = std::abs(delayResidual) <= 1e-9 ? 0.0 : delayResidual;
    // 首红线原本不包含普通内容的包装进位，必须单独落到新参考段末端。
    shiftInitialTimingBeat(first["beat"], firstTargetBeat - firstBeat);
    for ( std::size_t index = 1; index < times.size(); ++index )
        // 后续 delay 属于对应红线本身，平移拍位时保留其独立数值。
        shiftInitialTimingBeat(times[index]["beat"], beatDelta);
    for ( auto& effect : effects ) remapAbsoluteBeat(effect["beat"]);
    auto& notes = document["note"];
    for ( std::size_t index = 0; index < notes.size(); ++index ) {
        auto& node = notes[index];
        if ( pairedMainNode && index == *pairedMainNode ) {
            // 主音轨从新零点播放；需要后移的内容由音频前导静音承载。
            // 非后移情况也用新零点配对，不再继承旧首红线的 delay offset。
            node["beat"]   = nlohmann::json::array({ 0, 0, 1 });
            node["offset"] = 0;
            continue;
        }
        // 独立 Main 锚点在最终时间线建好后按原毫秒映射，文件静音承担后移。
        if ( primaryMainNode && index == *primaryMainNode ) continue;
        const double oldRootBeat = initialTimingBeatValue(node["beat"]);
        remapAbsoluteBeat(node["beat"]);
        // Key Hold 尾拍是绝对拍位，可能横跨新参考段与原 BPM 段。
        if ( node.contains("endbeat") ) remapAbsoluteBeat(node["endbeat"]);
        if ( node.contains("seg") &&
             oldRootBeat < firstBeat + wrappedContentShift - 1e-6 ) {
            // Slide seg 相对根节点。根节点在参考前导段时，分别映射端点和根，
            // 再还原相对拍差；根与子节点都在原 BPM 段时则无需改动。
            const double newRootBeat = initialTimingBeatValue(node["beat"]);
            for ( auto& segment : node["seg"] ) {
                auto&        beat     = segment["beat"];
                const double relative = initialTimingBeatValue(beat);
                shiftInitialTimingBeat(beat,
                                       remappedBeat(oldRootBeat + relative) -
                                           newRootBeat - relative);
            }
        }
    }
    // 新配对只插入一次，原红线和用户自定义 Scroll 都保留在统一变换后的位置。
    times.insert(times.begin(),
                 nlohmann::json{ { "beat", nlohmann::json::array({ 0, 0, 1 }) },
                                 { "bpm", referenceBpm },
                                 { "delay", 0 } });
    if ( primaryMainNode && !pairedMainNode ) {
        // 新时间线已包含完整 BPM 段，主采样可位于任意段而不局限于时间零。
        // 主音频原有 offset 保持独立。补静音后仍从原锚点触发，才能使音乐
        // 内容恰好后移一拍；同时移动 SOUND 和补静音会让音乐多迟一拍。
        double lastBeat = 0, lastTime = 0, bpm = referenceBpm;
        for ( std::size_t index = 1; index < times.size(); ++index ) {
            const auto&  timing = times[index];
            const double beat   = initialTimingBeatValue(timing["beat"]);
            const double time   = lastTime + (beat - lastBeat) * 60000.0 / bpm +
                                  timing.value("delay", 0.0);
            if ( time > primaryMainTimestamp + 1e-6 ) break;
            // 跳过未来红线，当前段的 BPM 才能用于反算主采样的触发拍位。
            lastTime = time;
            lastBeat = beat;
            bpm      = timing["bpm"].get<double>();
        }
        auto& beat = notes[*primaryMainNode]["beat"];
        beat       = nlohmann::json::array({ 0, 0, 1 });
        shiftInitialTimingBeat(
            beat,
            lastBeat + (primaryMainTimestamp - lastTime) / (60000.0 / bpm));
    }
    effects.insert(
        effects.begin(),
        nlohmann::json{ { "beat", nlohmann::json::array({ 0, 0, 1 }) },
                        { "scroll", 1.0 } });
    return true;
}
}  // namespace MMM::Internal
