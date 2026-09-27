#include "graphic/glfw/GLFWHeader.h"
#include "log/colorful-log.h"
#include <cstddef>
#include <string>
#include <string_view>

/// @file
/// @brief 对 GLFW 全局错误进行分级记录，并抑制已知或连续重复的平台噪声。
///
/// 回调可能由任意 GLFW API 同步触发，因此这里只维护极小的进程内重复状态，
/// 不执行窗口操作，也不把错误重新投递到事件系统。

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
    // 同时匹配平台错误码和完整描述，避免吞掉其他 Cocoa 故障。
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
    // 该错误在显示器切换的短暂窗口内可能高频重复，进程内只保留首条警告。
    static bool loggedOnce = false;
    if ( !isCocoaWorkareaWithoutScreenError(error, description) ) {
        return false;
    }

    if ( !loggedOnce ) {
        // 首次出现仍写入日志，便于区分正常运行与平台兼容分支生效。
        loggedOnce = true;
        XWARN("GLFW platform warning {}: {}", error, description);
    }
    return true;
#else
    // 非 macOS 构建保留统一函数形状，但不抑制任何平台错误。
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
    // GLFW 错误回调按调用顺序同步更新这一进程内连续序列状态。
    static GlfwErrorRepeatState state{};

    // 空描述映射为空视图，比较逻辑不解引用来自 GLFW 的空指针。
    const std::string_view currentDescription =
        description ? std::string_view(description) : std::string_view{};
    const bool isSameError =
        state.m_repeatCount > 0 && state.m_error == error &&
        std::string_view(state.m_description) == currentDescription;

    if ( !isSameError ) {
        // 错误码或描述变化会开启新序列，使不同故障仍能立即记录。
        state.m_error       = error;
        state.m_repeatCount = 0;
        if ( description ) {
            // 拷贝描述以跨越当前 GLFW 回调参数的生命周期进行下次比较。
            state.m_description = description;
        } else {
            state.m_description.clear();
        }
    }

    // 计数在阈值后一位饱和，避免长期重复错误导致 size_t 回绕。
    if ( state.m_repeatCount <= MAX_CONSECUTIVE_GLFW_ERROR_LOG_COUNT ) {
        ++state.m_repeatCount;
    }

    // 前若干条保留完整错误，超过阈值后仅抑制同一连续序列。
    return state.m_repeatCount > MAX_CONSECUTIVE_GLFW_ERROR_LOG_COUNT;
}
}  // namespace

/// @brief GLFW 全局错误回调，记录平台和窗口系统错误。
/// @param error GLFW 错误码。
/// @param description GLFW 错误描述。
void glfw_error_callback(int error, const char* description)
{
    // 先识别有明确兼容含义的 Cocoa 噪声，其首条按 warning 记录。
    if ( consumeRepeatedCocoaWorkareaError(error, description) ) {
        return;
    }
    // 通用重复限流在所有平台生效，但错误变化会立即恢复日志。
    if ( consumeConsecutiveRepeatedGlfwError(error, description) ) {
        return;
    }

    // 未被抑制的错误统一进入项目日志，保留 GLFW 原始码和描述。
    XERROR("GLFW Error {}: {}", error, description);
}
}  // namespace Graphic

}  // namespace MMM
