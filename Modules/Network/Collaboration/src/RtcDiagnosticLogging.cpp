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
    if ( offset > text.size() || prefix.size() > text.size() - offset ) {
        return false;
    }
    return std::equal(
        prefix.begin(),
        prefix.end(),
        text.begin() + offset,
        [](char lhs, char rhs) {
            return std::tolower(static_cast<unsigned char>(lhs)) ==
                   std::tolower(static_cast<unsigned char>(rhs));
        });
}

/// @brief 转发脱敏后的 libdatachannel 日志到项目日志系统。
/// @param level libdatachannel 日志级别。
/// @param message libdatachannel 日志文本。
void logRtcMessage(rtcLogLevel level, const char* message)
{
    if ( !message ) return;
    const auto redacted = Detail::redactRtcDiagnosticMessage(message);
    if ( level == RTC_LOG_FATAL || level == RTC_LOG_ERROR ) {
        XERROR("libdatachannel: {}", redacted);
    } else if ( level == RTC_LOG_WARNING ) {
        XWARN("libdatachannel: {}", redacted);
    } else {
        // 发布包可能在编译期裁剪 XDEBUG；显式开启诊断后统一写入全量日志。
        XINFO("libdatachannel: {}", redacted);
    }
}

}  // namespace

namespace Detail
{

std::string redactRtcDiagnosticMessage(std::string_view message)
{
    std::string result(message);
    std::size_t searchOffset = 0;
    while ( searchOffset < result.size() ) {
        std::size_t schemeOffset = std::string::npos;
        std::size_t schemeLength = 0;
        for ( std::size_t offset = searchOffset; offset < result.size();
              ++offset ) {
            if ( startsWithAsciiCaseInsensitive(result, offset, "turns:") ) {
                schemeOffset = offset;
                schemeLength = 6;
                break;
            }
            if ( startsWithAsciiCaseInsensitive(result, offset, "turn:") ) {
                schemeOffset = offset;
                schemeLength = 5;
                break;
            }
        }
        if ( schemeOffset == std::string::npos ) break;

        std::size_t credentialOffset = schemeOffset + schemeLength;
        if ( result.compare(credentialOffset, 2, "//") == 0 ) {
            credentialOffset += 2;
        }
        const auto authorityEnd =
            result.find_first_of(" \t\r\n\"'<>()[\\]{};,", credentialOffset);
        const auto atOffset = result.find('@', credentialOffset);
        if ( atOffset == std::string::npos ||
             (authorityEnd != std::string::npos && authorityEnd < atOffset) ) {
            searchOffset = credentialOffset;
            continue;
        }
        if ( atOffset == credentialOffset ) {
            searchOffset = atOffset + 1;
            continue;
        }

        constexpr std::string_view REDACTED = "[redacted]";
        result.replace(credentialOffset, atOffset - credentialOffset, REDACTED);
        searchOffset = credentialOffset + REDACTED.size() + 1;
    }
    return result;
}

}  // namespace Detail

void setRtcDiagnosticLoggingEnabled(bool enabled)
{
    if ( enabled ) {
        rtcInitLogger(RTC_LOG_DEBUG, &logRtcMessage);
        XINFO("WebRTC/ICE diagnostic logging enabled");
        return;
    }
    rtcInitLogger(RTC_LOG_NONE, nullptr);
    XINFO("WebRTC/ICE diagnostic logging disabled");
}

}  // namespace MMM::Network::Collaboration
