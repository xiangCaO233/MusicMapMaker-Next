#include "config/Utf8Path.h"

// 路径公共头必须可以独立使用，不再通过 Windows SDK 定义平台宏。
// 直接检查宏状态，防止头文件恢复内联转码时重新扩散 SDK 依赖。
#if defined(_WINDOWS_) || defined(near) || defined(NEAR)
#    error "Utf8Path.h must not include Windows SDK headers"
#endif

#ifdef _WIN32
// SDK 使用者显式包含自身依赖，路径工具不负责定义或清理外部 SDK 宏。
// 保留 commctrl 与 Shell 的组合，确认路径头在前时不破坏 SDK 声明。
// clang-format off
#    include <windows.h>
#    include <commctrl.h>
#    include <shlobj.h>
// clang-format on
#endif

#include <cmath>
#include <type_traits>

namespace
{
/// @brief 比较路径测试中使用的简单浮点值。
/// @param lhs 比较左值。
/// @param rhs 比较右值。
/// @return 数值在测试容差内相等时为真。
bool valuesNear(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-6;
}

#ifdef _WIN32
// 除了 SDK 头可以被解析，还明确检查大写兼容宏没有改变指针类型。
// SDK 自行维护 near 与 NEAR，路径工具不再参与宏别名重写。
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
    // 显式 SDK 包含后的普通 C++ 计算仍应保持可用。
    return valuesNear(0.25 + 0.25, 0.5) ? 0 : 4;
}
