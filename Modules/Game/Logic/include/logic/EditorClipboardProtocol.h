#pragma once

#include "logic/session/context/SessionContext.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Logic::EditorClipboardProtocol
{

/// @brief System clipboard text header used to identify MMM clipboard payloads.
inline constexpr std::string_view MAGIC = "MMM_CLIPBOARD_V2";

/// @brief Parsed editor clipboard payload imported from system clipboard text.
struct ParsedClipboard {
    /// @brief Note clipboard entries carried by the payload.
    std::vector<ClipboardItem> notes;

    /// @brief Timeline clipboard entries carried by the payload.
    std::vector<TimelineClipboardItem> timelines;
};

/// @brief Serialize note clipboard entries into a text payload for the system
/// clipboard.
/// @param items Note clipboard entries to serialize.
/// @return UTF-8 text payload suitable for ImGui/system clipboard APIs.
std::string serializeNotes(const std::vector<ClipboardItem>& items);

/// @brief Serialize timeline clipboard entries into a text payload for the
/// system clipboard.
/// @param items Timeline clipboard entries to serialize.
/// @return UTF-8 text payload suitable for ImGui/system clipboard APIs.
std::string serializeTimelines(const std::vector<TimelineClipboardItem>& items);

/// @brief Parse system clipboard text as an MMM clipboard payload.
/// @param text Text currently stored in the system clipboard.
/// @return Parsed clipboard content when the payload matches this protocol.
std::optional<ParsedClipboard> parse(std::string_view text);

}  // namespace MMM::Logic::EditorClipboardProtocol
