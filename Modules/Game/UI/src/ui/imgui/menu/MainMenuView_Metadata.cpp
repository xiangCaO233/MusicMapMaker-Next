#define IMGUI_DEFINE_MATH_OPERATORS
#include "canvas/TimeFormatUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Polyline.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/menu/MainMenuView.h"
#include <ImGuiFileDialog.h>
#include <fmt/core.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace MMM::UI
{

namespace
{
/// @brief 元数据字段 JSON 编辑器可写回的结果。
struct MetadataJsonEditResult {
    /// @brief 调用者传入的作用域标识，用于区分谱面/音符/格式页。
    std::string scopeId;
    /// @brief 被编辑的字段名。
    std::string key;
    /// @brief 校验通过后写回属性表的 JSON 文本。
    std::string value;
};

/// @brief 元数据 JSON 编辑器的跨帧 UI 状态。
struct MetadataJsonEditorState {
    /// @brief 当前弹窗是否处于打开状态。
    bool open = false;
    /// @brief 下一次渲染是否需要打开 ImGui 弹窗。
    bool requestOpen = false;
    /// @brief 当前编辑目标的作用域标识。
    std::string scopeId;
    /// @brief 当前编辑目标字段。
    std::string key;
    /// @brief 展示给用户看的完整路径。
    std::string displayPath;
    /// @brief 原生 JSON 编辑缓冲区。
    std::array<char, 32768> jsonBuffer{};
    /// @brief 子字段键名输入缓冲区。
    std::array<char, 128> childKeyBuffer{};
    /// @brief 子字段值输入缓冲区。
    std::array<char, 4096> childValueBuffer{};
    /// @brief 最近一次校验错误。
    std::string errorText;
    /// @brief 最近一次结构警告。
    std::string warningText;
    /// @brief 等待调用方消费的写回结果。
    std::optional<MetadataJsonEditResult> result;
};

/// @brief 元数据属性表类型。
using MetadataPropertyMap =
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>;

/// @brief 元数据字段说明列表类型。
using MetadataFieldList = std::vector<std::pair<std::string, std::string>>;

/// @brief OSU 纯文本元数据编辑器状态。
struct OsuMetadataTextEditorState {
    /// @brief 当前弹窗是否处于打开状态。
    bool open = false;
    /// @brief 下一次渲染是否需要打开 ImGui 弹窗。
    bool requestOpen = false;
    /// @brief 纯文本编辑缓冲区。
    std::array<char, 65536> textBuffer{};
    /// @brief 最近一次解析错误。
    std::string errorText;
    /// @brief 等待调用方消费的写回结果。
    std::optional<MetadataPropertyMap> result;
};

/// @brief 获取元数据 JSON 编辑器的持久状态。
/// @warning UI 每帧绘制路径：仅保存少量弹窗状态，不进行文件系统操作。
MetadataJsonEditorState& metadataJsonEditorState()
{
    static MetadataJsonEditorState state;
    return state;
}

/// @brief 获取 OSU 纯文本元数据编辑器的持久状态。
/// @warning UI 每帧绘制路径：仅保存少量弹窗状态，不进行文件系统操作。
OsuMetadataTextEditorState& osuMetadataTextEditorState()
{
    static OsuMetadataTextEditorState state;
    return state;
}

/// @brief 将字符串安全复制进固定 ImGui 输入缓冲区。
/// @warning UI 每帧/交互路径：只做固定上限内存复制。
template<size_t N>
void copyToInputBuffer(std::array<char, N>& buffer, std::string_view text)
{
    buffer.fill('\0');
    const size_t copyLen = std::min(text.size(), N - 1);
    std::copy_n(text.data(), copyLen, buffer.data());
}

/// @brief 解析 JSON 文本，不使用 C++ 异常。
nlohmann::json parseJsonNoThrow(const char* text)
{
    return nlohmann::json::parse(text, nullptr, false);
}

/// @brief 将 JSON 规整为缩进文本。
std::string dumpPrettyJson(const nlohmann::json& value)
{
    return value.dump(4);
}

/// @brief 将子字段输入解析为 JSON 值，非法 JSON 时按字符串保存。
nlohmann::json parseChildValueOrString(const char* text)
{
    nlohmann::json value = parseJsonNoThrow(text);
    if ( value.is_discarded() ) {
        return std::string(text);
    }
    return value;
}

/// @brief 将文本解析为 int32 元数据值，不使用 C++ 异常。
std::optional<int32_t> parseInt32Metadata(std::string_view text)
{
    if ( text.empty() ) return std::nullopt;

    int32_t     value    = 0;
    const char* begin    = text.data();
    const char* end      = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if ( ec != std::errc{} || ptr != end ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 将浮点数夹到 int32 范围后转换。
int32_t clampDoubleToInt32(double value)
{
    const double minValue =
        static_cast<double>(std::numeric_limits<int32_t>::min());
    const double maxValue =
        static_cast<double>(std::numeric_limits<int32_t>::max());
    return static_cast<int32_t>(
        std::round(std::clamp(value, minValue, maxValue)));
}

/// @brief 读取状态栏同源的当前判定线时间，并转换为预览元数据毫秒文本。
/// @return 非负毫秒整数字符串。
std::string readCurrentJudgelinePreviewMsText()
{
    auto&       engine         = Logic::EditorEngine::instance();
    std::string activeCameraId = engine.getActiveCameraId();
    auto        syncBuffer     = engine.getSyncBuffer(
        activeCameraId.empty() ? "Basic2DCanvas" : activeCameraId);

    double timeSeconds = 0.0;
    bool   hasTime     = false;
    if ( syncBuffer ) {
        auto* snapshot = syncBuffer->getReadingSnapshot();
        if ( snapshot ) {
            timeSeconds = snapshot->currentTime;
            hasTime     = true;
        }
    }

    if ( !hasTime ) {
        auto session = engine.getActiveSession();
        if ( session ) {
            timeSeconds = session->getContext().currentTime;
        }
    }

    int32_t timeMs = clampDoubleToInt32(std::max(0.0, timeSeconds) * 1000.0);
    return std::to_string(timeMs);
}

/// @brief 去掉纯文本编辑行首尾空白。
std::string trimMetadataLine(std::string value)
{
    auto isNotSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), isNotSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(),
                value.end());
    return value;
}

/// @brief 判断字符串是否以指定前缀开头。
bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

/// @brief 获取 OSU 元数据字段的导出默认值。
std::string getOsuMetadataDefaultValue(const BeatMap&     beatmap,
                                       const std::string& key)
{
    const auto& base = beatmap.m_baseMapMetadata;

    if ( key == "file_format_version" ) return "v14";
    if ( key == "General::AudioFilename" ) {
        return Config::pathToUtf8(base.main_audio_path);
    }
    if ( key == "General::AudioLeadIn" ) return "0";
    if ( key == "General::AudioHash" ) return "";
    if ( key == "General::PreviewTime" ) return "-1";
    if ( key == "General::Countdown" ) return "1";
    if ( key == "General::SampleSet" ) return "Normal";
    if ( key == "General::StackLeniency" ) return "0.7";
    if ( key == "General::Mode" ) return "3";
    if ( key == "General::LetterboxInBreaks" ) return "0";
    if ( key == "General::StoryFireInFront" ) return "1";
    if ( key == "General::UseSkinSprites" ) return "0";
    if ( key == "General::AlwaysShowPlayfield" ) return "0";
    if ( key == "General::OverlayPosition" ) return "NoChange";
    if ( key == "General::SkinPreference" ) return "";
    if ( key == "General::EpilepsyWarning" ) return "0";
    if ( key == "General::CountdownOffset" ) return "0";
    if ( key == "General::SpecialStyle" ) return "0";
    if ( key == "General::WidescreenStoryboard" ) return "0";
    if ( key == "General::SamplesMatchPlaybackRate" ) return "0";

    if ( key == "Editor::Bookmarks" ) return "";
    if ( key == "Editor::DistanceSpacing" ) return "0.0";
    if ( key == "Editor::BeatDivisor" ) return "4";
    if ( key == "Editor::GridSize" ) return "16";
    if ( key == "Editor::TimelineZoom" ) return "1";

    if ( key == "Metadata::Title" ) return base.title;
    if ( key == "Metadata::TitleUnicode" ) return base.title_unicode;
    if ( key == "Metadata::Artist" ) return base.artist;
    if ( key == "Metadata::ArtistUnicode" ) return base.artist_unicode;
    if ( key == "Metadata::Creator" ) {
        return base.author.empty() ? "mmm" : base.author;
    }
    if ( key == "Metadata::Version" ) {
        return base.version.empty() ? "[mmm]" : base.version;
    }
    if ( key == "Metadata::Source" ) return "";
    if ( key == "Metadata::Tags" ) return "";
    if ( key == "Metadata::BeatmapID" ) return "0";
    if ( key == "Metadata::BeatmapSetID" ) return "-1";

    if ( key == "Difficulty::HPDrainRate" ) return "5";
    if ( key == "Difficulty::CircleSize" ) {
        return std::to_string(base.track_count > 0 ? base.track_count : 4);
    }
    if ( key == "Difficulty::OverallDifficulty" ) return "8";
    if ( key == "Difficulty::ApproachRate" ) return "5";
    if ( key == "Difficulty::SliderMultiplier" ) return "1.4";
    if ( key == "Difficulty::SliderTickRate" ) return "1";

    if ( key == "Events::background" ) {
        if ( base.cover_type == CoverType::VIDEO ) {
            return fmt::format("Video,{},\"{}\"",
                               base.video_starttime,
                               Config::pathToUtf8(base.main_cover_path));
        }
        return fmt::format("0,0,\"{}\",{},{}",
                           Config::pathToUtf8(base.main_cover_path),
                           base.bgxoffset,
                           base.bgyoffset);
    }
    if ( key == "Events::breaks" ) return "";

    return "";
}

/// @brief 补齐 OSU 元数据默认字段。
void ensureCompleteOsuMetadata(MetadataPropertyMap&     props,
                               const BeatMap&           beatmap,
                               const MetadataFieldList& fields)
{
    props.try_emplace(
        "file_format_version",
        getOsuMetadataDefaultValue(beatmap, "file_format_version"));
    for ( const auto& [key, desc] : fields ) {
        props.try_emplace(key, getOsuMetadataDefaultValue(beatmap, key));
    }
}

/// @brief 将 OSU 元数据同步回基础元数据，保证导出使用编辑后的关键字段。
void syncOsuMetadataToBase(const MetadataPropertyMap& props, BeatMap& beatmap)
{
    auto get = [&props](const std::string& key) -> const std::string* {
        auto it = props.find(key);
        if ( it == props.end() ) return nullptr;
        return &it->second;
    };

    auto& base = beatmap.m_baseMapMetadata;
    if ( const auto* value = get("General::AudioFilename") ) {
        base.main_audio_path = Config::utf8ToPath(*value);
    }
    if ( const auto* value = get("Metadata::Title") ) base.title = *value;
    if ( const auto* value = get("Metadata::TitleUnicode") ) {
        base.title_unicode = *value;
    }
    if ( const auto* value = get("Metadata::Artist") ) base.artist = *value;
    if ( const auto* value = get("Metadata::ArtistUnicode") ) {
        base.artist_unicode = *value;
    }
    if ( const auto* value = get("Metadata::Creator") ) base.author = *value;
    if ( const auto* value = get("Metadata::Version") ) base.version = *value;

    if ( const auto* value = get("Difficulty::CircleSize") ) {
        nlohmann::json parsed = parseJsonNoThrow(value->c_str());
        if ( parsed.is_number() ) {
            base.track_count = clampDoubleToInt32(parsed.get<double>());
        } else if ( auto parsedInt = parseInt32Metadata(*value) ) {
            base.track_count = *parsedInt;
        }
    }

    if ( const auto* value = get("Events::background") ) {
        std::vector<std::string> parts;
        std::istringstream       stream(*value);
        std::string              token;
        while ( std::getline(stream, token, ',') ) {
            parts.push_back(trimMetadataLine(token));
        }

        if ( parts.size() >= 3 ) {
            base.cover_type =
                parts[0] == "Video" ? CoverType::VIDEO : CoverType::IMAGE;
            if ( auto start = parseInt32Metadata(parts[1]) ) {
                base.video_starttime = *start;
            }

            std::string path = parts[2];
            if ( path.size() >= 2 && path.front() == '"' &&
                 path.back() == '"' ) {
                path = path.substr(1, path.size() - 2);
            }
            base.main_cover_path = Config::utf8ToPath(path);

            if ( parts.size() >= 5 ) {
                if ( auto x = parseInt32Metadata(parts[3]) ) {
                    base.bgxoffset = *x;
                }
                if ( auto y = parseInt32Metadata(parts[4]) ) {
                    base.bgyoffset = *y;
                }
            }
        }
    }
}

/// @brief 将 OSU 元数据表序列化为 .osu 风格纯文本。
std::string buildOsuMetadataText(const MetadataPropertyMap& props,
                                 const BeatMap&             beatmap)
{
    auto get = [&props, &beatmap](const std::string& key) {
        if ( auto it = props.find(key); it != props.end() ) {
            return it->second;
        }
        return getOsuMetadataDefaultValue(beatmap, key);
    };

    std::ostringstream out;
    std::string        formatVersion = get("file_format_version");
    if ( !startsWith(formatVersion, "v") ) formatVersion = "v" + formatVersion;
    out << "osu file format " << formatVersion << "\n\n";

    auto writeExtraKeys = [&props, &out](std::string_view section,
                                         const auto&      knownKeys,
                                         bool             spaceAfterColon) {
        const std::string prefix = fmt::format("{}::", section);
        for ( const auto& [fullKey, value] : props ) {
            if ( !startsWith(fullKey, prefix) ) continue;

            std::string_view localKey(fullKey.data() + prefix.size(),
                                      fullKey.size() - prefix.size());
            const bool known = std::find_if(knownKeys.begin(),
                                            knownKeys.end(),
                                            [localKey](const char* knownKey) {
                                                return localKey == knownKey;
                                            }) != knownKeys.end();
            if ( known ) continue;

            out << localKey << (spaceAfterColon ? ": " : ":") << value << "\n";
        }
    };

    out << "[General]\n";
    const std::array generalKeys{ "AudioFilename",
                                  "AudioLeadIn",
                                  "AudioHash",
                                  "PreviewTime",
                                  "Countdown",
                                  "SampleSet",
                                  "StackLeniency",
                                  "Mode",
                                  "LetterboxInBreaks",
                                  "StoryFireInFront",
                                  "UseSkinSprites",
                                  "AlwaysShowPlayfield",
                                  "OverlayPosition",
                                  "SkinPreference",
                                  "EpilepsyWarning",
                                  "CountdownOffset",
                                  "SpecialStyle",
                                  "WidescreenStoryboard",
                                  "SamplesMatchPlaybackRate" };
    for ( const auto* key : generalKeys ) {
        out << key << ": " << get(fmt::format("General::{}", key)) << "\n";
    }
    writeExtraKeys("General", generalKeys, true);

    out << "\n[Editor]\n";
    const std::array editorKeys{ "Bookmarks",
                                 "DistanceSpacing",
                                 "BeatDivisor",
                                 "GridSize",
                                 "TimelineZoom" };
    for ( const auto* key : editorKeys ) {
        out << key << ": " << get(fmt::format("Editor::{}", key)) << "\n";
    }
    writeExtraKeys("Editor", editorKeys, true);

    out << "\n[Metadata]\n";
    const std::array metadataKeys{ "Title",         "TitleUnicode", "Artist",
                                   "ArtistUnicode", "Creator",      "Version",
                                   "Source",        "Tags",         "BeatmapID",
                                   "BeatmapSetID" };
    for ( const auto* key : metadataKeys ) {
        out << key << ":" << get(fmt::format("Metadata::{}", key)) << "\n";
    }
    writeExtraKeys("Metadata", metadataKeys, false);

    out << "\n[Difficulty]\n";
    const std::array difficultyKeys{ "HPDrainRate",       "CircleSize",
                                     "OverallDifficulty", "ApproachRate",
                                     "SliderMultiplier",  "SliderTickRate" };
    for ( const auto* key : difficultyKeys ) {
        out << key << ":" << get(fmt::format("Difficulty::{}", key)) << "\n";
    }
    writeExtraKeys("Difficulty", difficultyKeys, false);

    out << "\n[Events]\n";
    out << "//Background and Video events\n";
    out << get("Events::background") << "\n";
    out << "//Break Periods\n";
    const std::string breaks = get("Events::breaks");
    out << breaks;
    if ( !breaks.empty() && breaks.back() != '\n' ) {
        out << "\n";
    }
    return out.str();
}

/// @brief 从 .osu 风格纯文本解析 OSU 元数据表。
bool parseOsuMetadataText(std::string_view text, MetadataPropertyMap& props,
                          std::string& errorText)
{
    MetadataPropertyMap      nextProps;
    std::istringstream       stream{ std::string(text) };
    std::string              line;
    std::string              section;
    std::vector<std::string> breakLines;
    size_t                   lineNo = 0;

    while ( std::getline(stream, line) ) {
        ++lineNo;
        if ( !line.empty() && line.back() == '\r' ) line.pop_back();
        std::string trimmed = trimMetadataLine(line);
        if ( trimmed.empty() ) continue;

        if ( startsWith(trimmed, "osu file format") ) {
            std::string version = trimMetadataLine(
                trimmed.substr(std::string("osu file format").size()));
            if ( version.empty() ) {
                errorText =
                    fmt::format("第 {} 行：缺少 osu 文件版本。", lineNo);
                return false;
            }
            nextProps["file_format_version"] = version;
            continue;
        }

        if ( trimmed.front() == '[' && trimmed.back() == ']' ) {
            section = trimmed.substr(1, trimmed.size() - 2);
            continue;
        }

        if ( startsWith(trimmed, "//") || startsWith(trimmed, ";") ) {
            continue;
        }

        if ( section == "Events" ) {
            if ( startsWith(trimmed, "0,") || startsWith(trimmed, "Video,") ) {
                nextProps["Events::background"] = trimmed;
            } else {
                breakLines.push_back(trimmed);
            }
            continue;
        }

        if ( section != "General" && section != "Editor" &&
             section != "Metadata" && section != "Difficulty" ) {
            continue;
        }

        const size_t sep = trimmed.find(':');
        if ( sep == std::string::npos ) {
            errorText = fmt::format("第 {} 行：键值对缺少 ':'。", lineNo);
            return false;
        }

        std::string key   = trimMetadataLine(trimmed.substr(0, sep));
        std::string value = trimMetadataLine(trimmed.substr(sep + 1));
        if ( key.empty() ) {
            errorText = fmt::format("第 {} 行：键名不能为空。", lineNo);
            return false;
        }
        nextProps[fmt::format("{}::{}", section, key)] = value;
    }

    if ( !breakLines.empty() ) {
        std::string breaks;
        for ( const auto& breakLine : breakLines ) {
            if ( !breaks.empty() ) breaks += "\n";
            breaks += breakLine;
        }
        breaks += "\n";
        nextProps["Events::breaks"] = breaks;
    }

    props     = std::move(nextProps);
    errorText = "";
    return true;
}

/// @brief 打开 OSU 元数据纯文本编辑器。
/// @warning UI 交互路径：只更新弹窗状态，不直接修改谱面数据。
void openOsuMetadataTextEditor(const MetadataPropertyMap& props,
                               const BeatMap&             beatmap)
{
    auto& state       = osuMetadataTextEditorState();
    state.open        = true;
    state.requestOpen = true;
    state.errorText.clear();
    copyToInputBuffer(state.textBuffer, buildOsuMetadataText(props, beatmap));
}

/// @brief 消费 OSU 纯文本编辑器的写回结果。
/// @warning UI 每帧绘制路径：只搬移一次结果对象。
std::optional<MetadataPropertyMap> takeOsuMetadataTextResult()
{
    auto& state = osuMetadataTextEditorState();
    if ( !state.result ) return std::nullopt;
    auto result = std::move(state.result);
    state.result.reset();
    return result;
}

/// @brief 渲染 OSU 元数据纯文本编辑弹窗。
/// @warning UI 每帧绘制路径：仅弹窗打开时绘制固定缓冲区。
void renderOsuMetadataTextEditorPopup(float dpiScale)
{
    auto& state = osuMetadataTextEditorState();
    if ( state.requestOpen ) {
        ImGui::OpenPopup("OSU 元数据文本编辑###OsuMetadataTextEditorPopup");
        state.requestOpen = false;
    }

    if ( !state.open ) return;

    ImGui::SetNextWindowSize(ImVec2(680.0f * dpiScale, 560.0f * dpiScale),
                             ImGuiCond_FirstUseEver);
    bool popupOpen = state.open;
    if ( ImGui::BeginPopupModal(
             "OSU 元数据文本编辑###OsuMetadataTextEditorPopup",
             &popupOpen,
             ImGuiWindowFlags_None) ) {
        ImGui::TextUnformatted(
            "按 .osu 文件的 General / Editor / Metadata / Difficulty / Events "
            "格式编辑。");
        if ( !state.errorText.empty() ) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "%s",
                               state.errorText.c_str());
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextMultiline("##OsuMetadataTextBuffer",
                                  state.textBuffer.data(),
                                  state.textBuffer.size(),
                                  ImVec2(0.0f, 430.0f * dpiScale),
                                  ImGuiInputTextFlags_AllowTabInput);

        if ( ImGui::Button("完成##ApplyOsuMetadataText") ) {
            MetadataPropertyMap parsedProps;
            if ( parseOsuMetadataText(
                     state.textBuffer.data(), parsedProps, state.errorText) ) {
                state.result = std::move(parsedProps);
                state.open   = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if ( ImGui::Button("取消##CancelOsuMetadataText") ) {
            state.open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if ( !popupOpen ) {
        state.open = false;
    }
}

/// @brief 根据字段名检查常见 JSON 结构缺失。
/// @warning UI 弹窗绘制路径：仅在 JSON 编辑器打开时解析当前缓冲区。
std::string buildMetadataJsonWarnings(const std::string&    key,
                                      const nlohmann::json& value)
{
    std::vector<std::string> warnings;

    if ( key == "mode_ext" ) {
        if ( !value.is_object() ) {
            warnings.push_back("mode_ext 通常应为对象。");
        } else if ( !value.contains("column") ) {
            warnings.push_back(
                "mode_ext 缺少 column，导出后可能无法保留键数。");
        }
    }

    if ( key == "beat" || key == "endbeat" ) {
        if ( !value.is_array() || value.size() < 3 ) {
            warnings.push_back("beat/endbeat 应为 [小节, 分子, 分母] 数组。");
        }
    }

    if ( key == "seg" ) {
        if ( !value.is_array() ) {
            warnings.push_back("seg 应为数组。");
        } else {
            for ( size_t i = 0; i < value.size(); ++i ) {
                if ( !value[i].is_object() ) {
                    warnings.push_back(fmt::format("seg[{}] 应为对象。", i));
                    continue;
                }
                if ( !value[i].contains("beat") ) {
                    warnings.push_back(fmt::format("seg[{}] 缺少 beat。", i));
                }
            }
        }
    }

    std::string result;
    for ( const auto& warning : warnings ) {
        if ( !result.empty() ) result += "\n";
        result += warning;
    }
    return result;
}

/// @brief 打开某个元数据字段的 JSON 编辑器。
/// @warning UI 交互路径：只更新弹窗状态，不直接修改谱面数据。
void openMetadataJsonEditor(const std::string& scopeId, const std::string& key,
                            const std::string& value)
{
    auto& state       = metadataJsonEditorState();
    state.open        = true;
    state.requestOpen = true;
    state.scopeId     = scopeId;
    state.key         = key;
    state.displayPath =
        scopeId.empty() ? key : fmt::format("{} / {}", scopeId, key);
    state.errorText.clear();
    state.warningText.clear();
    state.childKeyBuffer.fill('\0');
    state.childValueBuffer.fill('\0');

    std::string    initialValue = value.empty() ? "{}" : value;
    nlohmann::json parsed       = parseJsonNoThrow(initialValue.c_str());
    if ( parsed.is_discarded() ) {
        copyToInputBuffer(state.jsonBuffer, initialValue);
    } else {
        copyToInputBuffer(state.jsonBuffer, dumpPrettyJson(parsed));
    }
}

/// @brief 绘制用于打开 JSON 编辑器的字段操作按钮。
/// @warning UI 每帧绘制路径：只绘制按钮和复制当前字段值。
void renderMetadataJsonButton(const std::string& scopeId,
                              const std::string& key, const std::string& value,
                              const std::string& idPrefix)
{
    if ( ImGui::Button(fmt::format("JSON##{}_{}", idPrefix, key).c_str()) ) {
        openMetadataJsonEditor(scopeId, key, value);
    }
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", "打开原生 JSON / 子字段编辑器");
    }
}

/// @brief 绘制新增字段区域的 JSON 编辑按钮。
/// @warning UI 每帧绘制路径：只读取输入缓冲并打开弹窗，不直接修改谱面数据。
void renderNewMetadataJsonButton(const std::string& scopeId, const char* key,
                                 const char* value, const std::string& idPrefix)
{
    const bool hasKey = key != nullptr && key[0] != '\0';
    if ( !hasKey ) {
        ImGui::BeginDisabled();
    }

    if ( ImGui::Button(fmt::format("JSON##{}", idPrefix).c_str()) && hasKey ) {
        openMetadataJsonEditor(
            scopeId,
            key,
            value == nullptr || value[0] == '\0' ? "{}" : std::string(value));
    }

    if ( !hasKey ) {
        ImGui::EndDisabled();
    }

    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s",
                          hasKey ? "用原生 JSON / 子字段编辑器创建该字段"
                                 : "先填写键名，再打开 JSON 编辑器");
    }
}

/// @brief 消费 JSON 编辑器的写回结果。
/// @warning UI 每帧绘制路径：只搬移一次小型结果对象。
std::optional<MetadataJsonEditResult> takeMetadataJsonEditResult(
    const std::string& scopeId)
{
    auto& state = metadataJsonEditorState();
    if ( !state.result || state.result->scopeId != scopeId ) {
        return std::nullopt;
    }

    auto result = state.result;
    state.result.reset();
    return result;
}

/// @brief 渲染元数据字段 JSON 编辑弹窗。
/// @warning UI 每帧绘制路径：仅弹窗打开时解析当前缓冲区并绘制 JSON 辅助工具。
void renderMetadataJsonEditorPopup(float dpiScale)
{
    auto& state = metadataJsonEditorState();
    if ( state.requestOpen ) {
        ImGui::OpenPopup("字段 JSON 编辑###MetadataJsonEditorPopup");
        state.requestOpen = false;
    }

    if ( !state.open ) return;

    ImGui::SetNextWindowSize(ImVec2(620.0f * dpiScale, 520.0f * dpiScale),
                             ImGuiCond_FirstUseEver);
    bool popupOpen = state.open;
    if ( ImGui::BeginPopupModal("字段 JSON 编辑###MetadataJsonEditorPopup",
                                &popupOpen,
                                ImGuiWindowFlags_None) ) {
        ImGui::Text("字段: %s", state.displayPath.c_str());
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f),
                           "%s",
                           "可直接编辑合法 JSON，也可以向对象字段添加子字段。");
        ImGui::Separator();

        nlohmann::json parsed = parseJsonNoThrow(state.jsonBuffer.data());
        if ( parsed.is_discarded() ) {
            state.warningText.clear();
            if ( state.errorText.empty() ) {
                state.errorText = "当前内容不是合法 JSON。";
            }
        } else {
            state.errorText.clear();
            state.warningText = buildMetadataJsonWarnings(state.key, parsed);
        }

        if ( !state.errorText.empty() ) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "%s",
                               state.errorText.c_str());
        }
        if ( !state.warningText.empty() ) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                               "%s",
                               state.warningText.c_str());
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextMultiline("##MetadataJsonRawEditor",
                                  state.jsonBuffer.data(),
                                  state.jsonBuffer.size(),
                                  ImVec2(0.0f, 220.0f * dpiScale),
                                  ImGuiInputTextFlags_AllowTabInput);

        if ( ImGui::Button("格式化##FormatMetadataJson") ) {
            parsed = parseJsonNoThrow(state.jsonBuffer.data());
            if ( parsed.is_discarded() ) {
                state.errorText = "无法格式化：当前内容不是合法 JSON。";
            } else {
                copyToInputBuffer(state.jsonBuffer, dumpPrettyJson(parsed));
                state.errorText.clear();
            }
        }
        ImGui::SameLine();
        if ( ImGui::Button("重建为空对象##ResetMetadataJsonObject") ) {
            copyToInputBuffer(state.jsonBuffer, "{}");
            state.errorText.clear();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("子字段");
        parsed = parseJsonNoThrow(state.jsonBuffer.data());
        if ( parsed.is_object() ) {
            if ( ImGui::BeginTable("MetadataJsonChildrenTable",
                                   3,
                                   ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_BordersOuter |
                                       ImGuiTableFlags_Resizable,
                                   ImVec2(0.0f, 110.0f * dpiScale)) ) {
                ImGui::TableSetupColumn(
                    "Key", ImGuiTableColumnFlags_WidthFixed, 170.0f * dpiScale);
                ImGui::TableSetupColumn("Value",
                                        ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Action",
                                        ImGuiTableColumnFlags_WidthFixed,
                                        70.0f * dpiScale);
                ImGui::TableHeadersRow();

                std::vector<std::string> childKeys;
                for ( auto it = parsed.begin(); it != parsed.end(); ++it ) {
                    childKeys.push_back(it.key());
                }

                for ( const auto& childKey : childKeys ) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(childKey.c_str());
                    ImGui::TableNextColumn();
                    std::string childValue = parsed[childKey].dump();
                    ImGui::TextWrapped("%s", childValue.c_str());
                    ImGui::TableNextColumn();
                    if ( ImGui::Button(
                             fmt::format("删除##JsonChildDel_{}", childKey)
                                 .c_str()) ) {
                        parsed.erase(childKey);
                        copyToInputBuffer(state.jsonBuffer,
                                          dumpPrettyJson(parsed));
                        break;
                    }
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextDisabled("%s", "当前 JSON 不是对象，无法列出子字段。");
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("键:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f * dpiScale);
        ImGui::InputText("##MetadataJsonChildKey",
                         state.childKeyBuffer.data(),
                         state.childKeyBuffer.size());
        ImGui::SameLine();
        ImGui::TextUnformatted("值:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(210.0f * dpiScale);
        ImGui::InputText("##MetadataJsonChildValue",
                         state.childValueBuffer.data(),
                         state.childValueBuffer.size());
        ImGui::SameLine();
        if ( ImGui::Button("添加/覆盖##MetadataJsonChildAdd") ) {
            std::string childKey = state.childKeyBuffer.data();
            if ( childKey.empty() ) {
                state.errorText = "子字段键名不能为空。";
            } else {
                parsed = parseJsonNoThrow(state.jsonBuffer.data());
                if ( parsed.is_discarded() || !parsed.is_object() ) {
                    parsed = nlohmann::json::object();
                }
                parsed[childKey] =
                    parseChildValueOrString(state.childValueBuffer.data());
                copyToInputBuffer(state.jsonBuffer, dumpPrettyJson(parsed));
                state.childKeyBuffer.fill('\0');
                state.childValueBuffer.fill('\0');
                state.errorText.clear();
            }
        }

        ImGui::Separator();
        if ( ImGui::Button("完成##ApplyMetadataJson") ) {
            parsed = parseJsonNoThrow(state.jsonBuffer.data());
            if ( parsed.is_discarded() ) {
                state.errorText = "无法完成：当前内容不是合法 JSON。";
            } else {
                state.warningText =
                    buildMetadataJsonWarnings(state.key, parsed);
                state.result = MetadataJsonEditResult{ state.scopeId,
                                                       state.key,
                                                       dumpPrettyJson(parsed) };
                state.open   = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if ( ImGui::Button("取消##CancelMetadataJson") ) {
            state.open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if ( !popupOpen ) {
        state.open = false;
    }
}
}  // namespace

/// @brief 渲染谱面元数据编辑窗口。
void MainMenuView::renderMetadataEditorWindow()
{
    if ( !m_showMetadataEditorWindow ) return;

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    ImGui::SetNextWindowSize(ImVec2(800.0f * dpiScale, 550.0f * dpiScale),
                             ImGuiCond_FirstUseEver);

    auto&   skinMgr   = Config::SkinManager::instance();
    ImFont* titleFont = skinMgr.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont);

    bool opened = ImGui::Begin("谱面额外元数据编辑###MetadataEditorWindow",
                               &m_showMetadataEditorWindow,
                               ImGuiWindowFlags_None);

    if ( titleFont ) ImGui::PopFont();

    if ( opened ) {
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( !session ) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "当前无活动的编辑器会话。");
        } else {
            auto beatmap = session->getContext().currentBeatmap;
            if ( !beatmap ) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "当前未加载任何谱面，请先打开或新建谱面。");
            } else {
                ImGui::Text("正在编辑谱面: %s (%s)",
                            beatmap->m_baseMapMetadata.name.c_str(),
                            beatmap->m_baseMapMetadata.version.c_str());
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                   "提示：在此处修改的额外字段会在保存谱面时一"
                                   "同导出。键入即可开始编辑。");
                ImGui::Separator();
                ImGui::Spacing();

                if ( ImGui::BeginTabBar("MetadataTypeTabBar") ) {
                    // ==================== OSU TAB ====================
                    if ( ImGui::BeginTabItem("osu! (OSU) 格式元数据") ) {
                        // Predefined OSU fields
                        static const MetadataFieldList OSU_FIELDS = {
                            { "file_format_version",
                              "osu! 文件格式版本 - 例如 v14" },
                            { "General::AudioFilename",
                              "音频文件名 - 谱面所使用的音频文件路径" },
                            { "General::AudioLeadIn",
                              "音频前导时间 (ms) - "
                              "谱面开始播放前的缓冲时间" },
                            { "General::AudioHash",
                              "音频哈希值 - 音频文件的 MD5" },
                            { "General::PreviewTime",
                              "预览开始时间 (ms) - "
                              "选歌界面预览音频的起点" },
                            { "General::Countdown",
                              "倒计时样式 - 0=无, 1=普通, 2=快速, 3=极速" },
                            { "General::SampleSet",
                              "默认音效样本组 - Normal, Soft, Drum" },
                            { "General::StackLeniency",
                              "堆叠容差 - 影响连打/重叠物件的错开位移" },
                            { "General::Mode",
                              "游戏模式 - 0=osu!, 1=Taiko, 2=Catch, "
                              "3=Mania" },
                            { "General::LetterboxInBreaks",
                              "休息段显示黑边 - 0/1" },
                            { "General::StoryFireInFront",
                              "故事板火花在前 - 0/1" },
                            { "General::UseSkinSprites", "使用皮肤精灵 - 0/1" },
                            { "General::AlwaysShowPlayfield",
                              "总是显示活动区域 - 0/1" },
                            { "General::OverlayPosition",
                              "界面覆盖层位置 - NoChange, Below, Above" },
                            { "General::SkinPreference", "推荐皮肤名称" },
                            { "General::EpilepsyWarning", "癫痫警告 - 0/1" },
                            { "General::CountdownOffset",
                              "倒计时偏移时间 (ms)" },
                            { "General::SpecialStyle",
                              "特殊样式 - 0/1, mania 中用于 N+1 键位布局" },
                            { "General::WidescreenStoryboard",
                              "宽屏故事板 - 0/1" },
                            { "General::SamplesMatchPlaybackRate",
                              "音效速率跟随播放速度 - 0/1" },

                            { "Editor::Bookmarks",
                              "书签 - 逗号分隔的毫秒整型数组" },
                            { "Editor::DistanceSpacing", "距离间距系数" },
                            { "Editor::BeatDivisor",
                              "节拍细分数 - 例如 4, 8, 12, 16" },
                            { "Editor::GridSize", "网格大小" },
                            { "Editor::TimelineZoom", "时间轴缩放倍率" },

                            { "Metadata::Title", "歌曲标题 (对应 base 标题)" },
                            { "Metadata::TitleUnicode",
                              "歌曲标题 (原语/Unicode)" },
                            { "Metadata::Artist", "艺术家 (对应 base 艺术家)" },
                            { "Metadata::ArtistUnicode",
                              "艺术家 (原语/Unicode)" },
                            { "Metadata::Creator", "谱面创作者" },
                            { "Metadata::Version", "难度版本名" },
                            { "Metadata::Source", "歌曲来源 - 如动漫/游戏名" },
                            { "Metadata::Tags", "检索标签 - 空格分隔" },
                            { "Metadata::BeatmapID", "谱面唯一 ID (官网分配)" },
                            { "Metadata::BeatmapSetID",
                              "谱面集唯一 ID (官网分配)" },

                            { "Difficulty::HPDrainRate", "HP 减少速率 (0-10)" },
                            { "Difficulty::CircleSize", "键数 / 轨道数" },
                            { "Difficulty::OverallDifficulty",
                              "综合难度 / 判定严准度 (0-10)" },
                            { "Difficulty::ApproachRate",
                              "缩圈速度 / 下落速度 (0-10)" },
                            { "Difficulty::SliderMultiplier", "滑条速度倍率" },
                            { "Difficulty::SliderTickRate",
                              "滑条 Tick 生成率" },

                            { "Events::background",
                              "背景图片设置串 - 格式: 0,0,\"文件名\",x,y" },
                            { "Events::breaks", "休息时间段定义串" }
                        };

                        auto& props = beatmap->m_metadata
                                          .map_properties[MapMetadataType::OSU];
                        if ( auto result = takeOsuMetadataTextResult() ) {
                            props = std::move(*result);
                            ensureCompleteOsuMetadata(
                                props, *beatmap, OSU_FIELDS);
                            syncOsuMetadataToBase(props, *beatmap);
                        }
                        if ( !props.empty() ) {
                            ensureCompleteOsuMetadata(
                                props, *beatmap, OSU_FIELDS);
                            syncOsuMetadataToBase(props, *beatmap);
                        }

                        bool osuPropsChanged = false;

                        if ( props.empty() ) {
                            ImGui::BeginDisabled();
                        }
                        if ( ImGui::Button("文本编辑##open_osu_text_editor") &&
                             !props.empty() ) {
                            openOsuMetadataTextEditor(props, *beatmap);
                        }
                        if ( props.empty() ) {
                            ImGui::EndDisabled();
                        }
                        if ( ImGui::IsItemHovered() ) {
                            ImGui::SetTooltip(
                                "%s",
                                props.empty()
                                    ? "先添加至少一个 OSU "
                                      "元数据字段，随后会补齐"
                                      "完整默认表。"
                                    : "打开 .osu 风格纯文本元数据编辑器");
                        }

                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;

                        float footerHeight = 45.0f * dpiScale;
                        if ( ImGui::BeginTable("OSUMetadataTable",
                                               4,
                                               tableFlags,
                                               ImVec2(0.0f, -footerHeight)) ) {
                            ImGui::TableSetupColumn(
                                "键名 (Key)",
                                ImGuiTableColumnFlags_WidthFixed,
                                220.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "描述 (Description)",
                                ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(
                                "数值 (Value)",
                                ImGuiTableColumnFlags_WidthFixed,
                                250.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "操作 (Action)",
                                ImGuiTableColumnFlags_WidthFixed,
                                130.0f * dpiScale);
                            ImGui::TableHeadersRow();

                            // 1. Render predefined fields
                            for ( const auto& [key, desc] : OSU_FIELDS ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();

                                bool hasKey = props.contains(key);
                                if ( !hasKey ) {
                                    ImGui::PushStyleColor(
                                        ImGuiCol_Text,
                                        ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                }
                                ImGui::TextUnformatted(key.c_str());
                                if ( !hasKey ) {
                                    ImGui::PopStyleColor();
                                }
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip(
                                        "双击可以复制该内置键名到剪贴板。");
                                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                                        ImGui::SetClipboardText(key.c_str());
                                    }
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::PushStyleColor(
                                    ImGuiCol_Text,
                                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                ImGui::TextWrapped("%s", desc.c_str());
                                ImGui::PopStyleColor();

                                ImGui::TableNextColumn();
                                char valBuf[1024] = { 0 };
                                if ( hasKey ) {
                                    std::string currentVal = props.at(key);
                                    size_t      copyLen    = std::min(
                                        currentVal.size(), sizeof(valBuf) - 1);
                                    std::copy(currentVal.begin(),
                                              currentVal.begin() + copyLen,
                                              valBuf);
                                }

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_osu_") + key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key]      = valBuf;
                                    osuPropsChanged = true;
                                }

                                ImGui::TableNextColumn();
                                const bool isPreviewTimeKey =
                                    key == "General::PreviewTime";
                                if ( isPreviewTimeKey ) {
                                    if ( ImGui::Button(
                                             (std::string(
                                                  "当前##current_osu_") +
                                              key)
                                                 .c_str()) ) {
                                        props[key] =
                                            readCurrentJudgelinePreviewMsText();
                                        osuPropsChanged = true;
                                    }
                                    if ( ImGui::IsItemHovered() ) {
                                        ImGui::SetTooltip(
                                            "%s",
                                            "读取当前判定线时间并写入毫秒值。");
                                    }
                                    if ( hasKey ) {
                                        ImGui::SameLine();
                                    }
                                }
                                if ( hasKey ) {
                                    if ( ImGui::Button(
                                             (std::string("默认##reset_osu_") +
                                              key)
                                                 .c_str()) ) {
                                        props[key] = getOsuMetadataDefaultValue(
                                            *beatmap, key);
                                        osuPropsChanged = true;
                                    }
                                } else if ( !isPreviewTimeKey ) {
                                    ImGui::TextDisabled("-");
                                }
                            }

                            // 2. Render other custom fields
                            std::vector<std::string> customKeys;
                            for ( const auto& [k, v] : props ) {
                                bool isPredefined = false;
                                for ( const auto& [pk, pd] : OSU_FIELDS ) {
                                    if ( pk == k ) {
                                        isPredefined = true;
                                        break;
                                    }
                                }
                                if ( !isPredefined ) {
                                    customKeys.push_back(k);
                                }
                            }

                            for ( const auto& key : customKeys ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(key.c_str());
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip("这是一个自定义键。");
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextDisabled("自定义键值");

                                ImGui::TableNextColumn();
                                char        valBuf[1024] = { 0 };
                                std::string currentVal   = props.at(key);
                                size_t copyLen = std::min(currentVal.size(),
                                                          sizeof(valBuf) - 1);
                                std::copy(currentVal.begin(),
                                          currentVal.begin() + copyLen,
                                          valBuf);

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_osu_custom_") +
                                          key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key]      = valBuf;
                                    osuPropsChanged = true;
                                }

                                ImGui::TableNextColumn();
                                if ( ImGui::Button(
                                         (std::string("删除##del_osu_") + key)
                                             .c_str()) ) {
                                    props.erase(key);
                                    osuPropsChanged = true;
                                }
                            }

                            ImGui::EndTable();
                        }

                        // Add new field form
                        ImGui::Separator();
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("新增自定义键名:");
                        ImGui::SameLine();
                        static char newOsuKey[128] = "";
                        ImGui::SetNextItemWidth(200.0f * dpiScale);
                        ImGui::InputText(
                            "##new_osu_key", newOsuKey, sizeof(newOsuKey));

                        ImGui::SameLine();
                        ImGui::Text("数值:");
                        ImGui::SameLine();
                        static char newOsuVal[256] = "";
                        ImGui::SetNextItemWidth(250.0f * dpiScale);
                        ImGui::InputText(
                            "##new_osu_val", newOsuVal, sizeof(newOsuVal));

                        ImGui::SameLine();
                        if ( ImGui::Button("添加##add_osu_field") ) {
                            std::string nk = newOsuKey;
                            if ( !nk.empty() ) {
                                props[nk] = newOsuVal;
                                ensureCompleteOsuMetadata(
                                    props, *beatmap, OSU_FIELDS);
                                syncOsuMetadataToBase(props, *beatmap);
                                newOsuKey[0] = '\0';
                                newOsuVal[0] = '\0';
                            }
                        }

                        if ( osuPropsChanged && !props.empty() ) {
                            ensureCompleteOsuMetadata(
                                props, *beatmap, OSU_FIELDS);
                            syncOsuMetadataToBase(props, *beatmap);
                        }

                        ImGui::EndTabItem();
                    }

                    // ==================== MALODY TAB ====================
                    if ( ImGui::BeginTabItem("Malody (MALODY) 格式元数据") ) {
                        static const std::vector<
                            std::pair<std::string, std::string>>
                            MALODY_FIELDS = {
                                { "id", "谱面 ID" },
                                { "preview",
                                  "音频预览时间戳 (ms) - 选歌界面试听起点" },
                                { "mode",
                                  "游戏模式 - 0=Key 模式，7=Slide 模式" },
                                { "$ver", "文件格式版本" },
                                { "aimode", "AI 辅助模式配置" },
                                { "mode_ext", "模式额外扩展配置 (JSON 串)" },
                                { "extra", "额外顶层扩展配置 (JSON 串)" },
                                { "initialDelay",
                                  "初始节拍延迟 / 时间戳首点 (ms)" },
                                { "audioOffset", "音频时间偏移 (ms)" }
                            };

                        auto& props =
                            beatmap->m_metadata
                                .map_properties[MapMetadataType::MALODY];
                        const std::string metadataScopeId = "map_malody";
                        if ( auto result =
                                 takeMetadataJsonEditResult(metadataScopeId) ) {
                            props[result->key] = result->value;
                        }

                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;

                        float footerHeight = 45.0f * dpiScale;
                        if ( ImGui::BeginTable("MalodyMetadataTable",
                                               4,
                                               tableFlags,
                                               ImVec2(0.0f, -footerHeight)) ) {
                            ImGui::TableSetupColumn(
                                "键名 (Key)",
                                ImGuiTableColumnFlags_WidthFixed,
                                220.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "描述 (Description)",
                                ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(
                                "数值 (Value)",
                                ImGuiTableColumnFlags_WidthFixed,
                                250.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "操作 (Action)",
                                ImGuiTableColumnFlags_WidthFixed,
                                190.0f * dpiScale);
                            ImGui::TableHeadersRow();

                            // 1. Render predefined fields
                            for ( const auto& [key, desc] : MALODY_FIELDS ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();

                                bool hasKey = props.contains(key);
                                if ( !hasKey ) {
                                    ImGui::PushStyleColor(
                                        ImGuiCol_Text,
                                        ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                }
                                ImGui::TextUnformatted(key.c_str());
                                if ( !hasKey ) {
                                    ImGui::PopStyleColor();
                                }
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip(
                                        "双击可以复制该内置键名到剪贴板。");
                                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                                        ImGui::SetClipboardText(key.c_str());
                                    }
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::PushStyleColor(
                                    ImGuiCol_Text,
                                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                ImGui::TextWrapped("%s", desc.c_str());
                                ImGui::PopStyleColor();

                                ImGui::TableNextColumn();
                                char valBuf[1024] = { 0 };
                                if ( hasKey ) {
                                    std::string currentVal = props.at(key);
                                    size_t      copyLen    = std::min(
                                        currentVal.size(), sizeof(valBuf) - 1);
                                    std::copy(currentVal.begin(),
                                              currentVal.begin() + copyLen,
                                              valBuf);
                                }

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_mld_") + key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key] = valBuf;
                                }

                                ImGui::TableNextColumn();
                                const std::string fieldValue =
                                    hasKey ? props.at(key) : std::string();
                                renderMetadataJsonButton(metadataScopeId,
                                                         key,
                                                         fieldValue,
                                                         "map_mld_builtin");
                                if ( key == "preview" ) {
                                    ImGui::SameLine();
                                    if ( ImGui::Button(
                                             "当前##current_mld_preview") ) {
                                        props[key] =
                                            readCurrentJudgelinePreviewMsText();
                                    }
                                    if ( ImGui::IsItemHovered() ) {
                                        ImGui::SetTooltip(
                                            "%s",
                                            "读取当前判定线时间并写入毫秒值。");
                                    }
                                }
                                if ( hasKey ) {
                                    ImGui::SameLine();
                                    if ( ImGui::Button(
                                             (std::string("清除##clear_mld_") +
                                              key)
                                                 .c_str()) ) {
                                        props.erase(key);
                                    }
                                }
                            }

                            // 2. Render other custom fields
                            std::vector<std::string> customKeys;
                            for ( const auto& [k, v] : props ) {
                                bool isPredefined = false;
                                for ( const auto& [pk, pd] : MALODY_FIELDS ) {
                                    if ( pk == k ) {
                                        isPredefined = true;
                                        break;
                                    }
                                }
                                if ( !isPredefined ) {
                                    customKeys.push_back(k);
                                }
                            }

                            for ( const auto& key : customKeys ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(key.c_str());
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip("这是一个自定义键。");
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextDisabled("自定义键值");

                                ImGui::TableNextColumn();
                                char        valBuf[1024] = { 0 };
                                std::string currentVal   = props.at(key);
                                size_t copyLen = std::min(currentVal.size(),
                                                          sizeof(valBuf) - 1);
                                std::copy(currentVal.begin(),
                                          currentVal.begin() + copyLen,
                                          valBuf);

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_mld_custom_") +
                                          key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key] = valBuf;
                                }

                                ImGui::TableNextColumn();
                                renderMetadataJsonButton(metadataScopeId,
                                                         key,
                                                         props.at(key),
                                                         "map_mld_custom");
                                ImGui::SameLine();
                                if ( ImGui::Button(
                                         (std::string("删除##del_mld_") + key)
                                             .c_str()) ) {
                                    props.erase(key);
                                }
                            }

                            ImGui::EndTable();
                        }

                        // Add new field form
                        ImGui::Separator();
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("新增自定义键名:");
                        ImGui::SameLine();
                        static char newMldKey[128] = "";
                        ImGui::SetNextItemWidth(200.0f * dpiScale);
                        ImGui::InputText(
                            "##new_mld_key", newMldKey, sizeof(newMldKey));

                        ImGui::SameLine();
                        ImGui::Text("数值:");
                        ImGui::SameLine();
                        static char newMldVal[256] = "";
                        ImGui::SetNextItemWidth(250.0f * dpiScale);
                        ImGui::InputText(
                            "##new_mld_val", newMldVal, sizeof(newMldVal));

                        ImGui::SameLine();
                        if ( ImGui::Button("添加##add_mld_field") ) {
                            std::string nk = newMldKey;
                            if ( !nk.empty() ) {
                                props[nk]    = newMldVal;
                                newMldKey[0] = '\0';
                                newMldVal[0] = '\0';
                            }
                        }

                        ImGui::EndTabItem();
                    }

                    // ==================== RM TAB ====================
                    if ( ImGui::BeginTabItem("RM (RM) 格式元数据") ) {
                        static const std::vector<
                            std::pair<std::string, std::string>>
                            RM_FIELDS = { { "mapLength",
                                            "谱面长度，RM/IMD 文件头 int32" },
                                          { "tabRows",
                                            "表格行数，RM/IMD 物件表 int32" } };

                        auto& props = beatmap->m_metadata
                                          .map_properties[MapMetadataType::RM];

                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;

                        if ( ImGui::BeginTable("RMMetadataTable",
                                               3,
                                               tableFlags,
                                               ImVec2(0.0f, 0.0f)) ) {
                            ImGui::TableSetupColumn(
                                "键名 (Key)",
                                ImGuiTableColumnFlags_WidthFixed,
                                220.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "描述 (Description)",
                                ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(
                                "数值 (Value)",
                                ImGuiTableColumnFlags_WidthFixed,
                                250.0f * dpiScale);
                            ImGui::TableHeadersRow();

                            for ( const auto& [key, desc] : RM_FIELDS ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();

                                bool hasKey = props.contains(key);
                                if ( !hasKey ) {
                                    ImGui::PushStyleColor(
                                        ImGuiCol_Text,
                                        ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                }
                                ImGui::TextUnformatted(key.c_str());
                                if ( !hasKey ) {
                                    ImGui::PopStyleColor();
                                }
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip(
                                        "双击可以复制该内置键名到剪贴板。");
                                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                                        ImGui::SetClipboardText(key.c_str());
                                    }
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::PushStyleColor(
                                    ImGuiCol_Text,
                                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                ImGui::TextWrapped("%s", desc.c_str());
                                ImGui::PopStyleColor();

                                ImGui::TableNextColumn();
                                int32_t defaultValue = 0;
                                if ( key == "mapLength" ) {
                                    defaultValue = clampDoubleToInt32(
                                        beatmap->m_baseMapMetadata.map_length);
                                }

                                int32_t value = defaultValue;
                                if ( hasKey ) {
                                    value = parseInt32Metadata(props.at(key))
                                                .value_or(defaultValue);
                                }

                                int valueInput = value;
                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputInt(
                                         (std::string("##val_rm_") + key)
                                             .c_str(),
                                         &valueInput,
                                         1,
                                         100) ) {
                                    props[key] = std::to_string(
                                        static_cast<int32_t>(valueInput));
                                }
                                if ( hasKey &&
                                     !parseInt32Metadata(props.at(key))
                                          .has_value() ) {
                                    ImGui::TextColored(
                                        ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                        "%s",
                                        "当前值不是合法 int32，编辑后会修正。");
                                }
                            }

                            ImGui::EndTable();
                        }

                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }
            }
        }
    }
    if ( opened ) {
        renderOsuMetadataTextEditorPopup(dpiScale);
        renderMetadataJsonEditorPopup(dpiScale);
    }
    ImGui::End();

    ImGui::PopStyleVar(6);
}

/// @brief 渲染选中音符元数据编辑窗口。
void MainMenuView::renderNoteMetadataEditorWindow()
{
    if ( !m_showNoteMetadataEditorWindow ) return;

    struct InputBuffer {
        char key[128] = "";
        char val[256] = "";
    };
    static std::unordered_map<std::string, InputBuffer> inputBuffers;
    static bool                                         lastShowState = false;
    if ( m_showNoteMetadataEditorWindow && !lastShowState ) {
        inputBuffers.clear();
    }
    lastShowState = m_showNoteMetadataEditorWindow;

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    ImGui::SetNextWindowSize(ImVec2(750.0f * dpiScale, 500.0f * dpiScale),
                             ImGuiCond_FirstUseEver);

    auto&   skinMgr   = Config::SkinManager::instance();
    ImFont* titleFont = skinMgr.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont);

    std::string windowTitle =
        std::string(TR("ui.edit.note_metadata.title").data()) +
        "###NoteMetadataEditorWindow";
    bool opened = ImGui::Begin(windowTitle.c_str(),
                               &m_showNoteMetadataEditorWindow,
                               ImGuiWindowFlags_None);

    if ( titleFont ) ImGui::PopFont();

    if ( opened ) {
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( !session ) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "%s",
                               TR("ui.tools.no_active_session").data());
        } else {
            // --- 收集选中物件 ---
            struct SelectedNote {
                entt::entity entity;
                double       timestamp;
            };
            std::vector<SelectedNote> selectedNotes;
            auto& registry = session->getContextMutable().noteRegistry;

            auto view = registry.view<const Logic::NoteComponent,
                                      const Logic::InteractionComponent>();
            for ( auto e : view ) {
                const auto& interaction =
                    view.get<const Logic::InteractionComponent>(e);
                if ( interaction.isSelected ) {
                    const auto& nc = view.get<const Logic::NoteComponent>(e);
                    selectedNotes.push_back({ e, nc.m_timestamp });
                }
            }

            if ( selectedNotes.empty() ) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                    "%s",
                    TR("ui.edit.note_metadata.no_selection").data());
            } else {
                // --- 按元数据指纹分组 ---
                // 将 note_properties 序列化为字符串作为分组键
                auto serializeMetadata =
                    [](const ::MMM::NoteMetadata& meta) -> std::string {
                    std::string result;
                    // 按 NoteMetadataType 排序遍历
                    for ( int typeIdx = 0; typeIdx < 4; ++typeIdx ) {
                        auto metaType =
                            static_cast<::MMM::NoteMetadataType>(typeIdx);
                        auto it = meta.note_properties.find(metaType);
                        if ( it == meta.note_properties.end() ) continue;
                        if ( it->second.empty() ) continue;
                        result += std::to_string(typeIdx) + "{";
                        // 收集并排序 key
                        std::vector<std::string> keys;
                        for ( const auto& [k, v] : it->second ) {
                            keys.push_back(k);
                        }
                        std::sort(keys.begin(), keys.end());
                        for ( const auto& k : keys ) {
                            result += k + "=" + it->second.at(k) + ";";
                        }
                        result += "}";
                    }
                    return result;
                };

                struct MetadataGroup {
                    std::vector<entt::entity> entities;
                    double                    minTime;
                    double                    maxTime;
                };
                std::map<std::string, MetadataGroup> groups;

                for ( const auto& sn : selectedNotes ) {
                    auto& nc = registry.get<Logic::NoteComponent>(sn.entity);
                    std::string fingerprint = serializeMetadata(nc.m_metadata);
                    auto&       group       = groups[fingerprint];
                    group.entities.push_back(sn.entity);
                    if ( group.entities.size() == 1 ) {
                        group.minTime = sn.timestamp;
                        group.maxTime = sn.timestamp;
                    } else {
                        group.minTime = std::min(group.minTime, sn.timestamp);
                        group.maxTime = std::max(group.maxTime, sn.timestamp);
                    }
                }

                // --- 摘要信息 ---
                std::string summaryStr = TR_FMT("ui.edit.note_metadata.summary",
                                                selectedNotes.size(),
                                                groups.size());
                ImGui::TextUnformatted(summaryStr.c_str());
                ImGui::Separator();
                ImGui::Spacing();

                // --- 各组的编辑区域 ---
                ImGui::BeginChild("NoteMetaGroups",
                                  ImVec2(0, 0),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_None);

                auto applyNoteJsonEditResult =
                    [&](const std::string&               scopeId,
                        ::MMM::NoteMetadataType          metaType,
                        const std::vector<entt::entity>& entities) {
                        if ( auto result =
                                 takeMetadataJsonEditResult(scopeId) ) {
                            for ( auto e : entities ) {
                                auto& nc =
                                    registry.get<Logic::NoteComponent>(e);
                                nc.m_metadata
                                    .note_properties[metaType][result->key] =
                                    result->value;
                            }
                        }
                    };

                int groupIdx = 0;
                for ( auto& [fingerprint, group] : groups ) {
                    ++groupIdx;
                    const auto minTimeText =
                        Canvas::formatCanvasTime(group.minTime);
                    const auto maxTimeText =
                        Canvas::formatCanvasTime(group.maxTime);
                    std::string headerStr =
                        TR_FMT("ui.edit.note_metadata.group_header",
                               groupIdx,
                               group.entities.size(),
                               minTimeText,
                               maxTimeText);

                    ImGui::PushID(groupIdx);

                    bool headerOpen = ImGui::CollapsingHeader(
                        headerStr.c_str(),
                        groups.size() == 1 ? ImGuiTreeNodeFlags_DefaultOpen
                                           : ImGuiTreeNodeFlags_None);

                    if ( headerOpen ) {
                        // 取第一个实体作为代表
                        auto  firstEntity = group.entities.front();
                        auto& firstNc =
                            registry.get<Logic::NoteComponent>(firstEntity);

                        if ( ImGui::BeginTabBar(
                                 fmt::format("MetaTabBar_{}", groupIdx)
                                     .c_str()) ) {
                            // --- OSU 标签页 ---
                            if ( ImGui::BeginTabItem("OSU") ) {
                                auto metaType = ::MMM::NoteMetadataType::OSU;
                                auto it =
                                    firstNc.m_metadata.note_properties.find(
                                        metaType);
                                static const decltype(firstNc.m_metadata
                                                          .note_properties)::
                                    mapped_type emptyMap;
                                const auto&     refProps =
                                    (it !=
                                     firstNc.m_metadata.note_properties.end())
                                        ? it->second
                                        : emptyMap;

                                ImGuiTableFlags tableFlags =
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_BordersOuter |
                                    ImGuiTableFlags_Resizable;

                                if ( ImGui::BeginTable(
                                         fmt::format("NoteMetaTable_OSU_{}",
                                                     groupIdx)
                                             .c_str(),
                                         3,
                                         tableFlags,
                                         ImVec2(0.0f, 0.0f)) ) {
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.key_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        220.0f * dpiScale);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.value_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.action_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        60.0f * dpiScale);
                                    ImGui::TableHeadersRow();

                                    std::vector<std::string> keys;
                                    for ( const auto& [k, v] : refProps ) {
                                        keys.push_back(k);
                                    }
                                    std::sort(keys.begin(), keys.end());

                                    for ( const auto& key : keys ) {
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();
                                        ImGui::AlignTextToFramePadding();
                                        ImGui::TextUnformatted(key.c_str());

                                        ImGui::TableNextColumn();
                                        char        valBuf[1024] = { 0 };
                                        std::string currentVal =
                                            refProps.at(key);
                                        size_t copyLen =
                                            std::min(currentVal.size(),
                                                     sizeof(valBuf) - 1);
                                        std::copy(currentVal.begin(),
                                                  currentVal.begin() + copyLen,
                                                  valBuf);

                                        ImGui::SetNextItemWidth(-1.0f);
                                        if ( ImGui::InputText(
                                                 fmt::format("##nm_osu_{}_{}",
                                                             groupIdx,
                                                             key)
                                                     .c_str(),
                                                 valBuf,
                                                 sizeof(valBuf)) ) {
                                            // 写回所有同组实体
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                nc.m_metadata
                                                    .note_properties[metaType]
                                                                    [key] =
                                                    valBuf;
                                            }
                                        }

                                        ImGui::TableNextColumn();
                                        if ( ImGui::Button(
                                                 fmt::format(
                                                     "{}##clr_osu_{}_{}",
                                                     TR("ui.edit.note_"
                                                        "metadata.clear_"
                                                        "btn")
                                                         .data(),
                                                     groupIdx,
                                                     key)
                                                     .c_str()) ) {
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                auto& m = nc.m_metadata
                                                              .note_properties
                                                                  [metaType];
                                                m.erase(key);
                                                if ( m.empty() ) {
                                                    nc.m_metadata
                                                        .note_properties.erase(
                                                            metaType);
                                                }
                                            }
                                        }
                                    }
                                    ImGui::EndTable();
                                }

                                // 新增字段
                                ImGui::Separator();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_key").data());
                                ImGui::SameLine();
                                std::string bufKey =
                                    fmt::format("{}_{}",
                                                groupIdx,
                                                static_cast<int>(metaType));
                                auto& buf = inputBuffers[bufKey];
                                ImGui::SetNextItemWidth(150.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmk_osu_{}", groupIdx)
                                        .c_str(),
                                    buf.key,
                                    sizeof(buf.key));
                                ImGui::SameLine();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_value")
                                        .data());
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(200.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmv_osu_{}", groupIdx)
                                        .c_str(),
                                    buf.val,
                                    sizeof(buf.val));
                                ImGui::SameLine();
                                if ( ImGui::Button(
                                         fmt::format("{}##nma_osu_{}",
                                                     TR("ui.edit.note_metadata."
                                                        "add_btn")
                                                         .data(),
                                                     groupIdx)
                                             .c_str()) ) {
                                    std::string nk = buf.key;
                                    if ( !nk.empty() ) {
                                        for ( auto e : group.entities ) {
                                            auto& nc =
                                                registry
                                                    .get<Logic::NoteComponent>(
                                                        e);
                                            nc.m_metadata
                                                .note_properties[metaType][nk] =
                                                buf.val;
                                        }
                                        buf.key[0] = '\0';
                                        buf.val[0] = '\0';
                                    }
                                }

                                ImGui::EndTabItem();
                            }

                            // --- MALODY 标签页 ---
                            if ( ImGui::BeginTabItem("MALODY") ) {
                                auto metaType = ::MMM::NoteMetadataType::MALODY;
                                std::string metadataScopeId =
                                    fmt::format("note_{}_{}",
                                                groupIdx,
                                                static_cast<int>(metaType));
                                applyNoteJsonEditResult(
                                    metadataScopeId, metaType, group.entities);
                                auto it =
                                    firstNc.m_metadata.note_properties.find(
                                        metaType);
                                static const decltype(firstNc.m_metadata
                                                          .note_properties)::
                                    mapped_type emptyMap;
                                const auto&     refProps =
                                    (it !=
                                     firstNc.m_metadata.note_properties.end())
                                        ? it->second
                                        : emptyMap;

                                ImGuiTableFlags tableFlags =
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_BordersOuter |
                                    ImGuiTableFlags_Resizable;

                                if ( ImGui::BeginTable(
                                         fmt::format("NoteMetaTable_MLD_{}",
                                                     groupIdx)
                                             .c_str(),
                                         3,
                                         tableFlags,
                                         ImVec2(0.0f, 0.0f)) ) {
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.key_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        220.0f * dpiScale);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.value_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.action_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        130.0f * dpiScale);
                                    ImGui::TableHeadersRow();

                                    std::vector<std::string> keys;
                                    for ( const auto& [k, v] : refProps ) {
                                        keys.push_back(k);
                                    }
                                    std::sort(keys.begin(), keys.end());

                                    for ( const auto& key : keys ) {
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();
                                        ImGui::AlignTextToFramePadding();
                                        ImGui::TextUnformatted(key.c_str());

                                        ImGui::TableNextColumn();
                                        char        valBuf[1024] = { 0 };
                                        std::string currentVal =
                                            refProps.at(key);
                                        size_t copyLen =
                                            std::min(currentVal.size(),
                                                     sizeof(valBuf) - 1);
                                        std::copy(currentVal.begin(),
                                                  currentVal.begin() + copyLen,
                                                  valBuf);

                                        ImGui::SetNextItemWidth(-1.0f);
                                        if ( ImGui::InputText(
                                                 fmt::format("##nm_mld_{}_{}",
                                                             groupIdx,
                                                             key)
                                                     .c_str(),
                                                 valBuf,
                                                 sizeof(valBuf)) ) {
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                nc.m_metadata
                                                    .note_properties[metaType]
                                                                    [key] =
                                                    valBuf;
                                            }
                                        }

                                        ImGui::TableNextColumn();
                                        renderMetadataJsonButton(
                                            metadataScopeId,
                                            key,
                                            refProps.at(key),
                                            fmt::format("note_mld_{}_field",
                                                        groupIdx));
                                        ImGui::SameLine();
                                        if ( ImGui::Button(
                                                 fmt::format(
                                                     "{}##clr_mld_{}_{}",
                                                     TR("ui.edit.note_"
                                                        "metadata.clear_"
                                                        "btn")
                                                         .data(),
                                                     groupIdx,
                                                     key)
                                                     .c_str()) ) {
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                auto& m = nc.m_metadata
                                                              .note_properties
                                                                  [metaType];
                                                m.erase(key);
                                                if ( m.empty() ) {
                                                    nc.m_metadata
                                                        .note_properties.erase(
                                                            metaType);
                                                }
                                            }
                                        }
                                    }
                                    ImGui::EndTable();
                                }

                                // 新增字段
                                ImGui::Separator();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_key").data());
                                ImGui::SameLine();
                                std::string bufKey =
                                    fmt::format("{}_{}",
                                                groupIdx,
                                                static_cast<int>(metaType));
                                auto& buf = inputBuffers[bufKey];
                                ImGui::SetNextItemWidth(150.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmk_mld_{}", groupIdx)
                                        .c_str(),
                                    buf.key,
                                    sizeof(buf.key));
                                ImGui::SameLine();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_value")
                                        .data());
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(200.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmv_mld_{}", groupIdx)
                                        .c_str(),
                                    buf.val,
                                    sizeof(buf.val));
                                ImGui::SameLine();
                                if ( ImGui::Button(
                                         fmt::format("{}##nma_mld_{}",
                                                     TR("ui.edit.note_metadata."
                                                        "add_btn")
                                                         .data(),
                                                     groupIdx)
                                             .c_str()) ) {
                                    std::string nk = buf.key;
                                    if ( !nk.empty() ) {
                                        for ( auto e : group.entities ) {
                                            auto& nc =
                                                registry
                                                    .get<Logic::NoteComponent>(
                                                        e);
                                            nc.m_metadata
                                                .note_properties[metaType][nk] =
                                                buf.val;
                                        }
                                        buf.key[0] = '\0';
                                        buf.val[0] = '\0';
                                    }
                                }
                                ImGui::SameLine();
                                renderNewMetadataJsonButton(
                                    metadataScopeId,
                                    buf.key,
                                    buf.val,
                                    fmt::format("note_mld_{}_new_field",
                                                groupIdx));

                                ImGui::EndTabItem();
                            }

                            // --- RM 标签页 ---
                            if ( ImGui::BeginTabItem("RM") ) {
                                auto metaType = ::MMM::NoteMetadataType::RM;
                                auto it =
                                    firstNc.m_metadata.note_properties.find(
                                        metaType);
                                static const decltype(firstNc.m_metadata
                                                          .note_properties)::
                                    mapped_type emptyMap;
                                const auto&     refProps =
                                    (it !=
                                     firstNc.m_metadata.note_properties.end())
                                        ? it->second
                                        : emptyMap;

                                ImGuiTableFlags tableFlags =
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_BordersOuter |
                                    ImGuiTableFlags_Resizable;

                                if ( ImGui::BeginTable(
                                         fmt::format("NoteMetaTable_RM_{}",
                                                     groupIdx)
                                             .c_str(),
                                         3,
                                         tableFlags,
                                         ImVec2(0.0f, 0.0f)) ) {
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.key_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        220.0f * dpiScale);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.value_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.action_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        60.0f * dpiScale);
                                    ImGui::TableHeadersRow();

                                    const std::string key = "Parameter";
                                    const bool hasKey = refProps.contains(key);
                                    int32_t    value  = 0;
                                    if ( hasKey ) {
                                        value =
                                            parseInt32Metadata(refProps.at(key))
                                                .value_or(0);
                                    }

                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::AlignTextToFramePadding();
                                    if ( !hasKey ) {
                                        ImGui::PushStyleColor(
                                            ImGuiCol_Text,
                                            ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                    }
                                    ImGui::TextUnformatted(key.c_str());
                                    if ( !hasKey ) {
                                        ImGui::PopStyleColor();
                                    }
                                    if ( ImGui::IsItemHovered() ) {
                                        ImGui::SetTooltip(
                                            "%s",
                                            "RM/IMD 行参数，二进制 int32。");
                                    }

                                    ImGui::TableNextColumn();
                                    int valueInput = value;
                                    ImGui::SetNextItemWidth(-1.0f);
                                    if ( ImGui::InputInt(
                                             fmt::format(
                                                 "##nm_rm_{}_{}", groupIdx, key)
                                                 .c_str(),
                                             &valueInput,
                                             1,
                                             100) ) {
                                        for ( auto e : group.entities ) {
                                            auto& nc =
                                                registry
                                                    .get<Logic::NoteComponent>(
                                                        e);
                                            nc.m_metadata
                                                .note_properties[metaType]
                                                                [key] =
                                                std::to_string(
                                                    static_cast<int32_t>(
                                                        valueInput));
                                        }
                                    }
                                    if ( hasKey &&
                                         !parseInt32Metadata(refProps.at(key))
                                              .has_value() ) {
                                        ImGui::TextColored(
                                            ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                            "%s",
                                            "当前值不是合法 "
                                            "int32，编辑后会修正。");
                                    }

                                    ImGui::TableNextColumn();
                                    if ( hasKey ) {
                                        if ( ImGui::Button(
                                                 fmt::format("{}##clr_rm_{}",
                                                             TR("ui.edit."
                                                                "note_metadata."
                                                                "clear_btn")
                                                                 .data(),
                                                             groupIdx)
                                                     .c_str()) ) {
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                auto& m = nc.m_metadata
                                                              .note_properties
                                                                  [metaType];
                                                m.erase(key);
                                                if ( m.empty() ) {
                                                    nc.m_metadata
                                                        .note_properties.erase(
                                                            metaType);
                                                }
                                            }
                                        }
                                    } else {
                                        ImGui::TextDisabled("-");
                                    }
                                    ImGui::EndTable();
                                }

                                ImGui::EndTabItem();
                            }

                            ImGui::EndTabBar();
                        }
                    }

                    ImGui::PopID();

                    if ( groupIdx < static_cast<int>(groups.size()) ) {
                        ImGui::Spacing();
                    }
                }

                ImGui::EndChild();
            }
        }
    }
    if ( opened ) {
        renderMetadataJsonEditorPopup(dpiScale);
    }
    ImGui::End();

    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
