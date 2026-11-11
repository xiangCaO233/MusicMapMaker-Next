#include "graphic/mobile/MobileTouchInput.h"
#include <limits>

/// @brief 验证实际生产跟踪器的身份、取消和有界容量，不依赖 SDL 或图形设备。
/// @details 手势结束后仍需保留最高指头数，才能识别同一帧内完成的双指轻点。
/// 所有时间都是固定输入，测试不使用 sleep 或读取真实设备状态。
/// @return 任一身份或生命周期契约被破坏时返回非零。
int main()
{
    using namespace MMM::Graphic;
    MobileTouchTracker input;
    // 相同手指编号可以来自不同设备，不能只用 finger 作为唯一键。
    // 重复按下同一身份只更新位置，不能增加活跃触点数量。
    // 下方两设备使用相同 finger 编号，故意覆盖容易被鼠标合成掩盖的情况。
    input.update(TouchPhase::Down, 1, 8, .2f, .3f, 1);
    input.update(TouchPhase::Down, 2, 8, .7f, .3f, 1.01);
    input.update(TouchPhase::Down, 2, 8, .8f, .3f, 1.02);
    if ( input.snapshot().m_count != 2 || input.snapshot().m_peak != 2 )
        return 1;
    // 双指起点冻结在第二指加入时，后续移动不能改写导航参照。
    // 最大位移保留已发生的拖动，手指回到起点也不能误识别为轻点。
    if ( input.snapshot().m_pairStart[0].m_x != .2f ||
         input.snapshot().m_pairStart[1].m_x != .7f ||
         input.snapshot().m_motionX < .099f )
        return 11;
    // 首指抬起后保留第二指真实身份，不能把它当成新的单指编辑。
    input.update(TouchPhase::Up, 1, 8, .2f, .3f, 1.03);
    if ( input.snapshot().m_count != 1 ||
         input.snapshot().m_contacts[0].m_device != 2 ||
         input.snapshot().m_peak != 2 )
        return 2;
    // 模拟同一事件泵内就抬起所有手指，不能依赖下一帧还有活跃点。
    // 即使 count 已归零，双指信息仍需保留给菜单和撤销仲裁。
    input.update(TouchPhase::Up, 2, 8, .8f, .3f, 1.04);
    if ( input.snapshot().m_count != 0 || input.snapshot().m_peak != 2 )
        return 3;
    // 消费方可依靠代际区分两次操作，而非使用坐标或固定时间窗口猜测。
    // 保留旧 epoch 用来确认新按下确实开始了独立手势。
    const auto epoch = input.snapshot().m_epoch;
    // 真正的新按下才重置 peak 和原点，普通空闲不生成假代际。
    input.update(TouchPhase::Down, 1, 9, .4f, .5f, 2);
    if ( input.snapshot().m_epoch != epoch + 1 ||
         input.snapshot().m_peak != 1 || input.snapshot().m_originX != .4f )
        return 4;
    // 系统取消可以不再补发 Up；新手势仍可正常开始。
    input.cancel();
    if ( input.snapshot().m_count || !input.snapshot().m_cancelled ) return 5;
    // 取消后的迟到移动不能复活旧手指，也不能等待超时才解除状态。
    // 恢复是否成功取决于后续真实 Down，而不是人工补发一次 Up。
    input.update(TouchPhase::Move, 1, 9, .6f, .6f, 2.1);
    if ( input.snapshot().m_count ) return 6;
    input.update(TouchPhase::Down, 1, 10, .4f, .5f, 3);
    if ( input.snapshot().m_cancelled || input.snapshot().m_count != 1 )
        return 7;
    // 超容量必须取消整次手势，不能越界写入或提前接受新编辑。
    input.cancel();
    // 十个活跃槽位以外的事件仍须安全收尾，测试不会读取越界数组项。
    // 先放下全部手指再依次抬起，覆盖已知点与溢出点共同存在的阶段。
    for ( unsigned i = 0; i < 12; ++i )
        input.update(TouchPhase::Down, 1, i, .5f, .5f, 4);
    if ( input.snapshot().m_count != 10 || !input.snapshot().m_cancelled )
        return 8;
    for ( unsigned i = 0; i < 12; ++i )
        input.update(TouchPhase::Up, 1, i, .5f, .5f, 4.1);
    // 所有正常与溢出手指抬起后，新手势应恢复，而不是永久保留取消标记。
    // 这里不调用 cancel 清理，避免掩盖溢出计数没有正确归零的实现。
    input.update(TouchPhase::Down, 1, 20, .5f, .5f, 5);
    if ( input.snapshot().m_count != 1 || input.snapshot().m_cancelled )
        return 9;
    // 非有限输入不能污染后续距离和缩放计算，直接取消而非继续使用坏坐标。
    input.update(TouchPhase::Move,
                 1,
                 20,
                 std::numeric_limits<float>::quiet_NaN(),
                 .5f,
                 5.1);
    if ( !input.snapshot().m_cancelled || input.snapshot().m_count ) return 10;
    return 0;
}
