#include "canvas/ComposeHoldTarget.h"
#include "canvas/ComposePolylineTarget.h"
#include "canvas/ComposeTargetEligibility.h"
#include <array>
#include <cstdint>
#include <limits>
#include <span>

/// @brief 验证长条始终邻近上一单键、异轨且首尾取自真实可见拍线。
/// @return 任一时间、轨道或不可用边界违反契约时返回非零。
/// @note 故意打乱拍线时间顺序，覆盖反向滚动和可见区间分段后的候选集合。
/// @note 时间采用非零偏移及三分拍，避免零起点四分拍掩盖重新量化的错误。
/// @note 负时间回归同时检查公共候选门禁与长条选择器，前者也供单键使用。
/// @note Flick 的同拍横移端点来自长条选择器的附近拍位，必须继承相同边界。
/// @note 新增折线目标同样只使用可见拍线，测试打乱输入次序避免依赖排序。
/// @note 双轨时验证 Hold 走 Flick 头的时间相反侧，不仅比较终点。
/// @note 多轨时验证换轨后可以继续使用最近后继，不牺牲教程可见性。
/// @note 若同轨相反侧不可见，必须保持无目标而不是穿过 Flick。
/// @note 折线末段两个同轨检查点共享一个最终子音符的几何语义。
/// @note 返回的时间必须取自输入拍线，不接受按固定四分拍重算。
/// @note 已放置 Hold 时重复不同随机种子，确认整条路径都避开它的轨道。
/// @note 双轨可用时间不足时应无目标，以免退化到重叠路径。
int main()
{
    using MMM::Canvas::chooseComposeHoldTarget;
    using MMM::Canvas::isComposeTargetBeatLine;
    using MMM::Canvas::isComposeTargetTime;
    using Line = MMM::Common::Render::PlayerBeatLineSnapshot;
    // 四条线同时提供前驱、同拍和两个后继，断言能区分最近与任意后继。
    // 非整数秒直接从输入返回，测试比较精确值以发现额外量化或四舍五入。
    const std::array lines{
        Line{ 1.337 + 1.0 / 3.0, 200.0F },
        Line{ 1.337 - 1.0 / 3.0, 400.0F },
        Line{ 1.337, 300.0F },
        Line{ 1.337 + 2.0 / 3.0, 100.0F },
    };
    // 全部轨数及随机种子都应排除原轨；首尾保持在最近的两条真实拍线上。
    // 起点固定与单键同拍，不因随机种子改变附近时间的定义。
    for ( int count = 2; count <= 8; ++count ) {
        for ( int track = 0; track < count; ++track ) {
            // 覆盖原轨位于首、中、末位置，防止非零偏移取模后绕回原轨。
            for ( unsigned seed = 0; seed < 16; ++seed ) {
                const auto target = chooseComposeHoldTarget(
                    lines, 1.337, track, count, 20.0F, 0.0F, 500.0F, seed);
                if ( !target || target->track == track || target->track < 0 ||
                     target->track >= count ||
                     target->startTime != lines[2].time ||
                     target->endTime != lines[0].time )
                    return 1;
            }
        }
    }
    // 原拍位位于视口上部时，选前一拍作为头部、原拍位作为尾部。
    // 这要求时间保持递增，不能简单交换屏幕坐标后制造负时长。
    const auto backward =
        chooseComposeHoldTarget(lines, 1.337, 1, 4, 20.0F, 250.0F, 500.0F, 3);
    if ( !backward || backward->startTime != lines[1].time ||
         backward->endTime != lines[2].time )
        return 2;
    // 多轨时直接换到 Flick 头以外的玩家轨，保持原先的近邻时间目标。
    const auto afterFlick = chooseComposeHoldTarget(
        lines, 1.337, 1, 4, 20.0F, 0.0F, 500.0F, 3, lines[0].time, 2);
    if ( !afterFlick || afterFlick->track == 1 || afterFlick->track == 2 ||
         afterFlick->endTime != lines[0].time )
        return 10;
    // Flick 从 1 轨横跨到 4 轨时，中间的 2、3 轨也被连接体占用。
    // 原先只排除起点轨和单键轨，会错误地把 2、3 轨当成空轨。
    const auto clearFlickSpan = chooseComposeHoldTarget(
        lines, 1.337, 4, 6, 20.0F, 0.0F, 500.0F, 3, lines[0].time, 1, 4);
    // 允许横向范围两侧的空轨，不把避让条件误写成固定选第一轨。
    if ( !clearFlickSpan ||
         (clearFlickSpan->track != 0 && clearFlickSpan->track != 5) ||
         clearFlickSpan->endTime != lines[0].time )
        return 20;
    // Flick 横跨全部轨道时不存在空轨；必须回到其时间相反侧。
    const auto allTracksOccupied = chooseComposeHoldTarget(
        lines, 1.337, 2, 3, 20.0F, 0.0F, 500.0F, 3, lines[0].time, 0, 2);
    if ( !allTracksOccupied || allTracksOccupied->startTime != lines[1].time ||
         allTracksOccupied->endTime != lines[2].time )
        return 21;
    // 两轨无法换轨时改走 Flick 头相反侧，身体也不经过已有 Flick。
    const auto beforeFlick = chooseComposeHoldTarget(
        lines, 1.337, 0, 2, 20.0F, 0.0F, 500.0F, 3, lines[0].time, 1);
    if ( !beforeFlick || beforeFlick->startTime != lines[1].time ||
         beforeFlick->endTime != lines[2].time )
        return 11;
    // 若相反侧没有完整可见的拍线，就等待视野变化而不强行穿过 Flick。
    if ( chooseComposeHoldTarget(
             lines, 1.337, 0, 2, 20.0F, 0.0F, 350.0F, 3, lines[0].time, 1) )
        return 15;
    // Flick 头在过去拍位时，两轨 Hold 改向下一拍延伸。
    const auto forwardFromFlick = chooseComposeHoldTarget(
        lines, 1.337, 0, 2, 20.0F, 0.0F, 500.0F, 3, lines[1].time, 1);
    if ( !forwardFromFlick || forwardFromFlick->startTime != lines[2].time ||
         forwardFromFlick->endTime != lines[0].time )
        return 16;
    // 反向滚动下时间增长对应 Y 增长；选择仍遵循时间，路径自然向下。
    auto reversed = lines;
    // 只反转屏幕位置，保留时间和输入顺序，隔离坐标方向对算法的影响。
    for ( auto& line : reversed ) line.y = 500.0F - line.y;
    const auto reverse =
        chooseComposeHoldTarget(reversed, 1.337, 0, 4, 20.0F, 0.0F, 500.0F, 0);
    if ( !reverse || reverse->startTime != lines[2].time ||
         reverse->endTime != lines[0].time )
        return 3;
    // 单轨、缺失原拍位、端点被裁剪或没有足够拖动空间，都不能凭空造目标。
    // 特别是缺失原拍位时，不能把视野中无关位置当作单键附近。
    // 裁剪用例的中心正好在边界上，只有完整尺寸检查才会拒绝它。
    // 大尺寸用例仍有可见中心，但端点之间的间隔不足以展示拖动路径。
    if ( chooseComposeHoldTarget(lines, 1.337, 0, 1, 20, 0, 500, 0) ||
         chooseComposeHoldTarget(lines, 8.0, 0, 4, 20, 0, 500, 0) ||
         chooseComposeHoldTarget(lines, 1.337, 0, 4, 20, 300, 500, 0) ||
         chooseComposeHoldTarget(lines, 1.337, 0, 4, 150, 0, 500, 0) )
        return 4;
    // 单键筛选与 Shift 物件共用门禁；负拍线即使完全可见也不允许作为任一端点。
    // 极小负数也必须拒绝，不能被缓存匹配的时间容差误认为零。
    // -0.034 对应可见拍线落在起播点之前的实际失败场景。
    // NaN 会穿过单纯大小比较，正无穷会穿过单纯非负检查，分别覆盖。
    for ( const double time : { -0.034,
                                -1e-9,
                                std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity() } ) {
        if ( isComposeTargetTime(time) ||
             isComposeTargetBeatLine(time, 300, 20, 0, 500) )
            return 5;
    }
    // 零秒是合法边界；正的首拍偏移也应原样保留，而非额外移到下一拍。
    // 不能通过把整个零附近区间排除来规避负数，否则合法首拍将无法练习。
    if ( !isComposeTargetBeatLine(0.0, 300, 20, 0, 500) ||
         !isComposeTargetBeatLine(0.034, 300, 20, 0, 500) )
        return 6;
    const std::array crossingZero{ Line{ -0.034, 400.0F },
                                   Line{ 0.0, 300.0F },
                                   Line{ 0.466, 200.0F } };
    // 负参考禁止吸附到零；零参考则只能选正后继，不能回退到负前驱。
    // 将极小负参考与真实零拍放在同一个集合中，专门暴露容差匹配漏洞。
    if ( chooseComposeHoldTarget(crossingZero, -1e-9, 0, 4, 20, 0, 500, 0) )
        return 7;
    const auto nonnegative =
        chooseComposeHoldTarget(crossingZero, 0.0, 0, 4, 20, 0, 500, 0);
    if ( !nonnegative || nonnegative->startTime != 0.0 ||
         nonnegative->endTime != 0.466 )
        return 8;
    // 只有零拍和负前驱可见时，不生成非法长条，也不供 Flick 借用负拍位。
    // 这里缩小可见区间而不修改时间数组，模拟滚动后只剩负时间候选。
    // 拒绝生成比钳制前驱为零更安全：后者会制造没有拖动跨度的伪路径。
    if ( chooseComposeHoldTarget(crossingZero, 0.0, 0, 4, 20, 250, 500, 0) )
        return 9;
    // 折线路线从打乱的真实拍线中保持时间递增，并明确显示末段延长检查点。
    // 五个拍位映射到七个操作检查点，后两点共用轨道构成第五个子段。
    // 轨道交替必须发生两次；否则看似七点却可能只清洗出长条。
    const std::array polylineLines{
        Line{ 3.0, 300.0F }, Line{ 1.0, 500.0F }, Line{ 5.0, 100.0F },
        Line{ 2.0, 400.0F }, Line{ 4.0, 200.0F },
    };
    const auto route = MMM::Canvas::chooseComposePolylineTarget(
        polylineLines, 4, 20.0F, 0.0F, 600.0F, 0);
    if ( !route ) return 12;
    const auto& points = route->waypoints;
    // 相邻横向点时间相等，纵向点时间严格增长。
    // 第六、第七点仍同轨，不引入第六段新类型。
    if ( points[0].time != 1.0 || points[1].time != 2.0 ||
         points[2].time != points[1].time || points[3].time != 3.0 ||
         points[4].time != points[3].time || points[5].time != 4.0 ||
         points[6].time != 5.0 || points[0].track != points[1].track ||
         points[1].track == points[2].track ||
         points[2].track != points[3].track ||
         points[3].track == points[4].track ||
         points[4].track != points[5].track ||
         points[5].track != points[6].track )
        return 13;
    // 先前的 Hold 即使与路线使用同一组拍位，也不能被折线覆盖。
    // 四轨以上整条路线不使用 Hold 轨；种子变化也不得绕开这项约束。
    // Hold 处于路线选中时间内，确保轨道避让承担实际防重叠作用。
    const MMM::Canvas::ComposeHoldTarget placedHold{ 1, 2.0, 4.0 };
    // 低位和高位种子会分别改变第一轨与第二轨的选择。
    // 检查所有节点，覆盖同轨纵向段和两次横向转折的两侧。
    // 编辑章节还会横移中间 Hold 和整段后缀，因此路线需预留可行方向。
    // 位移后不能让中间 Hold 回到前一 Flick 起点，否则会提前发生合并。
    // 用相同轨道公式复查每个随机种子，避免偶发的边界组合卡住教程。
    const auto canMoveMiddle = [](int source, int other, int laneCount) {
        // 目标是保留结构的局部移动，两端都必须继续位于玩家轨道内。
        // 中段落回源轨会让前 Flick 消失，应留给后面的合并练习。
        for ( const int delta : { -1, 1 } ) {
            if ( source + delta >= 0 && source + delta < laneCount &&
                 other + delta >= 0 && other + delta < laneCount &&
                 other + delta != source )
                return true;
        }
        return false;
    };
    for ( std::uint64_t seed = 0; seed < 24; ++seed ) {
        const auto clearRoute = MMM::Canvas::chooseComposePolylineTarget(
            polylineLines, 4, 20.0F, 0.0F, 600.0F, seed, placedHold);
        if ( !clearRoute ) return 15;
        for ( const auto& point : clearRoute->waypoints ) {
            if ( point.track == placedHold.track ) return 16;
        }
        if ( !canMoveMiddle(clearRoute->waypoints[0].track,
                            clearRoute->waypoints[2].track,
                            4) )
            return 20;
    }
    // 三轨且旧 Hold 占中轨时，两条剩余外轨无法共同横移。
    // 允许路线借用中轨，但所有拍位必须与旧 Hold 时间区间分开。
    // 这验证了编辑可行性与此前避免物件重叠的约束同时成立。
    const std::array threeTrackLines{
        Line{ 1.0, 540.0F }, Line{ 2.0, 500.0F }, Line{ 3.0, 460.0F },
        Line{ 4.0, 420.0F }, Line{ 5.0, 380.0F }, Line{ 6.0, 340.0F },
        Line{ 7.0, 300.0F }, Line{ 8.0, 260.0F }, Line{ 9.0, 220.0F },
    };
    const MMM::Canvas::ComposeHoldTarget middleHold{ 1, 1.0, 4.0 };
    const auto threeTrackRoute = MMM::Canvas::chooseComposePolylineTarget(
        threeTrackLines, 3, 20.0F, 0.0F, 600.0F, 0, middleHold);
    // 原 Hold 覆盖 1～4 秒，安全路线应从 5 秒或更晚开始。
    // 路线可以借用中轨，但不能在相同拍位覆盖原 Hold。
    // 两条外轨构成不可移动的组合时，候选筛选必须改选轨道对。
    if ( !threeTrackRoute || !canMoveMiddle(threeTrackRoute->waypoints[0].track,
                                            threeTrackRoute->waypoints[2].track,
                                            3) )
        return 21;
    for ( const auto& point : threeTrackRoute->waypoints ) {
        // 七个检查点都检查，首尾安全不能代替中间点安全。
        if ( point.time <= middleHold.endTime ) return 22;
    }
    // 双轨无法避开 Hold 轨道，五段路径应全部位于其时间区间外。
    // 可见拍位不足时须暂停提示，不能让路线从 Hold 身体上穿过去。
    // 前四拍和 Hold 重叠，后五拍刚好足够组成完整路线。
    // 各拍位屏幕间距达到两个 Note 高度，失败只能归因于避让条件。
    const std::array twoTrackLines{
        Line{ 1.0, 540.0F }, Line{ 2.0, 500.0F }, Line{ 3.0, 460.0F },
        Line{ 4.0, 420.0F }, Line{ 5.0, 380.0F }, Line{ 6.0, 340.0F },
        Line{ 7.0, 300.0F }, Line{ 8.0, 260.0F }, Line{ 9.0, 220.0F },
    };
    const MMM::Canvas::ComposeHoldTarget twoTrackHold{ 0, 1.0, 4.0 };
    const auto afterHold = MMM::Canvas::chooseComposePolylineTarget(
        twoTrackLines, 2, 20.0F, 0.0F, 600.0F, 0, twoTrackHold);
    // 检查七个操作点，而非只检查第一个与最后一个端点。
    if ( !afterHold ) return 17;
    for ( const auto& point : afterHold->waypoints ) {
        if ( point.time <= twoTrackHold.endTime ) return 18;
    }
    // 移除最后两拍后只剩三个安全拍位，不能从 Hold 区间借拍。
    // 这同时覆盖路线不足时返回空值，而不是生成不完整的七点提示。
    if ( MMM::Canvas::chooseComposePolylineTarget(
             std::span(twoTrackLines).first(7),
             2,
             20.0F,
             0.0F,
             600.0F,
             0,
             twoTrackHold) )
        return 19;
    // 少于五条完整可见拍线或只有一轨时不能虚构教学目标。
    // 边界裁剪用已存在的线实现，避免把测试变成另一套伪投影。
    if ( MMM::Canvas::chooseComposePolylineTarget(
             polylineLines, 1, 20.0F, 0.0F, 600.0F, 0) ||
         MMM::Canvas::chooseComposePolylineTarget(
             polylineLines, 4, 20.0F, 150.0F, 600.0F, 0) )
        return 14;
    // 高频场景只借用输入 span；不要求渲染设备、谱面实体或磁盘测试资源。
    // 通过生产 helper 验证几何约束，鼠标与命令链由画布集成测试另行覆盖。
    return 0;
}
