#include "ui/utils/FileModifiedTime.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <limits>
#include <string>

namespace MMM::UI::Utils
{
namespace FileModifiedTimeDetail
{
/// @brief 使用平台的可重入本地时间接口生成分钟文本。
/// @note localtime_s 与 localtime_r 的成功条件不同；仅格式化已成功写入的 tm。
/// @note 由 C 运行时选择当前进程时区及该日期的夏令时规则。
/// @warning 绘制可见行时使用固定大小缓冲；不执行文件系统查询。
std::string formatLocalMinute(std::time_t time)
{
    // 可重入接口把转换结果写入当前调用的缓冲，避免并行显示共享 tm。
    // 即使缓冲做了零初始化，失败后也不能拿其中的字段构造默认日期。
    std::tm localTime{};
#ifdef _WIN32
    // Windows 的返回值是错误码，零表示成功。
    if ( ::localtime_s(&localTime, &time) != 0 ) return {};
#else
    // POSIX 返回目标缓冲地址，空指针表示无法表达本地日期。
    if ( !::localtime_r(&time, &localTime) ) return {};
#endif
    // 格式化失败时缓冲内容可能不完整，不把数值零或部分日期显示给用户。
    // 保留三张列表原有的分钟列格式，并为年份和末尾空字符留出空间。
    char text[32]{};
    if ( std::strftime(text, sizeof(text), "%Y/%m/%d %H:%M", &localTime) == 0 )
        return {};
    return text;
}
}  // namespace FileModifiedTimeDetail

/// @brief 在文件时钟和系统时钟之间建立当前偏移，再转换为本地分钟。
/// @details 文件时钟纪元由标准库决定。保留双时钟锚点换算，兼容未提供
/// file_clock::to_sys 的标准库。先扩宽计数再求差，避免极值时间在窄整数中溢出。
std::string formatFileModifiedTime(std::filesystem::file_time_type time)
{
    // 使用文件系统类型对应的时钟，避免假设其 tick 单位和平台实现。
    using FileClock = std::filesystem::file_time_type::clock;
    using Seconds   = std::chrono::duration<long double>;
    // 两次相邻的时钟采样只建立纪元偏移，不要求文件时间位于 system_clock
    // 纳秒计数可覆盖的窄区间。对齐结果只供界面显示，不回写文件时间。
    const Seconds fileOffset = Seconds(time.time_since_epoch()) -
                               Seconds(FileClock::now().time_since_epoch());
    const Seconds systemNow(
        std::chrono::system_clock::now().time_since_epoch());
    // 负数也向下取整到实际所属的秒，防止纪元之前最后一秒跨到后一日期。
    const long double wholeSeconds =
        std::floor((systemNow + fileOffset).count());
    // 整数 time_t 的上界用 2 的幂表达，避免扩宽精度不足时把最大值向上舍入，
    // 进而让一个不可表示的计数通过检查。下界来自实际 time_t 的符号和位宽。
    // 支持的平台 time_t 为整数秒，此处直接检查该类型的可表示范围。
    const long double upper =
        std::ldexp(1.0L, std::numeric_limits<std::time_t>::digits);
    const auto lower =
        static_cast<long double>(std::numeric_limits<std::time_t>::lowest());
    if ( !std::isfinite(wholeSeconds) || wholeSeconds < lower ||
         wholeSeconds >= upper )
        return {};

    // 只有能表示的系统纪元秒才交给 C 运行时判断其日历范围。
    return FileModifiedTimeDetail::formatLocalMinute(
        static_cast<std::time_t>(wholeSeconds));
}

}  // namespace MMM::UI::Utils
