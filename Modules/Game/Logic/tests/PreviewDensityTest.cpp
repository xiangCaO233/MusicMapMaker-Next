#include "logic/PreviewDensity.h"

#include "log/colorful-log.h"

#include <array>
#include <cmath>
#include <vector>

namespace
{

/// @brief 使用小容差比较密度时间元数据。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
/// @note 只比较秒单位元数据，离散物件计数仍使用精确相等。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-9;
}

/// @brief 验证空谱面不会生成无意义样本。
/// @return 行为符合预期时返回 true。
bool testEmptyChart()
{
    // 即使给出有效音频长度，没有物件也不应分配一串无意义的零计数桶。
    const auto density =
        MMM::Logic::buildPreviewDensitySnapshot({}, 120.0, 2.0, 0.25, 512);
    // 空计数与有效时长可以同时存在，消费者仍需知道时间轴覆盖范围。
    if ( !density.counts.empty() || density.maxCount != 0 ||
         !near(density.duration, 120.0) ) {
        XERROR("Preview density empty-chart behavior changed");
        return false;
    }
    return true;
}

/// @brief 验证两秒滑窗会在相邻采样点持续覆盖密集物件。
/// @return 行为符合预期时返回 true。
bool testFixedSlidingWindow()
{
    // 时间按升序提供，被测接口不负责排序；前部三个物件形成密集簇。
    constexpr std::array<double, 5> TIMES{ 0.2, 0.8, 1.2, 2.8, 4.9 };
    const auto                      density =
        MMM::Logic::buildPreviewDensitySnapshot(TIMES, 5.0, 2.0, 1.0, 16);
    // 5 秒按 1 秒间隔得到五个桶，采样中心依次为 0.5、1.5、2.5、3.5、4.5。
    // 两秒窗口在首尾整体平移到范围内，不能仅截断后缩短统计时长。
    // 预期窗口分别为 [0,2]、[0.5,2.5]、[1.5,3.5]、[2.5,4.5]、[3,5]。
    const std::vector<std::uint32_t> expected{ 3, 2, 1, 1, 1 };
    if ( density.counts != expected || density.maxCount != 3 ||
         !near(density.sampleInterval, 1.0) ||
         !near(density.windowDuration, 2.0) ) {
        XERROR("Preview density fixed-window counts changed");
        return false;
    }
    return true;
}

/// @brief 验证长谱面会限制样本数量并覆盖末尾物件。
/// @return 行为符合预期时返回 true。
bool testLongChartBinLimit()
{
    // 首尾各放一个孤立物件，避免内部密集簇掩盖采样空隙丢失物件的问题。
    constexpr std::array<double, 2> TIMES{ 0.0, 599.9 };
    const auto                      density =
        MMM::Logic::buildPreviewDensitySnapshot(TIMES, 600.0, 2.0, 0.25, 64);
    // 0.25 秒偏好原本需要 2400 桶，上限要求重新均分全长而非截掉尾部。
    // 实际间隔 9.375 秒大于两秒窗口，首桶依赖稀疏峰值补充保留零时刻物件。
    if ( density.counts.size() != 64 || density.counts.front() != 1 ||
         density.counts.back() != 1 ||
         !near(density.sampleInterval, 600.0 / 64.0) ) {
        XERROR("Preview density long-chart bin limiting changed");
        return false;
    }
    return true;
}

/// @brief 验证物件末尾能够扩展无音轨谱面的密度时间轴。
/// @return 行为符合预期时返回 true。
bool testObjectTimeExtendsDuration()
{
    // 总时长传零模拟无音轨场景，最后一个物件应提供可用的时间轴范围。
    constexpr std::array<double, 1> TIMES{ 9.5 };
    const auto                      density =
        MMM::Logic::buildPreviewDensitySnapshot(TIMES, 0.0, 2.0, 0.5, 64);
    // 物件恰好位于推导出的总时长边界，末窗口必须包含右端点。
    if ( !near(density.duration, 9.5) || density.counts.empty() ||
         density.counts.back() != 1 ) {
        XERROR("Preview density object-duration fallback changed");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行预览密度缓存纯逻辑测试。
/// @return 全部测试通过时返回 0。
/// @note 测试只覆盖纯统计快照，不证明预览画布颜色或几何绘制正确。
int main()
{
    // 每个用例独立构造输入，没有跨用例缓存，也无需外部谱面或音频文件。
    // 失败用例打印具体类别，非零退出码让直接运行与测试运行器得到相同结论。
    // 保留先验证基础窗口、再验证样本上限与时长后备的短路顺序。
    return testEmptyChart() && testFixedSlidingWindow() &&
                   testLongChartBinLimit() && testObjectTimeExtendsDuration()
               ? 0
               : 1;
}
