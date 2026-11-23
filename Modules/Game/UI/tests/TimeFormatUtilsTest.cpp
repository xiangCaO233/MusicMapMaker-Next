/// @file TimeFormatUtilsTest.cpp
/// @brief 验证四位秒小数、累计分钟及共享时钟格式的数值边界。
#include "ui/utils/TimeFormatUtils.h"
#include "common/ClockTimeText.h"
#include "common/render/BeatTimeIndex.h"
#include "common/render/RenderSnapshot.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"
#include "ui/utils/FileModifiedTime.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
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
/// @brief 仅有 BPM 索引的快照仍能格式化前奏、变速边界和分拍。
/// @return 所有标签与分拍配置均消费同一修订，且无需完整滚动数据时为真。
/// @details 累计值由生产者提供，此用例只验证 UI 对区间的选择和格线量化。
/// 构造不带 ScrollSegment 的快照，模拟预览等无需完整滚动映射的视口。
bool testSnapshotBeatTime()
{
    using MMM::Common::Render::BeatTimeIndex;
    using MMM::Common::Render::RenderSnapshot;
    using MMM::UI::Utils::TimeFormatDetail::formatBeatTime;
    auto index = std::make_shared<BeatTimeIndex>();
    // 首段从负时间开始，三段分别用 120、60、240 BPM。
    // 前一段完成三拍，第二段完成两拍；累计值来自生产端。
    index->m_points = { { -0.5, 120.0, 0 },
                        { 1.0, 60.0, 3 },
                        { 3.0, 240.0, 5 } };
    // 单个索引承载全部标签，输入时刻改变时仍共享相同的节点列表。
    RenderSnapshot snapshot;
    snapshot.m_beatTimeIndex    = index;
    snapshot.currentBeatDivisor = 4;
    // 首节点前的整拍、分拍进位、同刻进入新段及变化后的分数分别校验。
    // 跨节点的期望值显式计算，用来区分选错区间与段内舍入错误。
    // 接近负时间首段的拍尾时，分拍舍入仍要进到下一拍。
    const std::array cases{
        ClockCase{ -1.0, "-1 + 0/1" },  ClockCase{ -0.5, "1 + 1/1" },
        ClockCase{ -0.375, "1 + 1/4" }, ClockCase{ -0.03, "2 + 1/1" },
        ClockCase{ 1.0, "4 + 1/1" },    ClockCase{ 1.5, "4 + 1/2" },
        ClockCase{ 3.0, "6 + 1/1" },    ClockCase{ 3.125, "6 + 1/2" },
    };
    for ( const auto& item : cases ) {
        const auto text = formatBeatTime(item.m_seconds, &snapshot);
        if ( text != item.m_expected ) {
            XERROR("Beat format mismatch: {} / expected {}",
                   text,
                   item.m_expected);
            return false;
        }
    }
    // 分拍偏好独立于 BPM 修订，更新查询参数即可看到变化。
    // 八分拍偏好不产生新 BPM 修订，仅改变段内格线的量化。
    snapshot.currentBeatDivisor = 8;
    return formatBeatTime(3.03125, &snapshot) == "6 + 1/8" &&
           snapshot.scrollSegments.empty();
}
/// @brief 验证文件时钟转换和本地时间接口的成功及失败路径。
/// @return 普通时刻、纪元边界和夏令时切换符合平台契约时为真。
/// @details POSIX 用例只改变当前测试进程时区，避免影响用户配置和其他测试。
/// 直接验证 time_t 的日历边界，再通过业务入口验证文件时钟和本地时间映射。
/// 两部分可分别定位平台历法转换失败和文件时钟换算的偏移错误。
bool testFileModifiedTime()
{
    // 构造 file_time_type 值而不创建实际文件，因此不会更改资源修改时间。
    // C 时钟的失败用例也不受文件系统可储存日期范围限制。
    using FileClock = std::filesystem::file_time_type::clock;
    using namespace std::chrono;
    using MMM::UI::Utils::formatFileModifiedTime;
    using MMM::UI::Utils::FileModifiedTimeDetail::formatLocalMinute;
    // 极端 C 时间超出常见 64 位平台的日历范围。32 位 time_t 的最大
    // 值可能正是合法的 2038 年时刻，因此不能把它当作失败案例。
    // 平台函数失败时，零初始化的 tm 也不能被格式化成貌似有效的日期。
    if ( std::numeric_limits<std::time_t>::digits >= 63 &&
         !formatLocalMinute(std::numeric_limits<std::time_t>::max()).empty() ) {
        XERROR("Unrepresentable local time did not return an empty result");
        return false;
    }

#ifndef _WIN32
    // TZ 使用自带切换规则的 POSIX 语法，测试不依赖系统时区数据库版本。
    if ( ::setenv("TZ", "UTC0", 1) != 0 ) return false;
    ::tzset();
    // UTC 下两个输入分属相邻日期，可检查负值有没有被错误钳制到纪元。
    if ( formatLocalMinute(30) != "1970/01/01 00:00" ||
         formatLocalMinute(-30) != "1969/12/31 23:59" ) {
        XERROR("Local minute did not preserve the epoch boundary");
        return false;
    }
    // 选取分钟中点，测试的是两个时钟纪元的映射，而非采样时钟的微秒差。
    // 固定日期同时验证年份和分钟，而当前时刻无法稳定写出预期文本。
    const auto target = sys_seconds{ seconds{ 1710054030 } };
    // 用相邻采样把系统节点映射到文件时间，不假定文件时钟纪元。
    const auto fileTime = FileClock::now() + duration_cast<FileClock::duration>(
                                                 target - system_clock::now());
    if ( formatFileModifiedTime(fileTime) != "2024/03/10 07:00" ) {
        XERROR("File clock did not map to the expected system minute");
        return false;
    }
    // 改变显示时区后复用同一个文件时钟值，确保原始绝对时刻无需更新。
    if ( ::setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1) != 0 ) return false;
    ::tzset();
    // 2024 年春季跳时前后的绝对时间连续，本地显示应从 01:59 跳到 03:00。
    // 转换前后用独立的固定分钟核对，避免只确认时区固定偏移而漏测夏令时。
    if ( formatLocalMinute(1710053970) != "2024/03/10 01:59" ||
         formatFileModifiedTime(fileTime) != "2024/03/10 03:00" ) {
        XERROR("File time did not follow local daylight saving time");
        return false;
    }
#else
    // Windows CRT 不能将负 time_t
    // 写成有效的本地日期，应和其他转换失败一样回退。
    // 当前时刻单独验证成功路径，不依赖测试机设置的具体时区。
    if ( !formatLocalMinute(-30).empty() ||
         formatFileModifiedTime(FileClock::now()).empty() )
        return false;
#endif
    return true;
}

}  // namespace

/// @brief 运行不依赖窗口、音频和文件系统的时间显示回归。
/// @return 任一入口格式或边界处理不符合预期时返回非零。
int main()
{
    // 不写文件、不需要计时等待；返回值是本组回归的唯一成功依据。
    // 实际判定线绘制及批注导出另由各模块既有测试检查最终入口。
    // 时区用例最后运行，避免其他时间格式测试继承这里设置的时区。
    return testClock() && testPreferences() && testSnapshotBeatTime() &&
                   testFileModifiedTime()
               ? 0
               : 1;
}
