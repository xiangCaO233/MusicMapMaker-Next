#pragma once

#include "logic/session/context/SessionContext.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Logic::EditorClipboardProtocol
{

/// @brief 系统剪贴板文本头，用于识别 MMM 剪贴板载荷。
inline constexpr std::string_view MAGIC = "MMM_CLIPBOARD_V2";

/// @brief 从系统剪贴板文本解析出的编辑器剪贴板载荷。
struct ParsedClipboard {
    /// @brief 载荷中的音符剪贴板条目。
    std::vector<ClipboardItem> notes;

    /// @brief 载荷中的时间线剪贴板条目。
    std::vector<TimelineClipboardItem> timelines;
};

/// @brief 将音符剪贴板条目序列化为系统剪贴板文本载荷。
/// @param items 待序列化的音符剪贴板条目。
/// @return 适用于 ImGui 和系统剪贴板 API 的 UTF-8 文本载荷。
std::string serializeNotes(const std::vector<ClipboardItem>& items);

/// @brief 将时间线剪贴板条目序列化为系统剪贴板文本载荷。
/// @param items 待序列化的时间线剪贴板条目。
/// @return 适用于 ImGui 和系统剪贴板 API 的 UTF-8 文本载荷。
std::string serializeTimelines(const std::vector<TimelineClipboardItem>& items);

/// @brief 将系统剪贴板文本解析为 MMM 剪贴板载荷。
/// @param text 当前系统剪贴板中的文本。
/// @return 文本符合协议时返回解析后的剪贴板内容。
std::optional<ParsedClipboard> parse(std::string_view text);

}  // namespace MMM::Logic::EditorClipboardProtocol
