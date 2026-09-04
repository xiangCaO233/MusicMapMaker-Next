#include "mmm/timing/BpmNormalization.h"

#include "log/colorful-log.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace
{

/// @brief 检查 BPM 规范化是否保留正常值并按原值方向收敛越界值。
/// @return 所有有限值与无穷值均落到预期边界时返回 true。
bool testBoundaryNormalization()
{
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
    const double nan = std::numeric_limits<double>::quiet_NaN();
    return MMM::normalizeBpmValue(nan) == MMM::DEFAULT_NORMALIZED_BPM &&
           MMM::normalizeBpmValue(nan, 240.0) == 240.0 &&
           MMM::normalizeBpmValue(nan, 20000.0) == MMM::MAX_NORMALIZED_BPM &&
           MMM::normalizeBpmValue(nan, nan) == MMM::DEFAULT_NORMALIZED_BPM;
}

}  // namespace

int main()
{
    if ( !testBoundaryNormalization() || !testNanFallbackNormalization() ) {
        XERROR("BPM normalization semantics do not match the safe range");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
