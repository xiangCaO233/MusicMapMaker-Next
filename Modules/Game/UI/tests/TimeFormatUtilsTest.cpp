/// @file TimeFormatUtilsTest.cpp
/// @brief 验证四位秒小数、累计分钟及共享时钟格式的数值边界。
#include "ui/utils/TimeFormatUtils.h"
#include "common/ClockTimeText.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"

#include <array>
#include <limits>
#include <string>
#include <string_view>

namespace
{
/// @brief 独立的输入与预期文本，不从生产计算结果反推预期值。
struct ClockCase {
    /// @brief 以秒表示的播放位置或持续时长。
    double m_seconds;
    /// @brief 手工给定的累计分钟与四位秒小数表示。
    std::string_view m_expected;
};

/// @brief 共同验证热路径无分配入口和 UI 字符串入口。
/// @return 两个入口在常规、进位、长时长和非法值上均符合预期时为真。
/// @details 数据包含跨一小时、一日以及原两位小时显示上限的持续时长。
/// 特别检查显示四位而不先截成毫秒，以及舍入后的负零抑制。
/// 不创建窗口或加载媒体，全部断言仅检查时间文本及数值范围保护。
bool testClock()
{
    // 边界值避开十进制浮点恰好半格的歧义，直接比较完整预期文本。
    // 极值预期依据 int64 最大整数的十进制分解，防止转换前溢出。
    const auto       invalid  = std::numeric_limits<double>::quiet_NaN();
    const auto       infinity = std::numeric_limits<double>::infinity();
    const std::array cases{
        ClockCase{ 0, "00:00.0000" },
        ClockCase{ 1.23456, "00:01.2346" },
        ClockCase{ -1.23456, "-00:01.2346" },
        // 从总精度整数拆分字段，使分钟边界和小时边界统一进位。
        // 秒字段不能显示为 60，跨小时后也不能重新生成小时列。
        ClockCase{ 59.99996, "01:00.0000" },
        ClockCase{ 3599.99996, "60:00.0000" },
        ClockCase{ 3600, "60:00.0000" },
        ClockCase{ 3723.45678, "62:03.4568" },
        ClockCase{ -3723.45678, "-62:03.4568" },
        // 超过一天和旧两位小时上限的时长继续累计分钟。
        // 分钟字段只有最小宽度，不能被 60 或显示字符数回卷。
        ClockCase{ 86400.12346, "1440:00.1235" },
        ClockCase{ 600000, "10000:00.0000" },
        // 不足半个显示单位的负时间在舍入后归零。
        // 最小可见正值则保留第四位，排除先转整数毫秒的精度损失。
        ClockCase{ -0.00001, "00:00.0000" },
        ClockCase{ 0.00006, "00:00.0001" },
        // 非有限数回退零，不能传给 llround 触发非法整数转换。
        // 只约束文本表示，不将占位数值写回播放器或谱面模型。
        ClockCase{ invalid, "00:00.0000" },
        ClockCase{ infinity, "00:00.0000" },
        // 即使输入本身有限，乘以精度比例也可能溢出到无穷。
        // 极值保护发生在转换之前，完整预期同时检查缓冲区未截断。
        ClockCase{ std::numeric_limits<double>::max(),
                   "15372286728091:17.5807" },
        ClockCase{ -std::numeric_limits<double>::max(),
                   "-15372286728091:17.5807" },
    };
    for ( const auto& item : cases ) {
        // Common 返回值保持存活再观察 data，UI 则按原有契约返回拥有的字符串。
        // 两种入口必须分别命中手工预期，而不只是彼此相等。
        // 这样可以发现共享实现同时出错或 UI 仍先做毫秒截断的情况。
        const auto buffer = MMM::Common::formatClockTime(item.m_seconds);
        const auto text =
            MMM::UI::Utils::TimeFormatDetail::formatClock(item.m_seconds);
        if ( std::string_view(buffer.data()) != item.m_expected ||
             text != item.m_expected ) {
            XERROR("Clock format mismatch: {} / {} / expected {}",
                   buffer.data(),
                   text,
                   item.m_expected);
            return false;
        }
    }
    return true;
}

/// @brief 验证普通秒和缺少 BPM 时的回退都使用四位，纯毫秒单位保持整数。
/// @return 不同偏好各自保留语义且共享显示精度时为真。
/// @details 拍号模式在无快照时只能回退秒，不应凭空构造拍位。
/// 时间精度调整不改变纯毫秒偏好的整数单位，也不重新解释保存的枚举值。
bool testPreferences()
{
    // 显式传偏好避免读取用户配置，已有 Clock/Seconds 枚举存储身份保持不变。
    // 单位模式分别校验，不把纯毫秒整数显示误变成秒的小数。
    using MMM::Config::TimeFormatPreference;
    using MMM::UI::Utils::TimeFormatDetail::formatTimeWithPreference;
    // 以明确原始秒值覆盖正负号和最后一位四舍五入，不依赖当前个人配置。
    return formatTimeWithPreference(12.34567,
                                    TimeFormatPreference::Seconds,
                                    nullptr) == "12.3457 s" &&
           formatTimeWithPreference(-12.34567,
                                    TimeFormatPreference::Seconds,
                                    nullptr) == "-12.3457 s" &&
           formatTimeWithPreference(
               12.34567, TimeFormatPreference::Beat, nullptr) == "12.3457 s" &&
           formatTimeWithPreference(12.34567,
                                    TimeFormatPreference::Clock,
                                    nullptr) == "00:12.3457" &&
           formatTimeWithPreference(12.34567,
                                    TimeFormatPreference::Milliseconds,
                                    nullptr) == "12346 ms";
}
}  // namespace

/// @brief 运行不依赖窗口、音频和文件系统的时间显示回归。
/// @return 任一入口格式或边界处理不符合预期时返回非零。
int main()
{
    // 不写文件、不需要计时等待；返回值是本组回归的唯一成功依据。
    // 实际判定线绘制及批注导出另由各模块既有测试检查最终入口。
    return testClock() && testPreferences() ? 0 : 1;
}
