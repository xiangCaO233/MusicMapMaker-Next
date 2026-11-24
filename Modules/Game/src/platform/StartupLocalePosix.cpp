#include "StartupLocale.h"

#include <array>
#include <clocale>

namespace MMM::Platform
{

/// @brief 按运行库支持情况选择 UTF-8 字符 locale。
bool initializeStartupCharacterLocale() noexcept
{
    // 优先选用无地域偏好的 C.UTF-8。缺少该别名的系统可提供 en_US.UTF-8。
    // 精简镜像可能省略全部 UTF-8 数据，因此以运行库的返回值判断是否生效。
    // 省略空字符串候选，因为环境可指定与 UTF-8 无关的字符编码。
    constexpr std::array names{ "C.UTF-8", "en_US.UTF-8" };
    for ( const char* name : names ) {
        // 只设置字符类别，用户环境中的小数点和日期规则不会被启动流程覆盖。
        if ( std::setlocale(LC_CTYPE, name) ) return true;
    }

    // 每次设置失败都保留原字符类别，调用方可在日志初始化后记录回退。
    return false;
}

}  // namespace MMM::Platform
