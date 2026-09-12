#include "network/collaboration/RtcDiagnosticLogging.h"

#include "RtcDiagnosticLoggingInternal.h"
#include "log/colorful-log.h"

#include <rtc/rtc.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace MMM::Network::Collaboration
{
namespace
{

/// @brief 判断指定位置是否以忽略 ASCII 大小写的前缀开头。
/// @param text 待检查文本。
/// @param offset 前缀起始位置。
/// @param prefix 待匹配前缀。
/// @return 前缀完整匹配时返回 true。
[[nodiscard]] bool startsWithAsciiCaseInsensitive(std::string_view text,
                                                  std::size_t      offset,
                                                  std::string_view prefix)
{
    // 先用减法形式检查剩余长度，避免 offset 加法溢出。
    if ( offset > text.size() || prefix.size() > text.size() - offset ) {
        return false;
    }
    // URI scheme 只包含 ASCII；逐字节比较无需区域设置或 Unicode 折叠。
    return std::equal(
        prefix.begin(),
        prefix.end(),
        text.begin() + offset,
        [](char lhs, char rhs) {
            // cctype 要求传入 unsigned char 可表示值，避免负 char
            // 触发未定义行为。
            return std::tolower(static_cast<unsigned char>(lhs)) ==
                   std::tolower(static_cast<unsigned char>(rhs));
        });
}

/// @brief 转发脱敏后的 libdatachannel 日志到项目日志系统。
/// @param level libdatachannel 日志级别。
/// @param message libdatachannel 日志文本。
/// @warning 由 libdatachannel 内部线程回调；允许诊断期开销，但不得保留 message
/// 指针。
void logRtcMessage(rtcLogLevel level, const char* message)
{
    // 第三方回调允许用空指针表达无消息，直接忽略以保护字符串构造。
    if ( !message ) return;
    // 在进入任何项目日志宏前完成脱敏，确保原始 TURN 凭据不会被格式化。
    const auto redacted = Detail::redactRtcDiagnosticMessage(message);
    // 致命与错误均映射到项目错误级别，保持用户日志中的故障可见性。
    if ( level == RTC_LOG_FATAL || level == RTC_LOG_ERROR ) {
        XERROR("libdatachannel: {}", redacted);
    } else if ( level == RTC_LOG_WARNING ) {
        // 警告保持原级别，便于区分可恢复的 ICE 状态与连接失败。
        XWARN("libdatachannel: {}", redacted);
    } else {
        // 发布包可能在编译期裁剪 XDEBUG；显式开启诊断后统一写入全量日志。
        XINFO("libdatachannel: {}", redacted);
    }
}

}  // namespace

namespace Detail
{

/// @brief 复制日志并隐藏 TURN/TURNS URI authority 中的用户信息。
/// @param message 原始第三方诊断文本。
/// @return 保留服务地址和上下文、将凭据替换为固定标记的新字符串。
std::string redactRtcDiagnosticMessage(std::string_view message)
{
    // 返回独立字符串，既允许原位替换，也不依赖回调参数的短生命周期。
    std::string result(message);
    std::size_t searchOffset = 0;
    // 一条日志可能列出多个 ICE 服务，必须逐个定位并脱敏。
    while ( searchOffset < result.size() ) {
        std::size_t schemeOffset = std::string::npos;
        std::size_t schemeLength = 0;
        // 手工扫描可同时支持嵌入普通文本的 URI 和大小写变体。
        for ( std::size_t offset = searchOffset; offset < result.size();
              ++offset ) {
            // 优先匹配较长的 turns:，避免被较短规则错误截断。
            if ( startsWithAsciiCaseInsensitive(result, offset, "turns:") ) {
                schemeOffset = offset;
                schemeLength = 6;
                break;
            }
            // turn: 使用五字节 scheme，后续 authority 解析规则相同。
            if ( startsWithAsciiCaseInsensitive(result, offset, "turn:") ) {
                schemeOffset = offset;
                schemeLength = 5;
                break;
            }
        }
        // 没有更多 TURN scheme 时，剩余文本不含目标凭据。
        if ( schemeOffset == std::string::npos ) break;

        // URI 同时兼容 turn:user:pass@host 与 turn://user:pass@host 形式。
        std::size_t credentialOffset = schemeOffset + schemeLength;
        if ( result.compare(credentialOffset, 2, "//") == 0 ) {
            credentialOffset += 2;
        }
        // authority 在常见日志分隔符处结束，不能跨到后续字段寻找 @。
        const auto authorityEnd =
            result.find_first_of(" \t\r\n\"'<>()[\\]{};,", credentialOffset);
        const auto atOffset = result.find('@', credentialOffset);
        // 没有 @ 或 @ 已落到 authority 之外时，此 URI 不携带用户信息。
        if ( atOffset == std::string::npos ||
             (authorityEnd != std::string::npos && authorityEnd < atOffset) ) {
            // 从 authority 起点继续扫描，仍允许后续独立 URI 被发现。
            searchOffset = credentialOffset;
            continue;
        }
        // 空用户信息无需替换，并前移游标避免重复命中同一 scheme。
        if ( atOffset == credentialOffset ) {
            searchOffset = atOffset + 1;
            continue;
        }

        // 固定标记既隐藏用户名和密码，也保留 @host 供网络诊断。
        constexpr std::string_view REDACTED = "[redacted]";
        result.replace(credentialOffset, atOffset - credentialOffset, REDACTED);
        // 替换会改变字符串长度，必须基于新文本位置继续扫描。
        searchOffset = credentialOffset + REDACTED.size() + 1;
    }
    // 无匹配时返回内容相同的副本，调用方无需区分脱敏是否发生。
    return result;
}

}  // namespace Detail

/// @brief 配置进程级 libdatachannel 诊断日志回调与最低级别。
/// @param enabled 为 true 时启用 Debug 及以上转发，否则彻底停用回调。
/// @warning 该入口修改第三方库全局状态，只能由串行的应用设置路径调用。
void setRtcDiagnosticLoggingEnabled(bool enabled)
{
    if ( enabled ) {
        // libdatachannel 保存进程级函数指针，回调自身不捕获应用对象。
        rtcInitLogger(RTC_LOG_DEBUG, &logRtcMessage);
        // 使用 Info 记录显式启用，便于确认诊断会增加日志量。
        XINFO("WebRTC/ICE diagnostic logging enabled");
        return;
    }
    // NONE 与空回调同时关闭第三方产生日志和回调转发。
    rtcInitLogger(RTC_LOG_NONE, nullptr);
    // 关闭事件同样保留审计记录，但不会包含任何 RTC 原始内容。
    XINFO("WebRTC/ICE diagnostic logging disabled");
}

}  // namespace MMM::Network::Collaboration
