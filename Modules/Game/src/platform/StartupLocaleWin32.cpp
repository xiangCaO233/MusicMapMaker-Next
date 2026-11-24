#include "StartupLocale.h"

#include <clocale>

namespace MMM::Platform
{

/// @brief 通过 UCRT 的 UTF-8 代码页设置字符类别。
bool initializeStartupCharacterLocale() noexcept
{
    // 较旧的运行库可能不支持 .UTF8，调用方需要依据返回值记录回退。
    // 可执行文件路径与命令行仍由 Win32 宽字符接口提供。
    // 不修改 LC_NUMERIC，谱面和剪贴板中的十进制分隔符保持原有含义。
    return std::setlocale(LC_CTYPE, ".UTF8") != nullptr;
}

}  // namespace MMM::Platform
