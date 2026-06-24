#include "graphic/glfw/GLFWHeader.h"
#include "log/colorful-log.h"
#include <cstddef>
#include <string>
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

/// @brief 相同 GLFW 错误连续打印的最大次数。
constexpr std::size_t MAX_CONSECUTIVE_GLFW_ERROR_LOG_COUNT = 10;

/// @brief GLFW 错误连续重复计数状态。
struct GlfwErrorRepeatState {
    int         m_error{ 0 };        ///< 最近一次 GLFW 错误码。
    std::string m_description{};     ///< 最近一次 GLFW 错误描述。
    std::size_t m_repeatCount{ 0 };  ///< 最近错误连续出现次数。
};

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

/// @brief 消费超过阈值的连续重复 GLFW 错误。
/// @param error GLFW 错误码。
/// @param description GLFW 错误描述。
/// @return 当前错误已超过连续打印阈值时返回 true。
bool consumeConsecutiveRepeatedGlfwError(int error, const char* description)
{
    static GlfwErrorRepeatState state{};

    const std::string_view currentDescription =
        description ? std::string_view(description) : std::string_view{};
    const bool isSameError =
        state.m_repeatCount > 0 && state.m_error == error &&
        std::string_view(state.m_description) == currentDescription;

    if ( !isSameError ) {
        state.m_error       = error;
        state.m_repeatCount = 0;
        if ( description ) {
            state.m_description = description;
        } else {
            state.m_description.clear();
        }
    }

    if ( state.m_repeatCount <= MAX_CONSECUTIVE_GLFW_ERROR_LOG_COUNT ) {
        ++state.m_repeatCount;
    }

    return state.m_repeatCount > MAX_CONSECUTIVE_GLFW_ERROR_LOG_COUNT;
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
    if ( consumeConsecutiveRepeatedGlfwError(error, description) ) {
        return;
    }

    XERROR("GLFW Error {}: {}", error, description);
}
}  // namespace Graphic

}  // namespace MMM
