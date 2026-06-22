#include "graphic/glfw/GLFWHeader.h"
#include "log/colorful-log.h"
#include <string_view>

namespace MMM
{
namespace Graphic
{
namespace
{
/// @brief macOS 上 GLFW 在 monitor 暂时没有 NSScreen 时报告的非致命平台错误。
constexpr std::string_view COCOA_WORKAREA_WITHOUT_SCREEN_ERROR =
    "Cocoa: Cannot query workarea without screen";

/// @brief 判断 GLFW 错误是否为 macOS 工作区查询不可用的已知噪声。
/// @param error GLFW 错误码。
/// @param description GLFW 错误描述。
/// @return 匹配已知 Cocoa workarea 错误时返回 true。
bool isCocoaWorkareaWithoutScreenError(int error, const char* description)
{
    return error == GLFW_PLATFORM_ERROR && description &&
           std::string_view(description) == COCOA_WORKAREA_WITHOUT_SCREEN_ERROR;
}

/// @brief 消费重复的 macOS workarea 平台错误，保留首条 warning。
/// @param error GLFW 错误码。
/// @param description GLFW 错误描述。
/// @return 当前错误已被处理时返回 true。
bool consumeRepeatedCocoaWorkareaError(int error, const char* description)
{
#if defined(__APPLE__)
    static bool loggedOnce = false;
    if ( !isCocoaWorkareaWithoutScreenError(error, description) ) {
        return false;
    }

    if ( !loggedOnce ) {
        loggedOnce = true;
        XWARN("GLFW platform warning {}: {}", error, description);
    }
    return true;
#else
    (void)error;
    (void)description;
    return false;
#endif
}
}  // namespace

/// @brief GLFW 全局错误回调，记录平台和窗口系统错误。
/// @param error GLFW 错误码。
/// @param description GLFW 错误描述。
void glfw_error_callback(int error, const char* description)
{
    if ( consumeRepeatedCocoaWorkareaError(error, description) ) {
        return;
    }

    XERROR("GLFW Error {}: {}", error, description);
}
}  // namespace Graphic

}  // namespace MMM
