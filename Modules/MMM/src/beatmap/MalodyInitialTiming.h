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
/// 同参考 BPM 时不增加红绿线，不改变用户设计的首段滚速。
/// 不同 BPM 先确定回卷首拍，再向前放置参考一拍，最后统一规范负时间。
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
/// @param sourceContentBeat 源首红线对应的普通内容拍位，含旧包装整拍补偿。
/// @param pairedMainNode 零点主音轨在 note 数组中的索引，没有配对时为空。
/// @param primaryMainNode 主音轨节点，可带有独立的 offset 而不满足首拍配对。
/// @param primaryMainTimestamp 主音轨原始毫秒锚点，不含独立 offset。
/// @param audioLeadInMilliseconds 接收必须加到主音频前面的静音长度。
/// @return BPM 和拍轴计算均有限时返回 true。
/// @note 后续红绿线只接受统一拍轴变换，不添加或覆盖其效果参数。
/// @warning 低频导出路径：仅遍历已生成的 JSON，不访问文件或共享模型。
inline bool normalizeMalodyInitialTiming(
    nlohmann::json& document, double referenceBpm, double sourceFirstTime,
    bool rewound, double sourceContentBeat,
    std::optional<std::size_t> pairedMainNode,
    std::optional<std::size_t> primaryMainNode, double primaryMainTimestamp,
    double& audioLeadInMilliseconds)
{
    // 失败或无需初始化时不携带调用方上一次导出的音频计划。
    audioLeadInMilliseconds = 0;
    // 没有 BPM 的文档无需初始化；不能仅凭歌曲显示值虚构整条计时时间线。
    const auto timesIt = document.find("time");
    if ( timesIt == document.end() || !timesIt->is_array() || timesIt->empty() )
        return true;
    auto& times = *timesIt;
    // 此处输入来自正式构建器，三元组结构已确定；无 BPM 的文档保持原状。
    // BPM 非有限或非正数时拒绝导出，不能将错误数值写成初始化红线。
    // 来源容器允许乱序，以最早拍位取首 BPM，不以 note 数组或效果值猜测。
    const auto& first = *std::min_element(
        times.begin(), times.end(), [](const auto& lhs, const auto& rhs) {
            return initialTimingBeatValue(lhs["beat"]) <
                   initialTimingBeatValue(rhs["beat"]);
        });
    const double firstBpm = first["bpm"].get<double>();
    // 无效 BPM 必须在触碰输出拍位前拒绝，避免除法把 NaN 扩散到三元组。
    if ( !std::isfinite(firstBpm) || !(firstBpm > 0) ) return false;
    if ( !std::isfinite(referenceBpm) || !(referenceBpm > 0) )
        referenceBpm = firstBpm;
    // 参考 BPM 相同就沿用原拍轴和用户滚速，不再无条件插入单位绿线。
    if ( std::abs(firstBpm - referenceBpm) <= 1.0e-6 ) {
        // 相同 BPM 仍保留原时间线排序，不能因跳过补偿而依赖来源容器有序。
        std::stable_sort(
            times.begin(), times.end(), [](const auto& lhs, const auto& rhs) {
                return initialTimingBeatValue(lhs["beat"]) <
                       initialTimingBeatValue(rhs["beat"]);
            });
        return true;
    }
    // 两个 BPM 对应不同拍长，回卷使用原首拍，前置间隔使用参考拍。
    const double firstBeatLength     = 60000.0 / firstBpm;
    const double referenceBeatLength = 60000.0 / referenceBpm;
    if ( !std::isfinite(sourceFirstTime) || !std::isfinite(sourceContentBeat) ||
         !std::isfinite(firstBeatLength) || !(firstBeatLength > 0) ||
         !std::isfinite(referenceBeatLength) || !(referenceBeatLength > 0) )
        return false;

    // 先在源时间域回卷整拍，再在回卷位置前放置完整的参考一拍。
    // 已在负时间的红线不向未来挪动，保留其前导语义交由统一规范化处理。
    double rolledTime = sourceFirstTime > 0
                            ? std::fmod(sourceFirstTime, firstBeatLength)
                            : sourceFirstTime;
    // 清除整拍除法残差，避免凭空增加几纳秒静音或出现负零。
    if ( std::abs(rolledTime) <= 1e-6 ||
         std::abs(rolledTime - firstBeatLength) <= 1e-6 )
        rolledTime = 0;
    const double referenceTime = rolledTime - referenceBeatLength;
    // 负参考锚点整体转成合法非负时轴，音频补同样静音，不能只夹取红线为零。
    audioLeadInMilliseconds = std::max(0.0, -referenceTime);
    // 统一平移后，前导参考段恰好占一参考拍；正锚点保留其正时间。
    const double rolledBeat =
        (rolledTime + audioLeadInMilliseconds) / referenceBeatLength;
    // 原用户红线即使远于一拍也保留，回卷副本负责较早区间的同 BPM 网格。
    const double firstTargetBeat =
        rolledBeat + (sourceFirstTime - rolledTime) / firstBeatLength;
    // 原首红线之后仍是原 BPM 积分，因此只改变绝对拍原点。
    const double beatDelta = firstTargetBeat - sourceContentBeat;
    const double referenceTargetBeat =
        (referenceTime + audioLeadInMilliseconds) / referenceBeatLength;
    if ( !std::isfinite(firstTargetBeat) || !std::isfinite(beatDelta) ||
         !std::isfinite(referenceTargetBeat) )
        return false;

    const auto fitReference = fitMalodyBeatFraction(referenceTargetBeat);
    const auto referenceBeat =
        nlohmann::json::array({ fitReference.beatIndex,
                                fitReference.numerator,
                                fitReference.denominator });
    const double fittedReferenceBeat =
        fitReference.beatIndex +
        static_cast<double>(fitReference.numerator) / fitReference.denominator;
    // 回卷锚点与参考锚点共用一次拟合，计算 delay 时必须扣掉前一红线的残差。
    // 否则正参考锚点的相位会在下一红线再次叠加，形成双重补偿。
    const auto   fitRolled = fitMalodyBeatFraction(rolledBeat);
    const double fittedRolledBeat =
        fitRolled.beatIndex +
        static_cast<double>(fitRolled.numerator) / fitRolled.denominator;
    // 原导出包装红线只负责 delay/offset 协议，不是用户事件；新拍轴重新建立它。
    // 它在 timeArr
    // 中固定先追加，必须在排序前按该身份移除，而不是猜测同拍用户红线。
    if ( rewound ) times.erase(times.begin());
    // 去除旧包装后只对真正用户红线排序，保持同拍来源顺序。
    std::stable_sort(
        times.begin(), times.end(), [](const auto& lhs, const auto& rhs) {
            return initialTimingBeatValue(lhs["beat"]) <
                   initialTimingBeatValue(rhs["beat"]);
        });
    // effect 缺失是合法 MC，按需建立数组而不修改源模型的 Timing 容器。
    auto& effects = document["effect"];
    if ( !effects.is_array() ) effects = nlohmann::json::array();
    // 普通内容拍位来自明确的源首红线内容锚点，不能从已包装的 delay 反推。
    // 先于回卷红线的部分使用参考 BPM，其余首 BPM 段及后续段保留原拍间距。
    // 分段映射仅作用于生成文档，导出重复调用不会在原谱上累计偏移。
    const auto remappedBeat = [&](double beat) {
        if ( beat < sourceContentBeat - 1e-6 ) {
            const double time =
                sourceFirstTime + (beat - sourceContentBeat) * firstBeatLength;
            // 位于新红绿线与回卷首红线之间的物件必须按参考 BPM 重算拍距。
            if ( time < rolledTime - 1e-6 )
                return (time + audioLeadInMilliseconds) / referenceBeatLength;
            return rolledBeat + (time - rolledTime) / firstBeatLength;
        }
        // 后续变速与 delay 已由原序列化器积分，不在此重算或近似它们。
        return beat + beatDelta;
    };
    /// @brief 对绝对 beat 应用分段映射，并复用统一的三元组拟合。
    // 使用统一分母拟合器，避免各物件路径产生不同的分拍编码。
    const auto remapAbsoluteBeat = [&](nlohmann::json& beat) {
        const double oldBeat = initialTimingBeatValue(beat);
        shiftInitialTimingBeat(beat, remappedBeat(oldBeat) - oldBeat);
    };
    // 原首红线单独按源绝对时间落位；其他红线仍按其独立 delay 平移。
    // 拟合误差留在 delay，不能把有限分母误差传播为整段歌曲漂移。
    const auto   fittedFirst = fitMalodyBeatFraction(firstTargetBeat);
    const double fittedFirstBeat =
        fittedFirst.beatIndex +
        static_cast<double>(fittedFirst.numerator) / fittedFirst.denominator;
    // 延迟属于进入当前红线前的 BPM 段，已拟合的前一红线是实际积分原点。
    const bool   hasRolledCopy = rolledTime < sourceFirstTime - 1e-6;
    const double previousTime  = hasRolledCopy ? rolledTime : referenceTime;
    const double previousBeat =
        hasRolledCopy ? fittedRolledBeat : fittedReferenceBeat;
    const double previousBeatLength =
        hasRolledCopy ? firstBeatLength : referenceBeatLength;
    const double residual =
        sourceFirstTime - previousTime -
        (fittedFirstBeat - previousBeat) * previousBeatLength;
    // sourceContentBeat 可能包含主音频包装进位，原首红线本身未必包含它。
    auto& originalFirst    = times.front();
    originalFirst["beat"]  = nlohmann::json::array({ fittedFirst.beatIndex,
                                                     fittedFirst.numerator,
                                                     fittedFirst.denominator });
    originalFirst["delay"] = std::abs(residual) <= 1e-9 ? 0.0 : residual;
    // 后续红线自有的 delay 是内容，不应与首段相位一起清除。
    for ( std::size_t index = 1; index < times.size(); ++index )
        shiftInitialTimingBeat(times[index]["beat"], beatDelta);
    for ( auto& effect : effects ) remapAbsoluteBeat(effect["beat"]);
    // 只修改绝对时间字段，音量、轨道、绑定文件及其余元数据保持原样。
    auto& notes = document["note"];
    for ( std::size_t index = 0; index < notes.size(); ++index ) {
        auto& node = notes[index];
        // 索引由当前文档生成阶段提供，不通过同文件名重复触发猜测事件身份。
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
        if ( node.contains("seg") && oldRootBeat < sourceContentBeat - 1e-6 ) {
            // Slide seg 相对根节点。根节点在参考前导段时，分别映射端点和根，
            // 再还原相对拍差；根与子节点都在原 BPM 段时则无需改动。
            // 根与尾分处两个 BPM 区间时，不能直接给相对 seg 加同一常数。
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
    // 首红线原本离零点超过一拍时，补一个同 BPM 的回卷副本；原用户红线保留。
    // 同刻用户绿线仍在原位置，不能复制到回卷锚点并改变前导滚速。
    // 副本只携带当前 BPM，不能继承原红线的历史 beat 缓存或旧 delay。
    if ( rolledTime < sourceFirstTime - 1e-6 ) {
        // 前一参考红线已保留其实际毫秒，相邻红线只补两次拟合之间的剩余差。
        const auto&  fit = fitRolled;
        const double rollResidual =
            rolledTime - referenceTime -
            (fittedRolledBeat - fittedReferenceBeat) * referenceBeatLength;
        times.insert(
            times.begin(),
            nlohmann::json{
                { "beat",
                  nlohmann::json::array(
                      { fit.beatIndex, fit.numerator, fit.denominator }) },
                { "bpm", firstBpm },
                { "delay",
                  std::abs(rollResidual) <= 1e-9 ? 0.0 : rollResidual } });
    }
    // MC 使用有限分母，参考锚点同样拟合而不是写入负分子或浮点拍号。
    // 正参考锚点可能无法精确表示，delay 承载实际毫秒与拟合拍号的差。
    const double referenceResidual =
        (referenceTargetBeat - fittedReferenceBeat) * referenceBeatLength;
    // 正参考锚点保留实际拍位；负锚点此前已与所有内容和音频一起平移到零。
    times.insert(times.begin(),
                 nlohmann::json{ { "beat", referenceBeat },
                                 { "bpm", referenceBpm },
                                 { "delay",
                                   std::abs(referenceResidual) <= 1e-9
                                       ? 0.0
                                       : referenceResidual } });
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
        // 独立局部 offset 由采样自身保留，反算这里只使用源触发锚点。
        auto& beat = notes[*primaryMainNode]["beat"];
        beat       = nlohmann::json::array({ 0, 0, 1 });
        shiftInitialTimingBeat(
            beat,
            lastBeat + (primaryMainTimestamp - lastTime) / (60000.0 / bpm));
    }
    // 初始化只插入一次且严格与参考红线同拍，原用户效果保持在其后。
    // 单位 Scroll 属于新增参考段，原有用户绿线既不删除也不覆盖。
    effects.insert(
        effects.begin(),
        nlohmann::json{ { "beat", referenceBeat }, { "scroll", 1.0 } });
    return true;
}
}  // namespace MMM::Internal
