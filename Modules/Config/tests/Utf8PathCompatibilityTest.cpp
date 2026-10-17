#include "config/Utf8Path.h"

#ifdef _WIN32
// 故意在路径工具之后包含这两个 SDK 头，重现窗口适配器的真实包含顺序。
// commctrl 的指针别名和 Shell 头的工具栏声明都依赖 NEAR 的正常展开。
#    include <commctrl.h>
#    include <shlobj.h>
#endif

#include <cmath>
#include <type_traits>

namespace
{
/// @brief 验证公共路径头不会把小写 near 宏泄漏给后续 C++ 函数。
/// @param lhs 比较左值。
/// @param rhs 比较右值。
/// @return 数值在测试容差内相等时为真。
/// @details 名称故意使用 near，不能改名绕过 Windows 宏兼容契约。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-6;
}

#ifdef _WIN32
// 除了 SDK 头可以被解析，还明确检查大写兼容宏没有改变指针类型。
// 小写 near 已被清理，但大写 NEAR 必须仍可用于 Win32 声明。
using NearPointer = int NEAR*;
static_assert(std::is_same_v<NearPointer, int*>);
static_assert(std::is_same_v<PTBBUTTON, TBBUTTON*>);
#endif
}  // namespace

/// @brief 验证路径公共头的 SDK 兼容与 UTF-8 转换行为。
/// @return 编译契约满足且跨平台路径往返无损时返回零。
/// @details Windows 编译阶段覆盖宏展开，运行阶段覆盖 Unicode 和空路径。
/// 测试仅构造 path 值，不访问磁盘，不依赖系统当前 ANSI 区域设置。
int main()
{
    // 混合中文、空格和补充平面字符，防止转换退回本地窄字符编码。
    const std::string text = "路径 空格/音频-🎵.wav";
    const auto        path = MMM::Config::utf8ToPath(text);
    if ( MMM::Config::pathToUtf8Generic(path) != text ) return 1;
    // 反向构造同一个路径，确认泛用分隔符转换没有改变路径含义。
    if ( MMM::Config::utf8ToPath(MMM::Config::pathToUtf8(path)) != path )
        return 2;
    if ( !MMM::Config::utf8ToPath("").empty() ) return 3;
    // 该调用同时保证 near 函数声明和表达式均没有被 SDK 宏破坏。
    return near(0.25 + 0.25, 0.5) ? 0 : 4;
}
