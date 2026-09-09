#include "logic/ecs/system/ScrollCache.h"
#include "config/EditorConfig.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/BpmNormalization.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>

namespace MMM::Logic::System
{

/// @brief 检查 Malody 原始元数据中保存的效果名称。
/// @param tl 待检查的时间线组件，不修改其来源元数据。
/// @param name 不带 JSON 引号的效果名称，按原样匹配大小写。
/// @return 元数据域或 effect 键缺失时返回 false。
static bool isMalodyEffect(const TimelineComponent& tl, const std::string& name)
{
    if ( auto it = tl.m_metadata.timing_properties.find(
             ::MMM::TimingMetadataType::MALODY);
         it != tl.m_metadata.timing_properties.end() ) {
        if ( auto effectIt = it->second.find("effect");
             effectIt != it->second.end() ) {
            // 元数据保留 JSON 字符串表示，比较时也必须包含外层引号。
            return effectIt->second == "\"" + name + "\"";
        }
    }
    return false;
}

/// @brief 判断时间点是否带有 Malody 元数据域，用于区分 BPM 状态继承语义。
/// @param tl 当前事件，仅检查来源域是否存在。
/// @return 即使来源属性表为空，只要存在 Malody 域也返回 true。
static bool hasMalodyMetadata(const TimelineComponent& tl)
{
    return tl.m_metadata.timing_properties.contains(
        ::MMM::TimingMetadataType::MALODY);
}

/// @brief 根据时间线注册表重建滚动缓存。
/// @pre 时间点的时间戳满足排序要求；注册表在整个重建期间保持稳定。
/// @param timelineRegistry 时间线注册表。
/// @param config 当前编辑器配置。
/// @param beatmap 当前 Session 绑定的谱面；为空时使用保守默认值。
/// @warning 逻辑热路径低频分支：会完整遍历和排序时间线，只能在 isDirty
/// 时执行，严禁每 update 无条件调用。
/// @note 重建不是跨线程发布接口，调用方仍负责隔离注册表和缓存读写。
void ScrollCache::rebuild(const entt::registry&       timelineRegistry,
                          const Config::EditorConfig& config,
                          MMM::BeatMap*               beatmap)
{
    // 线性映射只关闭显示效果，仍保留时间点本身供状态查询和编辑使用。
    const double BASE_SPEED      = 500.0;
    const auto&  visualConfig    = config.visual;
    const bool   isLinearMapping = visualConfig.enableLinearScrollMapping;
    const bool   enableEffects   = !isLinearMapping;
    double       timelineZoom    = visualConfig.timelineZoom;
    // 缩放必须是有限正数，避免后续分段速度退化或污染积分结果。
    if ( !std::isfinite(timelineZoom) || timelineZoom <= 1e-9 ) {
        timelineZoom = 1.0;
    }
    m_lastZoom = timelineZoom;
    // 记录规范后的基础缩放，动画缩放另由 setAnimatedZoomScale 管理。

    // 复用暂存容量，仅在本次重建期间借用注册表中的组件地址。
    // 重建过程中调用方不得修改注册表并使这些地址失效。
    m_rebuildScratch.clear();
    auto tlView = timelineRegistry.view<const TimelineComponent>();
    m_rebuildScratch.reserve(tlView.size());

    for ( auto entity : tlView ) {
        m_rebuildScratch.push_back(
            { entity, &tlView.get<const TimelineComponent>(entity) });
    }

    // 空时间线也是一次有效重建，必须清除旧谱面的派生索引和跳变窗口。
    if ( m_rebuildScratch.empty() ) {
        // 没有任何事件时不生成默认积分段，后续查询按各自的空缓存约定处理。
        m_segments.clear();
        m_absYRangeIndex.clear();
        m_microImpulseWindows.clear();
        m_hasJumpEffects = false;
        isDirty          = false;
        // 修订号仍需推进，让依赖缓存版本的消费者识别状态已被替换。
        ++m_revision;
        return;
    }

    // 排序逻辑：时间戳升序；时间戳相同时，BPM 类型优先于 SCROLL 类型
    std::stable_sort(
        m_rebuildScratch.begin(),
        m_rebuildScratch.end(),
        [](const auto& a, const auto& b) {
            if ( std::abs(a.component->m_timestamp - b.component->m_timestamp) >
                 1e-9 ) {
                return a.component->m_timestamp < b.component->m_timestamp;
            }
            if ( a.component->m_effect != b.component->m_effect ) {
                return a.component->m_effect == ::MMM::TimingEffect::BPM;
            }
            return false;  // 保持原始顺序
        });

    // 新段落在局部容器内构造，避免在读取旧缓存时暴露部分重建结果。
    std::vector<ScrollSegment> newSegments;
    newSegments.reserve(m_rebuildScratch.size() + 1);

    // 1. 完整版 osu! 逻辑：计算最常见 BPM 作为基准，并获取 SliderMultiplier。
    double refBPM           = 120.0;
    double sliderMultiplier = 1.0;
    // 无谱面对象时保持中性倍率；有谱面但没有 osu! 属性时采用格式默认值。
    if ( beatmap ) {
        // 尝试获取 SliderMultiplier (默认为 1.4)
        sliderMultiplier = beatmap->m_metadata.get_value<double>(
            MapMetadataType::OSU, "Difficulty::SliderMultiplier", 1.4);

        // 自动计算最常见的 BPM (持续时间最长)
        // 按规范后的 BPM 合并时长，而不是按出现次数选择基准速度。
        std::map<double, double> bpmDurations;
        double                   lastBpmTime   = 0.0;
        double                   currentBpmVal = 0.0;
        bool                     hasBpmEvent   = false;
        for ( const auto& entry : m_rebuildScratch ) {
            if ( entry.component->m_effect == ::MMM::TimingEffect::BPM ) {
                // 遇到下一条 BPM 才能结算上一段；首条之前不虚构持续时长。
                if ( hasBpmEvent ) {
                    bpmDurations[currentBpmVal] +=
                        (entry.component->m_timestamp - lastBpmTime);
                }
                currentBpmVal =
                    ::MMM::normalizeBpmValue(entry.component->m_value, refBPM);
                lastBpmTime = entry.component->m_timestamp;
                hasBpmEvent = true;
            }
        }
        // 加上最后一个段落到末尾的时间 (假设谱面时长)
        if ( hasBpmEvent ) {
            bpmDurations[currentBpmVal] +=
                (beatmap->m_baseMapMetadata.map_length - lastBpmTime);
        }

        double maxDuration = -1.0;
        // 这里比较的是累计时长，不是最后一段 BPM，也不是事件数量最多的 BPM。
        // 相同时长不覆盖既有候选；有序映射使该选择保持确定性。
        for ( const auto& [bpm, dur] : bpmDurations ) {
            if ( dur > maxDuration ) {
                maxDuration = dur;
                refBPM      = bpm;
            }
        }
        if ( hasBpmEvent ) {
            refBPM = ::MMM::normalizeBpmValue(refBPM);
        }
    }

    double currentBPM = refBPM;

    // 状态栏需要展示时间线语义上的当前 SV。该值独立于线性映射开关，
    // 避免关闭滚动效果后丢失谱面实际段落信息。
    double activeScrollValue = 1.0;

    // osu! MultiplierControlPoint 换算：
    //   倍率 = Velocity × ScrollSpeed × BaseBeatLength / BeatLength。
    //   推导 = 1.0 × scrollMult × (60000 / refBPM) / (60000 / bpm)。
    //   推导 = scrollMult × bpm / refBPM。
    //   速度 = 倍率 × scrollLength / timeRange。
    //   推导 = scrollMult × bpm / refBPM × BASE_SPEED × timelineZoom。
    /// @brief 将当前 BPM 和 SV 换算为该段积分速度。
    /// @param bpm 已按公共 BPM 规则规范的当前值。
    /// @param sm 有符号 SV，零值允许形成停止段。
    /// @return 当前基础缩放下的滚动距离变化率，不包含 HS 局部投影倍率。
    // 线性模式提前返回，不受 BPM、SV 或 osu! 滑条倍率影响。
    auto calcSpeed = [&](double bpm, double sm) {
        if ( isLinearMapping ) {
            // 保持恒定正向速度，但后面的事件位和活动值仍会照常记录。
            return BASE_SPEED * timelineZoom;
        }
        double ratio = bpm / refBPM;
        if ( !std::isfinite(ratio) ) {
            ratio = 0.0;
        }
        if ( ratio < 0.0 ) {
            ratio = 0.0;
        }
        // 只限制 BPM 比值；SV 可为负数，以表达反向滚动。
        return ratio * sm * sliderMultiplier * BASE_SPEED * timelineZoom;
    };

    // 段落状态从默认倍率开始；后续 BPM 是否重置 SV 由来源元数据决定。
    double currentScrollMult = 1.0;
    double currentHs         = 1.0;
    // HS 不进入积分速度，段落记录它供显示距离查询单独应用。
    // 首事件早于零时从负时间起算，避免丢失其后到零点的积分跨度。
    double lastTime = std::min(0.0, m_rebuildScratch[0].component->m_timestamp);
    double currentAbsY = 0.0;
    m_hasJumpEffects   = false;

    double currentSpeed = calcSpeed(currentBPM, currentScrollMult);
    // 初始段提供首事件前的外推基准，即使首事件位于正时间也有段可查。
    newSegments.push_back({ lastTime, 0.0, currentSpeed, 0 });
    newSegments.back().hs                = currentHs;
    newSegments.back().hsValue           = currentHs;
    newSegments.back().activeBpmValue    = currentBPM;
    newSegments.back().activeScrollValue = activeScrollValue;
    // 初始段没有真实事件位，时间点标记绘制应跳过它而非生成虚构红线。

    for ( const auto& entry : m_rebuildScratch ) {
        const auto* tl = entry.component;
        // 先按旧速度积到事件时刻，再应用新状态，保持段边界连续。
        // 同时刻事件共用段落，按前面的排序顺序叠加效果。
        if ( tl->m_timestamp > lastTime ) {
            double dt = tl->m_timestamp - lastTime;
            currentAbsY += dt * currentSpeed;
            lastTime = tl->m_timestamp;
            newSegments.push_back({ lastTime, currentAbsY, currentSpeed, 0 });
        }

        if ( tl->m_effect == ::MMM::TimingEffect::BPM ) {
            newSegments.back().effects |= SCROLL_EFFECT_BPM;
            newSegments.back().bpmEntity = entry.entity;
            newSegments.back().bpmValue  = tl->m_value;
            // 实体 ID 和原始字段用于定位编辑对象，活动字段用于查询当前状态。
            // 编辑展示保留原始值，实际速度计算使用规范后的 BPM。
            currentBPM = ::MMM::normalizeBpmValue(tl->m_value, refBPM);
            if ( !hasMalodyMetadata(*tl) ) {
                // osu! 红线会重置 SV；Malody 的 BPM 不改变 effect 状态。
                activeScrollValue = 1.0;
                if ( enableEffects ) {
                    currentScrollMult = 1.0;
                }
            }
        } else if ( tl->m_effect == ::MMM::TimingEffect::SCROLL ) {
            newSegments.back().effects |= SCROLL_EFFECT_SCROLL;
            newSegments.back().scrollEntity = entry.entity;
            newSegments.back().scrollValue  = tl->m_value;
            // 同时刻的多个同类事件仍只留一个段，最后处理的事件成为该类编辑目标。
            // mmm/Malody 内部均存储原始 SV 倍率；osu! 的负 inherited
            // beatLength 已在导入边界转换。
            // 非有限 SV 不替换当前有效状态，但事件记录仍保留供编辑显示。
            if ( std::isfinite(tl->m_value) ) {
                activeScrollValue = tl->m_value;
                if ( enableEffects ) {
                    currentScrollMult = tl->m_value;
                }
            }
        } else if ( tl->m_effect == ::MMM::TimingEffect::JUMP ) {
            newSegments.back().effects |= SCROLL_EFFECT_JUMP;
            newSegments.back().jumpEntity = entry.entity;
            newSegments.back().jumpValue  = tl->m_value;
            m_hasJumpEffects              = true;
            // 即使线性映射禁用了位移，也保留存在 Jump 事件的事实。
            if ( enableEffects ) {
                // Malody Jump 在滚动积分上制造瞬时断层。
                currentAbsY += (tl->m_value / 1000.0) * currentSpeed;
                // Jump 值按毫秒换算为秒，不改变事件时间戳，只调整累计位置。
                newSegments.back().absY = currentAbsY;
            }
        } else if ( tl->m_effect == ::MMM::TimingEffect::HS ) {
            newSegments.back().effects |= SCROLL_EFFECT_HS;
            newSegments.back().hsEntity = entry.entity;
            newSegments.back().hsValue  = tl->m_value;
            // 原始 HS 值可用于编辑展示，实际应用值在线性模式下保持中性状态。
            if ( enableEffects ) {
                currentHs = tl->m_value;
            }
        }

        // 每个事件处理后回写完整活动状态，后续查询无需向前追溯多种事件。
        currentSpeed             = calcSpeed(currentBPM, currentScrollMult);
        newSegments.back().speed = currentSpeed;
        newSegments.back().hs    = currentHs;
        newSegments.back().activeBpmValue    = currentBPM;
        newSegments.back().activeScrollValue = activeScrollValue;
    }

    // 派生索引必须基于新段落重建，完成后才清除脏标志并发布新修订号。
    m_segments = std::move(newSegments);
    // 时间顺序段落与位置反查索引的顺序不同，不能直接把时间段号当位置索引。
    rebuildAbsYRangeIndex();
    rebuildMicroImpulseWindows();
    isDirty = false;
    ++m_revision;
}

/// @brief 设置渲染用动画时间线缩放比例。
/// @param scale 相对于缓存基础缩放的倍率，无效或过小值回退为 1。
/// @note 这里只改变查询坐标，不改变 m_lastZoom 或缓存修订号。
/// @warning 逻辑/渲染热路径：每个 Session update 执行；只做常量级赋值。
void ScrollCache::setAnimatedZoomScale(double scale)
{
    // 不通过重建整条时间线完成缩放动画，以免每个动画步重新排序和积分。
    if ( !std::isfinite(scale) || scale <= 1e-9 ) {
        m_animatedZoomScale = 1.0;
        return;
    }
    m_animatedZoomScale = scale;
}

/// @brief 返回反查索引采用的未动画缩放位置区间。
/// @param index 时间顺序中的段号，不是位置索引中的条目号。
/// @return 有限段的端点包围区间；末段返回带无穷远边界的区间。
/// @pre index 对应现有段落；本函数不检查索引越界。
/// @warning 缓存重建期间调用，保持常量复杂度。
std::pair<double, double> ScrollCache::getSegmentAbsYRange(
    std::size_t index) const
{
    const auto& seg = m_segments[index];
    if ( index + 1 < m_segments.size() ) {
        // 区间取缓存端点而非用当前速度外推，包含下一事件产生的位置变化。
        // 取两端位置的包围区间，反向滚动也统一为递增边界。
        double nextAbsY = m_segments[index + 1].absY;
        return { std::min(seg.absY, nextAbsY), std::max(seg.absY, nextAbsY) };
    }

    // 最后一段没有结束时间，按速度方向延伸到无穷远。
    if ( seg.speed >= 0.0 ) {
        // 零速尾段也保守归入正向无穷区间，精确时间求交由后续反查逻辑处理。
        return { seg.absY, std::numeric_limits<double>::infinity() };
    }
    return { -std::numeric_limits<double>::infinity(), seg.absY };
}

/// @brief 为位置到时间的反查建立位置区间索引，保留原段落编号。
/// @pre m_segments 已替换为本次重建完成的段落集合。
/// @note 原积分段不排序；只排序独立索引，以保留时间查询所需顺序。
/// @warning 低频缓存重建分支执行完整遍历与排序，不得逐帧调用。
void ScrollCache::rebuildAbsYRangeIndex()
{
    // 容量复用不保留旧条目；每个条目携带新段落编号，不能混用两代数据。
    m_absYRangeIndex.clear();
    m_absYRangeIndex.reserve(m_segments.size());

    for ( std::size_t i = 0; i < m_segments.size(); ++i ) {
        const auto& seg = m_segments[i];
        // 停止段中同一位置对应多个时间，不能作为除以速度的反查候选。
        if ( std::abs(seg.speed) < 1e-9 ) continue;

        auto [minAbsY, maxAbsY] = getSegmentAbsYRange(i);
        // 反向段也按有序位置区间保存，原速度符号仍留在时间段内供反解使用。
        m_absYRangeIndex.push_back({ minAbsY, maxAbsY, i });
    }

    // 先按下界排序以缩小查询范围；相同范围按原始段号保持确定顺序。
    std::stable_sort(m_absYRangeIndex.begin(),
                     m_absYRangeIndex.end(),
                     [](const AbsYRangeEntry& a, const AbsYRangeEntry& b) {
                         if ( std::abs(a.minAbsY - b.minAbsY) > 1e-9 )
                             return a.minAbsY < b.minAbsY;
                         if ( std::abs(a.maxAbsY - b.maxAbsY) > 1e-9 )
                             return a.maxAbsY < b.maxAbsY;
                         return a.segmentIndex < b.segmentIndex;
                     });
}

/// @brief 识别短时正反滚动脉冲，为视觉锚点建立线性替代窗口。
/// @pre 段落按时间排列，速度和位置均处于未动画缩放空间。
/// @note 每次重新识别，不保留上一谱面或旧缩放下的窗口。
/// @warning 仅随缓存重建执行；窗口只影响视觉锚点，不修改原始积分段。
void ScrollCache::rebuildMicroImpulseWindows()
{
    // 时间限制用于识别谱面中的短脉冲，不是线程等待或渲染节流窗口。
    constexpr double MAX_SLICE_SECONDS        = 0.0035;
    constexpr double MAX_WINDOW_SECONDS       = 0.0075;
    constexpr double MIN_PEAK_DISPLACEMENT    = 24.0;
    constexpr double MAX_ABS_NET_DISPLACEMENT = 12.0;
    constexpr double MAX_REL_NET_DISPLACEMENT = 0.12;
    // 位移门槛在缓存基础缩放空间中判断，后续动画倍率不重新决定窗口资格。

    m_microImpulseWindows.clear();
    // 两个连续片段需要三个边界点，末段缺少结束点时不能单独组成窗口。
    if ( m_segments.size() < 3 ) {
        return;
    }

    m_microImpulseWindows.reserve(m_segments.size() / 8);
    // reserve 只是容量预估，不限制最终窗口数；识别结果由后面的条件决定。
    for ( std::size_t i = 0; i + 2 < m_segments.size(); ++i ) {
        const auto& first  = m_segments[i];
        const auto& second = m_segments[i + 1];
        const auto& after  = m_segments[i + 2];

        const double firstDuration  = second.time - first.time;
        const double secondDuration = after.time - second.time;
        const double windowDuration = after.time - first.time;
        // 排除空片段及持续过长的正常速度变化，只处理极短的往返位移。
        if ( firstDuration <= 0.0 || secondDuration <= 0.0 ||
             firstDuration > MAX_SLICE_SECONDS ||
             secondDuration > MAX_SLICE_SECONDS ||
             windowDuration > MAX_WINDOW_SECONDS ) {
            continue;
        }

        // 必须是有限且异号的速度，单向加速或停止不属于往返脉冲。
        if ( !std::isfinite(first.speed) || !std::isfinite(second.speed) ||
             first.speed * second.speed >= 0.0 ) {
            continue;
        }

        const double firstDelta  = first.speed * firstDuration;
        const double secondDelta = second.speed * secondDuration;
        const double peakDelta =
            std::max(std::abs(firstDelta), std::abs(secondDelta));
        const double netDelta = firstDelta + secondDelta;
        // 用速度与时长衡量脉冲往返，最终插值端点则取实际累计位置。
        // 小幅往返无需替代；保留它们可避免平滑改变普通细微滚动。
        if ( peakDelta < MIN_PEAK_DISPLACEMENT ) {
            continue;
        }
        // 净位移只需满足绝对或相对容差之一；两项都超限才排除。
        if ( std::abs(netDelta) > MAX_ABS_NET_DISPLACEMENT &&
             std::abs(netDelta) > peakDelta * MAX_REL_NET_DISPLACEMENT ) {
            continue;
        }

        // 保存真实边界积分值，替代插值在窗口两端与原始位置衔接。
        m_microImpulseWindows.push_back(
            { first.time, after.time, first.absY, after.absY });
        // 查询仍能在原始段落中看到往返轨迹，窗口只提供另一种视觉锚点位置。
        // 已配对的第二片段不再作为下一窗口起点，避免相邻窗口重叠。
        ++i;
    }
}

/// @brief 将缓存积分位置映射到当前动画缩放空间。
/// @param absY 可正可负的缓存坐标，同一缩放也用于速度导出。
/// @return 只含乘法缩放的结果，不附加判定线偏移或 HS。
/// @warning 热路径常量运算，不重建积分段。
double ScrollCache::applyAnimatedZoomScale(double absY) const
{
    // 基础缩放已参与重建积分，这里只叠加动画相对倍率，不能重复乘 m_lastZoom。
    return absY * m_animatedZoomScale;
}

/// @brief 撤销动画缩放，以便在固定缓存坐标中反查。
/// @param animatedAbsY 已应用动画倍率、尚未应用 HS 的坐标。
/// @return 缓存坐标；防御性退化分支原样返回输入。
/// @warning 热路径常量运算，不修改动画状态。
double ScrollCache::toUnscaledAbsY(double animatedAbsY) const
{
    // 防御性保留输入，避免极小比例导致反查坐标除零或急剧放大。
    if ( std::abs(m_animatedZoomScale) <= 1e-9 ) {
        return animatedAbsY;
    }
    return animatedAbsY / m_animatedZoomScale;
}

/// @brief 按分段速度查询原始积分位置，不应用动画或微脉冲修饰。
/// @param t 查询时间，允许落在首个时间点之前。
/// @return 空缓存时采用基础速度外推；否则按所在段速度积分。
/// @pre t 为有限时间值；该查询不负责修复非法时间输入。
/// @warning 热路径使用二分查找，不遍历时间线实体。
double ScrollCache::getUnscaledRawAbsY(double t) const
{
    // 空缓存使用与线性模式相同的基础速度，但仍保留最近一次规范后的基础缩放。
    const double DEFAULT_SPEED = 500.0 * m_lastZoom;
    if ( m_segments.empty() ) return t * DEFAULT_SPEED;

    // 查找严格晚于 t 的段，回退一步使边界时刻采用新段状态。
    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        t,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    if ( it == m_segments.begin() ) {
        // 如果 t 比第一个点还早，按第一个点的速度回溯
        return m_segments[0].absY +
               (t - m_segments[0].time) * m_segments[0].speed;
    }
    --it;
    // 段内位置由锚点加线性位移得到，不跨段累加，查询成本不随谱面时长增加。
    return it->absY + (t - it->time) * it->speed;
}

/// @brief 返回带动画缩放的原始滚动位置。
/// @param t 查询时间，单位秒。
/// @return 保留 Jump 和速度反转的坐标，不乘物件锚点 HS。
/// @warning 逐物件热路径，只委托缓存查询和常量缩放。
double ScrollCache::getRawAbsY(double t) const
{
    // 保持原始曲线入口独立，物件坐标不能被视觉锚点的脉冲平滑一并改写。
    return applyAnimatedZoomScale(getUnscaledRawAbsY(t));
}

/// @brief 在命中的短脉冲窗口内以边界插值替代原始位置。
/// @param t 用于命中窗口的时间，单位秒。
/// @param rawAbsY 未动画缩放的原始位置，与窗口坐标处于同一空间。
/// @return 窗口外原样返回 rawAbsY，窗口内返回边界间的线性位置。
/// @warning 热路径只做二分查找和插值，不新建窗口。
double ScrollCache::applyMicroImpulseWindow(double t, double rawAbsY) const
{
    if ( m_microImpulseWindows.empty() ) {
        return rawAbsY;
    }

    auto it =
        std::upper_bound(m_microImpulseWindows.begin(),
                         m_microImpulseWindows.end(),
                         t,
                         [](double value, const MicroImpulseWindow& window) {
                             return value < window.startTime;
                         });

    if ( it == m_microImpulseWindows.begin() ) {
        // 查询早于第一个窗口，不向前外推平滑区间。
        return rawAbsY;
    }

    --it;
    // 最近的起点不代表仍在窗口内，两个窗口之间必须恢复原始滚动。
    if ( t < it->startTime || t > it->endTime ) {
        return rawAbsY;
    }

    const double duration = it->endTime - it->startTime;
    // 即使内部窗口数据退化，也保持原始位置可用，避免插值除以零。
    if ( duration <= 0.0 ) {
        return rawAbsY;
    }

    // 已验证时间在闭区间内，插值系数无需另行限幅。
    const double alpha = (t - it->startTime) / duration;
    return it->startAbsY + (it->endAbsY - it->startAbsY) * alpha;
}

/// @brief 获取物件使用的原始滚动位置，保留全部瞬时位移。
/// @param t 物件时间，单位秒。
/// @note 不减去当前显示原点；相对距离应由 getDisplayDelta 计算。
/// @warning 热路径委托原始查询，不应用视觉锚点平滑。
double ScrollCache::getAbsY(double t) const
{
    return getRawAbsY(t);
}

/// @brief 获取视觉锚点位置，先平滑微脉冲，再应用动画缩放。
/// @param t 视觉锚点时间，单位秒。
/// @note 该结果不是原始积分反查的唯一解，不能假定 getTime 可还原 t。
/// @warning 热路径使用现有段落和窗口，不重建缓存。
double ScrollCache::getVisualAnchorAbsY(double t) const
{
    // 窗口与原始积分在同一基础空间中插值，最后统一施加动画缩放。
    return applyAnimatedZoomScale(
        applyMicroImpulseWindow(t, getUnscaledRawAbsY(t)));
}

/// @brief 从动画缩放后的滚动位置反查时间；多段命中时选最早段号。
/// @param absY 未乘 HS、未减判定线原点的位置。
/// @return 候选段上的反解或首尾外推时间，不保证还原原始查询时刻。
/// @note 负速度和 Jump 可使映射非单调或不连续，反查选择是确定性约定。
/// @warning 查询会遍历下界匹配的区间候选，不是纯二分常量查询。
double ScrollCache::getTime(double absY) const
{
    const double DEFAULT_SPEED = 500.0 * m_lastZoom;
    // 区间索引保存未动画缩放坐标，查询前必须回到同一坐标空间。
    absY = toUnscaledAbsY(absY);
    if ( m_segments.empty() ) return absY / DEFAULT_SPEED;

    constexpr double EPS = 1e-6;
    // 容差用于位置区间边界比较，不代表把返回时间吸附到某个毫秒网格。
    std::size_t bestIndex = std::numeric_limits<std::size_t>::max();
    auto        endIt =
        std::upper_bound(m_absYRangeIndex.begin(),
                         m_absYRangeIndex.end(),
                         absY + EPS,
                         [](double value, const AbsYRangeEntry& entry) {
                             return value < entry.minAbsY;
                         });

    // 反向滚动可能使多个时间段覆盖同一位置，索引排序不代表时间先后。
    for ( auto it = m_absYRangeIndex.begin(); it != endIt; ++it ) {
        if ( it->maxAbsY < absY - EPS ) continue;
        if ( it->segmentIndex < bestIndex ) {
            bestIndex = it->segmentIndex;
        }
    }

    if ( bestIndex != std::numeric_limits<std::size_t>::max() ) {
        // 候选已经排除停止段，可按该段速度直接反解；不再选择离当前播放点最近的解。
        const auto& seg = m_segments[bestIndex];
        return seg.time + (absY - seg.absY) / seg.speed;
    }

    // 没有区间命中时按较近的首尾锚点外推；距离相等选末段。
    const auto& first = m_segments.front();
    const auto& last  = m_segments.back();
    const auto& edge =
        std::abs(absY - first.absY) < std::abs(absY - last.absY) ? first : last;
    // 外推不是将坐标截断到首尾范围，返回时间可能落在现有事件范围之外。
    // 近零速度无法稳定反解，直接返回选中边界的时间。
    if ( std::abs(edge.speed) < 1e-6 ) return edge.time;
    return edge.time + (absY - edge.absY) / edge.speed;
}

/// @brief 返回查询时刻的积分速度，包含当前动画缩放。
/// @param t 查询时间，单位秒。
/// @return 不含 HS 的段速度；首点之前采用首段速度。
/// @warning 热路径仅二分现有段落，不应用微脉冲窗口插值。
double ScrollCache::getSpeedAt(double t) const
{
    // 该值是原始分段速度，不是 getVisualAnchorAbsY 平滑曲线的数值导数。
    if ( m_segments.empty() ) {
        return applyAnimatedZoomScale(500.0 * m_lastZoom);
    }
    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        t,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    if ( it == m_segments.begin() ) {
        return m_segments[0].speed * m_animatedZoomScale;
    }
    // 边界时刻采用新段速度，与原始位置查询的 upper_bound 约定一致。
    --it;
    return it->speed * m_animatedZoomScale;
}

/// @brief 返回锚点所在段的 HS 倍率，空缓存采用单位倍率。
/// @param t 决定显示倍率的锚点时间，不一定是被绘制端点的时间。
/// @return 重建时记录的活动 HS，不再次限制倍率的符号或范围。
/// @warning 热路径二分查询；HS 本身不随动画缩放变化。
double ScrollCache::getHsAt(double t) const
{
    // HS 独立于积分速度；不能从 getSpeedAt 的结果中反推它。
    if ( m_segments.empty() ) return 1.0;
    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        t,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    if ( it == m_segments.begin() ) return m_segments[0].hs;
    // 首点之前沿用首段，其余时间取不晚于查询时刻的最后一段。
    --it;
    return it->hs;
}

/// @brief 获取给定时间戳所在滚动段的 BPM 与 SV。
/// @param t 查询时间，单位秒。
/// @return 当前段落生效的 BPM 与 SV；缓存为空时返回保守默认值。
/// @warning 逻辑热路径：每个 Session update 调用一次；只执行二分查找。
ScrollTimingState ScrollCache::getTimingStateAt(double t) const
{
    // 返回重建时保存的活动语义值，不根据当前视觉速度重新推算 BPM 或 SV。
    if ( m_segments.empty() ) {
        return {};
    }

    auto it = std::upper_bound(m_segments.begin(),
                               m_segments.end(),
                               t,
                               [](double value, const ScrollSegment& segment) {
                                   return value < segment.time;
                               });

    if ( it == m_segments.begin() ) {
        return { it->activeBpmValue, it->activeScrollValue };
    }

    --it;
    // 按值返回轻量状态，不让调用方持有可被下次重建失效的段落引用。
    return { it->activeBpmValue, it->activeScrollValue };
}

/// @brief 计算物件到当前显示原点的距离，并应用指定锚点的 HS。
/// @param t 要投影的物件或端点时间。
/// @param currentAbsY 已包含动画缩放的当前显示原点。
/// @param anchorTime 决定 HS 的锚点时间，长条尾部可沿用头部时间。
/// @return 相对显示距离，尚未转换为画布判定线坐标。
/// @pre currentAbsY 与本缓存使用相同动画倍率，不能传入未缩放的积分位置。
/// @warning 逐物件热路径，只查询已有段落，不遍历注册表。
double ScrollCache::getDisplayDelta(double t, double currentAbsY,
                                    double anchorTime) const
{
    const double DEFAULT_SPEED = 500.0 * m_lastZoom;
    if ( m_segments.empty() ) {
        return applyAnimatedZoomScale(t * DEFAULT_SPEED) - currentAbsY;
    }

    // 查询端点所在段只用于同锚点快速取 HS，跨锚点情况还需独立查询倍率。
    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        t,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    const ScrollSegment* seg = nullptr;
    if ( it == m_segments.begin() ) {
        seg = &m_segments.front();
    } else {
        seg = &(*std::prev(it));
    }

    // 同锚点直接复用当前段 HS；跨段端点仍使用指定锚点而非自身段的倍率。
    double absY = getAbsY(t);
    double hs =
        (std::abs(t - anchorTime) <= 1e-9) ? seg->hs : getHsAt(anchorTime);
    // 先减视觉原点再乘 HS；把两端各自乘不同 HS 会改变长条等物件的整体几何。
    // 零 HS 将距离压到原点，负 HS 反转距离方向，不在这里强行改成正值。
    return (absY - currentAbsY) * hs;
}

/// @brief 返回缓存是否记录过 Jump 时间点，不代表当前显示模式启用了跳变。
/// @return 上次重建记录的存在性标志，尚未重建的注册表修改不会立即反映在此。
/// @warning 热路径常量查询，不扫描段落。
bool ScrollCache::hasJumpEffects() const
{
    return m_hasJumpEffects;
}

/// @brief 判断给定时间跨度是否可使用线性插值快速路径。
/// @param startTime 候选区间起点，单位秒。
/// @param duration 必须为有限正数的区间长度，不是绝对结束时间。
/// @return 时间非法、跨段或接近跳变及微脉冲窗口时返回 false。
/// @note 这是能否使用快速路径的保守判定，不在函数内计算插值结果。
/// @note 返回 false 要求调用方走完整查询，不代表谱面时间本身不可用。
/// @warning 热路径查询可能扫描附近 Jump 段，不得在此重建索引。
bool ScrollCache::canInterpolateLinearly(double startTime,
                                         double duration) const
{
    if ( !std::isfinite(startTime) || !std::isfinite(duration) ||
         duration <= 0.0 ) {
        return false;
    }

    // 即使两个输入有限，相加仍可能溢出或因精度不足不能推进时间。
    const double endTime = startTime + duration;
    if ( !std::isfinite(endTime) || endTime <= startTime ) {
        return false;
    }

    auto nextSegmentIt = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        startTime,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    // 区间内部不能跨段；恰好位于终点的段界由后续跳变检查进一步判断。
    if ( nextSegmentIt != m_segments.end() && nextSegmentIt->time < endTime ) {
        return false;
    }

    if ( nextSegmentIt != m_segments.begin() ) {
        // 段起点上的 Jump 即使不处于查询区间内部，也会使本段拒绝简单插值。
        const auto& currentSegment = *std::prev(nextSegmentIt);
        if ( (currentSegment.effects & SCROLL_EFFECT_JUMP) != 0 ) {
            return false;
        }
    } else if ( !m_segments.empty() &&
                (m_segments.front().effects & SCROLL_EFFECT_JUMP) != 0 ) {
        // 查询早于首段时仍考虑首段跳变状态，不把向前外推视为无条件安全。
        return false;
    }

    // 向前多查一个跨度，保守排除刚经过瞬时跳变的插值区间。
    if ( m_hasJumpEffects ) {
        const double jumpQueryStart = startTime - duration;
        // 扩展范围来自调用者本次跨度，不是固定时间等待或人为延迟本地反馈。
        auto jumpIt =
            std::lower_bound(m_segments.begin(),
                             m_segments.end(),
                             jumpQueryStart,
                             [](const ScrollSegment& seg, double value) {
                                 return seg.time < value;
                             });

        for ( ; jumpIt != m_segments.end() && jumpIt->time <= endTime;
              ++jumpIt ) {
            // 终点上的 Jump 也拒绝插值，不能跨过瞬时不连续边界混合位置。
            if ( (jumpIt->effects & SCROLL_EFFECT_JUMP) != 0 ) {
                return false;
            }
        }
    }

    // 原始积分与视觉锚点在微脉冲窗口中不同，交叠时不能共用简单插值。
    if ( !m_microImpulseWindows.empty() ) {
        // 窗口按时间排列且不重叠，找到首个末端不早于起点的窗口即可判断相交。
        auto windowIt = std::lower_bound(
            m_microImpulseWindows.begin(),
            m_microImpulseWindows.end(),
            startTime,
            [](const MicroImpulseWindow& window, double value) {
                return window.endTime < value;
            });

        if ( windowIt != m_microImpulseWindows.end() &&
             windowIt->startTime < endTime ) {
            // 恰好在查询终点才开始的窗口不影响这次区间，故使用严格小于。
            return false;
        }
    }

    return true;
}

/// @brief 查询扩展时间区间内最大的 Jump 绝对时长，单位秒。
/// @param startTime 查询的一侧时间边界，允许晚于 endTime。
/// @param endTime 另一侧时间边界，函数内部规整先后顺序。
/// @param padding 两端额外扩展的时间，由调用方确定。
/// @pre 时间与 padding 为有限数；要扩展而非收缩范围时传入非负 padding。
/// @return 未命中 Jump 时返回零，不以活动速度估算跳变距离。
/// @warning 按时间顺序扫描缓存段落，到上界即停止，不访问谱面实体。
double ScrollCache::getMaxJumpSecondsInRange(double startTime, double endTime,
                                             double padding) const
{
    if ( startTime > endTime ) {
        std::swap(startTime, endTime);
    }

    const double queryStart = startTime - padding;
    const double queryEnd   = endTime + padding;
    // padding 与时间戳使用秒，只有 jumpValue 本身在读取时由毫秒换算。
    double maxJump = 0.0;

    for ( const auto& seg : m_segments ) {
        if ( seg.time < queryStart ) continue;
        // 时间有序，超过上界即可结束，不能对未排序容器套用这一提前退出。
        if ( seg.time > queryEnd ) break;
        if ( (seg.effects & SCROLL_EFFECT_JUMP) == 0 ) continue;
        // Jump 元数据以毫秒表达，取绝对值用于与跳变方向无关的范围扩展。
        maxJump = std::max(maxJump, std::abs(seg.jumpValue) / 1000.0);
    }

    return maxJump;
}

/// @brief 将位置窗口反投影为按时间排序、合并后的候选时间区间。
/// @param minAbsY 动画缩放空间中的一侧位置边界。
/// @param maxAbsY 另一侧位置边界，允许顺序相反。
/// @return 空缓存返回空集合，不使用默认速度推测范围。
/// @note 返回候选用于可见性裁剪，不是从单个位置选出唯一时间的 getTime 语义。
/// @note 停止段不进入位置反查索引，不能据此查询获得静止位置对应的全部时间。
/// @note 窗口不包含 HS 或判定线偏移，调用方须先还原到滚动位置空间。
/// @warning 此查询分配结果和候选容器并排序，不是逐物件常量查询。
std::vector<std::pair<double, double>> ScrollCache::getTimeRangesForAbsYWindow(
    double minAbsY, double maxAbsY) const
{
    std::vector<std::pair<double, double>> ranges;
    if ( m_segments.empty() ) return ranges;

    if ( minAbsY > maxAbsY ) {
        std::swap(minAbsY, maxAbsY);
    }
    minAbsY = toUnscaledAbsY(minAbsY);
    maxAbsY = toUnscaledAbsY(maxAbsY);
    // 反变换后再确认边界顺序，后面的相交比较统一假设下界不大于上界。
    if ( minAbsY > maxAbsY ) {
        std::swap(minAbsY, maxAbsY);
    }

    /// @brief 合并按时间顺序输入的重叠或相邻区间。
    /// @param startTime 候选时间下界，单个反向候选会先交换端点。
    /// @param endTime 候选时间上界。
    /// @note 修改局部输出向量，不改变缓存段落或空间索引。
    // 调用顺序必须保持时间递增，否则只与尾区间合并会遗漏交叠。
    auto appendRange = [&](double startTime, double endTime) {
        if ( startTime > endTime ) {
            std::swap(startTime, endTime);
        }
        if ( endTime < startTime - 1e-9 ) return;

        if ( !ranges.empty() && startTime <= ranges.back().second + 1e-6 ) {
            // 合并只扩展末端，保留先前区间的起点，避免包含关系使范围缩短。
            ranges.back().second = std::max(ranges.back().second, endTime);
            return;
        }

        ranges.emplace_back(startTime, endTime);
    };

    std::vector<std::size_t> candidateIndices;
    // 位置索引先排除下界已经越过窗口的条目，再检查各候选上界。
    auto rangeEndIt =
        std::upper_bound(m_absYRangeIndex.begin(),
                         m_absYRangeIndex.end(),
                         maxAbsY + 1e-6,
                         [](double value, const AbsYRangeEntry& entry) {
                             return value < entry.minAbsY;
                         });

    for ( auto it = m_absYRangeIndex.begin(); it != rangeEndIt; ++it ) {
        if ( it->maxAbsY < minAbsY - 1e-6 ) continue;
        candidateIndices.push_back(it->segmentIndex);
    }

    // 空间索引顺序可能因负 SV 与时间相反，先恢复段号顺序再合并区间。
    std::sort(candidateIndices.begin(), candidateIndices.end());
    candidateIndices.erase(
        // 同一段只反解一次，防止重复候选导致冗余区间及后续重复渲染。
        std::unique(candidateIndices.begin(), candidateIndices.end()),
        candidateIndices.end());

    for ( std::size_t i : candidateIndices ) {
        const auto& seg = m_segments[i];
        // 保留局部除法保护，不依赖索引构建阶段的过滤作为唯一前提。
        if ( std::abs(seg.speed) < 1e-9 ) continue;

        const bool hasNext = i + 1 < m_segments.size();
        // 先反解整条直线，再按实际段时域收窄，不把位置包围框当精确可见结果。
        // 负速度会交换两个反解时间的大小，统一取 min/max 后再裁剪。
        double t0           = seg.time + (minAbsY - seg.absY) / seg.speed;
        double t1           = seg.time + (maxAbsY - seg.absY) / seg.speed;
        double overlapStart = std::min(t0, t1);
        double overlapEnd   = std::max(t0, t1);

        if ( hasNext ) {
            double segEndTime = m_segments[i + 1].time;
            // 使用本段速度算终点，不能直接拿下一段可能已发生 Jump 的位置。
            double segEndAbsY = seg.absY + (segEndTime - seg.time) * seg.speed;
            // Jump 造成的空白位置不属于本段连续运动，需要在粗索引命中后排除。
            double segMinAbsY = std::min(seg.absY, segEndAbsY);
            double segMaxAbsY = std::max(seg.absY, segEndAbsY);

            if ( segMaxAbsY < minAbsY - 1e-6 || segMinAbsY > maxAbsY + 1e-6 ) {
                continue;
            }

            // 反解直线可以无限延伸，有限段必须裁回其有效时间范围。
            overlapStart = std::max(overlapStart, seg.time);
            overlapEnd   = std::min(overlapEnd, segEndTime);
        } else {
            // 尾段允许向未来延伸，但不能把该段速度反解出的过去时间也加入候选。
            overlapStart = std::max(overlapStart, seg.time);
        }

        if ( overlapEnd >= overlapStart - 1e-9 ) {
            // 容差保留接近边界的交集，后续像素裁剪负责去掉真正离屏的几何。
            appendRange(overlapStart, overlapEnd);
        }
    }

    return ranges;
}

/// @brief 拷贝段落并缩放副本中的位置和速度，保留原缓存及时间点语义。
/// @param out 被覆盖的输出容器，不会向旧内容后追加。
/// @note 导出的是原始段落副本，不将微脉冲窗口的线性替代写回这些段落。
/// @pre out 是调用方拥有的独立向量，不与内部段落容器别名。
/// @warning 快照导出涉及完整复制，调用方应复用输出容量并控制触发频率。
void ScrollCache::copyAnimatedSegmentsTo(std::vector<ScrollSegment>& out) const
{
    out = m_segments;
    // 单位倍率也必须先复制，不能因无需缩放而返回调用方残留的旧内容。
    if ( std::abs(m_animatedZoomScale - 1.0) <= 1e-9 ) {
        return;
    }

    // 时间、HS 和事件值不属于位置空间，不随动画比例改写。
    for ( auto& segment : out ) {
        // 位置和速度同时缩放，保持导出副本中的段内积分关系一致。
        segment.absY  = applyAnimatedZoomScale(segment.absY);
        segment.speed = applyAnimatedZoomScale(segment.speed);
    }
}

/// @brief 在滚动段边界处分割指定时间区间，不改变调用方起止范围。
/// @param startTime 切片起点，单位秒。
/// @param endTime 切片终点，单位秒；与起点相反时不自动交换。
/// @return 起点不早于终点时返回空集合，否则返回连续的非空切片。
/// @note 只按时间段边界拆分，不按可见窗口裁剪，也不按速度正负筛选切片。
/// @pre 两个输入时间为有限值，函数不将无穷区间展开为有限片段。
/// @warning 查询构造结果容器并遍历覆盖段落，不应逐帧重复查询不变区间。
std::vector<std::pair<double, double>> ScrollCache::getTimeSlices(
    double startTime, double endTime) const
{
    std::vector<std::pair<double, double>> slices;
    // 与位置窗口查询不同，本接口不接受逆序时间范围并自动修正。
    if ( startTime >= endTime ) return slices;

    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        startTime,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    // upper_bound 跳过恰好等于起点的段界，避免生成零长度首片。
    double current = startTime;
    while ( current < endTime ) {
        // 越过最后段界后仍补齐到请求终点；空缓存也得到一个完整切片。
        double nextTime = (it != m_segments.end()) ? it->time : endTime;
        if ( nextTime > endTime ) nextTime = endTime;
        if ( nextTime > current ) {
            // 跳过重合段界产生的零跨度，避免下游对空片段做除法或插值。
            slices.emplace_back(current, nextTime);
        }
        current = nextTime;
        // 消费段界后继续前进，剩余尾片仍由请求的 endTime 决定。
        if ( it != m_segments.end() ) ++it;
    }
    // 不对相邻片段合并，否则会抹去调用方需要的速度或效果切换边界。
    return slices;
}

}  // namespace MMM::Logic::System
