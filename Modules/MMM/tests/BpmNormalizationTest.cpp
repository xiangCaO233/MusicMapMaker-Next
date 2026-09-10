#include "mmm/timing/BpmNormalization.h"

#include "log/colorful-log.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace
{

/// @file BpmNormalizationTest.cpp
/// @brief 固化 BPM 异常值向最近合法边界收敛的语义。
///
/// 有限越界值和正负无穷按原值方向夹取；NaN 没有可比较方向，先使用调用者
/// 回退值，再对回退值执行相同边界规范化。
/// 测试使用精确边界常量，避免把旧的固定 120 BPM 回退语义重新引入。
/// 正常区间内部的 120 BPM 用作不应发生变化的控制样本。

/// @brief 检查 BPM 规范化是否保留正常值并按原值方向收敛越界值。
/// @return 所有有限值与无穷值均落到预期边界时返回 true。
bool testBoundaryNormalization()
{
    // 同时覆盖区间内部、上下越界与两种方向的无穷值。
    return MMM::normalizeBpmValue(120.0) == 120.0 &&
           MMM::normalizeBpmValue(10001.0) == MMM::MAX_NORMALIZED_BPM &&
           MMM::normalizeBpmValue(std::numeric_limits<double>::infinity()) ==
               MMM::MAX_NORMALIZED_BPM &&
           MMM::normalizeBpmValue(0.0) == MMM::MIN_NORMALIZED_BPM &&
           MMM::normalizeBpmValue(-1.0) == MMM::MIN_NORMALIZED_BPM &&
           MMM::normalizeBpmValue(-std::numeric_limits<double>::infinity()) ==
               MMM::MIN_NORMALIZED_BPM;
}

/// @brief 检查 NaN 使用回退值且回退值本身仍遵循边界语义。
/// @return 默认、自定义和异常回退值均得到确定结果时返回 true。
bool testNanFallbackNormalization()
{
    // 回退值本身可能越界或仍为 NaN，因此需要二次规范化而非直接返回。
    const double nan = std::numeric_limits<double>::quiet_NaN();
    return MMM::normalizeBpmValue(nan) == MMM::DEFAULT_NORMALIZED_BPM &&
           MMM::normalizeBpmValue(nan, 240.0) == 240.0 &&
           MMM::normalizeBpmValue(nan, 20000.0) == MMM::MAX_NORMALIZED_BPM &&
           MMM::normalizeBpmValue(nan, nan) == MMM::DEFAULT_NORMALIZED_BPM;
}

}  // namespace

/// @brief 运行 BPM 边界与 NaN 回退语义测试。
/// @return 任一规范化结果偏离契约时返回失败。
int main()
{
    if ( !testBoundaryNormalization() || !testNanFallbackNormalization() ) {
        XERROR("BPM normalization semantics do not match the safe range");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
