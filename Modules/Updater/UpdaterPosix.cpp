#include "UpdaterPlatform.h"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <limits>
#include <signal.h>
#include <sys/types.h>
#include <thread>

namespace MMM::Update
{
/// @brief 有限等待 POSIX 父进程退出，只有 ESRCH 证明该 PID 已不存在。
/// @param pid 已检查范围的正数进程标识，避开进程组语义。
/// @return 仅进程探测报告 ESRCH 时返回 true。
/// @note 探测不会发信号。权限不足仍可能有旧进程，安全起见不得继续安装。
/// @details POSIX 不依赖父进程作为更新器的子进程，因此这里不调用 waitpid。
/// @warning 低频更新阶段有三十秒等待上限，主应用线程不参与这段等待。
bool waitForParent(long value)
{
    if ( value > std::numeric_limits<pid_t>::max() ) return false;
    const auto pid = static_cast<pid_t>(value);
    // 每半秒检查一次，最后一次检查包含完整三十秒等待窗口。
    // 探测返回成功或 EPERM 表示 PID 仍存在。其他错误直接结束探测，
    // 其中只有 ESRCH 可作为继续替换安装文件的依据。
    for ( int i = 0; i <= 60; ++i ) {
        if ( kill(pid, 0) == -1 && errno != EPERM ) return errno == ESRCH;
        if ( i < 60 )
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

/// @brief 解析父进程 PID 参数。
/// @param text 命令行中的 PID 文本。
/// @return 解析成功时返回正数 PID，否则返回 0。
///
/// from_chars 不受区域设置影响且不分配；要求完整消费输入，拒绝后缀字符、
/// 溢出、零值和负值。零作为统一无效哨兵不会与合法进程 ID 冲突。
long parseParentPid(std::string_view text)
{
    long       value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    // 三项检查分别覆盖数值错误、未完整消费和非正进程 ID。
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
         value <= 0 ) {
        return 0;
    }
    return value;
}
}  // namespace MMM::Update

/// @brief POSIX 保留命令行路径的原始字节。
int main(int argc, char* argv[])
{
    return MMM::Update::runUpdater(argc, argv);
}
