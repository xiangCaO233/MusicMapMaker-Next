#include "ui/plugin/ToolPluginView.h"

// 工具插件的调用边界分成三层：
// 1. 加载期执行入口 Lua，校验清单并构造首份控件快照；
// 2. 用户动作期调用 Lua 回调，由显式宿主 API 执行文件、音频和谱面操作；
// 3. 普通 UI 帧只遍历 C++ 控件快照，不调用 Lua 或访问文件系统。
// 这个分层让脚本可以描述窗口，同时避免每帧跨语言分配与不可控 I/O。
// 每份脚本有独立虚拟机，插件之间不共享全局变量或对象所有权。
// 资源图片只在渲染器通知的资源准备阶段上传到 GPU。
// 音频导出由文件线程池执行，工作线程只持有纯 C++ 任务状态。
// 工作线程永远不借用 Lua 栈、ImGui 上下文、窗口或插件对象。
// 谱面转换使用 BeatMap 读写器；文本预览来自真实序列化结果。
// 用户对文本预览的修改直接保存，避免二次序列化覆盖现场编辑。
// IMD 是二进制格式，仅提供明确的结构化参数调整入口。
// 文件对话框按插件 ID 和用途键记录最近目录，历史写入只在选择成功时发生。
// 所有路径转换仅在加载、用户动作或资源阶段执行，不放入 UI 热路径。
// 窗口标题缓存并带稳定 ### ID，脚本改显示名称时仍保持停靠身份。
// 旧纹理延迟到视图销毁，防止在途命令仍引用原 descriptor。

#include "BuiltinToolPlugins.h"

#include "audio/AudioSpeedExportService.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "graphic/imguivk/VKTexture.h"
#include "mmm/beatmap/BeatMap.h"
#include "runtime/AppThreadPool.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"

#include <ice/manage/dec/MediaInfo.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <stb_image.h>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{

/// @brief 单个声明式控件的绘制快照。
/// @details Lua 对象不进入逐帧路径；输入缓冲在控件树重建时一次性分配。
struct Widget {
    // 这个结构只含 C++ 值，Lua VM 卸载前可以安全替换或销毁控件树。
    // 类型白名单在 rebuild 中统一校验，绘制代码不猜测未知类型。
    /// @brief 控件类型，未知类型会在解析时被拒绝。
    std::string type;
    /// @brief 插件内稳定控件 ID。
    std::string id;
    /// @brief 用户可见的提示文本。
    std::string label;
    /// @brief 当前文本值或复选框布尔值的字符串形式。
    std::string value;
    /// @brief 组合框可选项。
    std::vector<std::string> choices;
};

/// @brief 将 Lua 表中的字符串字段复制为 C++ 值。
/// @param table 插件声明表。
/// @param key 字段名。
/// @return 缺失或类型错误时为空字符串。
std::string tableString(const sol::table& table, const char* key)
{
    // Lua 字段缺失或类型错误时不执行隐式字符串转换。
    // 让清单校验能够区分真正提供的文本和错误声明。
    const sol::object value = table[key];
    return value.is<std::string>() ? value.as<std::string>() : std::string{};
}

/// @brief 限制插件 ID 为可直接用作状态文件名的字符集合。
/// @param id 待检查标识。
/// @return 非空且仅有 ASCII 字母、数字、点、下划线和连字符时为 true。
bool validPluginId(std::string_view id)
{
    // ID 同时进入 ImGui 内部名称和配置文件名，必须限制为安全 ASCII。
    // 长度上界避免极端字符串占用管理列表及状态路径。
    if ( id.empty() || id.size() > 128 ) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        // 点只允许作为名字空间分隔字符，不具备目录穿越能力。
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

/// @brief 判断路径后缀是否属于当前支持的四种谱面格式。
/// @param extension 小写扩展名，包含前导点。
/// @return 格式由统一 BeatMap 读写器支持时为 true。
bool supportedMapExtension(std::string_view extension)
{
    // 四种扩展名与当前统一 BeatMap 读写器的实际格式入口一致。
    // 比较精确后缀，不根据文件内容猜测输出格式。
    return extension == ".mmm" || extension == ".mc" || extension == ".osu" ||
           extension == ".imd";
}

/// @brief 释放独立的 FFmpeg 探测上下文。
struct AudioProbeContextDeleter {
    /// @brief 关闭输入并释放容器与流元信息。
    void operator()(AVFormatContext* context) const
    {
        // avformat_close_input 需要指针地址；局部副本避免泄漏到调用方。
        avformat_close_input(&context);
    }
};

/// @brief 将 FFmpeg 字典逐项复制到 Lua 数组，保留同名标签与原始顺序。
/// @param lua 当前工具插件的独立虚拟机。
/// @param metadata 容器或流所有的标签字典。
/// @return 每个元素含 key 和 value 的 Lua 数组。
/// @warning 文件探测动作路径：会复制元数据，不得从 UI 每帧调用。
sol::table copyAudioTags(sol::state& lua, const AVDictionary* metadata)
{
    sol::table tags  = lua.create_table();
    int        index = 1;
    // 字典可能含重复键，不能转换为 Lua 哈希表后丢掉后面的值。
    // 每个条目复制为独立 Lua 表，关闭输入后不会借用 FFmpeg 字符指针。
    // 容器和流使用相同规则，空字典自然得到空数组。
    for ( const AVDictionaryEntry* entry = av_dict_iterate(metadata, nullptr);
          entry;
          entry = av_dict_iterate(metadata, entry) ) {
        sol::table tag = lua.create_table();
        tag["key"]     = entry->key;
        tag["value"]   = entry->value;
        tags[index++]  = std::move(tag);
    }
    return tags;
}

/// @brief 补充容器及所有流公开的元信息，不改变 ICE 的预编译 ABI。
/// @param lua 当前工具插件的独立虚拟机。
/// @param path 已成功通过 ICE 探测的输入文件 UTF-8 路径。
/// @param output 待填充的音频探测结果表。
/// @return FFmpeg 能读取完整流信息时为 true。
/// @warning 用户选择文件后的低频路径：再次打开媒体，不得在 build 中调用。
bool appendAudioDetails(sol::state& lua, const std::string& path,
                        sol::table& output)
{
    // ICE 已完成基础探测；这里仅补充其公开结构没有容纳的标签与流。
    // 不改 MediaInfo 布局，保持尚未更新的 macOS ICE 库可用。
    AVFormatContext* raw = nullptr;
    if ( avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0 )
        return false;
    std::unique_ptr<AVFormatContext, AudioProbeContextDeleter> context(raw);
    if ( avformat_find_stream_info(context.get(), nullptr) < 0 ) return false;

    // 容器时长与 ICE 估计帧数分别展示，未知时长以零表示，不伪装成精确值。
    // 两个时长来自不同时间基，保留两者以便识别容器声明偏差。
    const auto* format  = context->iformat;
    output["container"] = format && format->name ? format->name : "";
    output["container_long_name"] =
        format && format->long_name ? format->long_name : "";
    output["container_duration"] =
        context->duration == AV_NOPTS_VALUE
            ? 0.0
            : static_cast<double>(context->duration) / AV_TIME_BASE;
    output["format_tags"] = copyAudioTags(lua, context->metadata);

    sol::table streams = lua.create_table();
    // 不只报告第一条音轨：图片、视频及附带的第二音轨也属于文件可读取信息。
    // Lua 数组从 1 开始，item.index 仍保留 FFmpeg 的零起始流编号。
    for ( unsigned int index = 0; index < context->nb_streams; ++index ) {
        const AVStream* stream = context->streams[index];
        if ( !stream || !stream->codecpar ) continue;
        const AVCodecParameters* codec = stream->codecpar;
        sol::table               item  = lua.create_table();
        const char* type    = av_get_media_type_string(codec->codec_type);
        item["index"]       = static_cast<int>(index);
        item["type"]        = type ? type : "unknown";
        item["codec"]       = avcodec_get_name(codec->codec_id);
        item["bitrate"]     = codec->bit_rate > 0 ? codec->bit_rate : 0;
        item["sample_rate"] = codec->sample_rate;
        item["channels"]    = codec->ch_layout.nb_channels;
        item["duration"] =
            stream->duration == AV_NOPTS_VALUE || stream->time_base.den == 0
                ? 0.0
                : static_cast<double>(stream->duration) *
                      stream->time_base.num / stream->time_base.den;
        item["attached_picture"] =
            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        // 附图标志只描述流；封面像素依旧由 ICE 的首张有效附件读取。
        item["tags"] = copyAudioTags(lua, stream->metadata);
        streams[static_cast<int>(index + 1)] = std::move(item);
    }
    output["streams"] = std::move(streams);
    return true;
}

/// @brief 为格式专属元数据生成稳定排序的可读 JSON 文本。
/// @param map 已加载的谱面。
/// @return 各来源属性与物件计数的详细文本。
std::string describeMapDetails(const BeatMap& map)
{
    // 详细信息只在用户载入谱面时构造，不进入逐帧窗口绘制。
    // JSON 对象用稳定键名，便于插件直接展示，也便于人工比较转换前后内容。
    // 不把 BeatMap 的指针或引用放入 Lua，防止重载后持有悬空对象。
    nlohmann::json details = nlohmann::json::object();
    const auto&    notes   = map.m_noteData;
    // 四类玩家物件分开统计；折线节点不另行计入普通 Note 数量。
    // Timing 与音频事件保持独立计数，转换时更容易发现不支持的对象类型。
    details["counts"] = {
        { "notes", notes.notes.size() },
        { "holds", notes.holds.size() },
        { "flicks", notes.flicks.size() },
        { "polylines", notes.polylines.size() },
        { "timings", map.m_timings.size() },
        { "audio_samples", map.m_audioSamples.size() },
        { "annotations", map.m_annotations.size() },
    };
    // nlohmann::json 对象键有序，详细视图与重新加载后的比较保持稳定。
    auto& properties = details["source_properties"];
    properties       = nlohmann::json::object();
    // 来源属性按格式命名空间隔离，同名字段不能互相覆盖。
    // Malody 的 mc 与原生 mmm 会共享通用模型，但格式专属值仍保留来源。
    for ( const auto& [source, values] : map.m_metadata.map_properties ) {
        const char* name = source == MapMetadataType::OSU      ? "osu"
                           : source == MapMetadataType::MALODY ? "malody"
                                                               : "imd";
        for ( const auto& [key, value] : values ) {
            // 保留字符串原文，让用户能看到非数值及写出器未知的属性。
            properties[name][key] = value;
        }
    }
    details["resource_paths"] = {
        // 这些路径只作展示；预览转换不去重定位外部资源。
        { "song_file_hint",
          Config::pathToUtf8(map.m_baseMapMetadata.song_file_hint) },
        { "main_audio_path",
          Config::pathToUtf8(map.m_baseMapMetadata.main_audio_path) },
        { "cover_path", Config::pathToUtf8(map.m_baseMapMetadata.cover_path) },
        { "main_cover_path",
          Config::pathToUtf8(map.m_baseMapMetadata.main_cover_path) },
    };
    // 视频背景及偏移不属于通用文本字段，但转换前需要可见以判断目标格式损失。
    details["background"] = {
        // 视频起始时间与背景偏移并非所有目标格式都能表达。
        // 先暴露原值，用户可以依据最终文本判断是否出现信息损失。
        { "type",
          map.m_baseMapMetadata.cover_type == CoverType::VIDEO ? "video"
                                                               : "image" },
        { "video_start_time", map.m_baseMapMetadata.video_starttime },
        { "offset_x", map.m_baseMapMetadata.bgxoffset },
        { "offset_y", map.m_baseMapMetadata.bgyoffset },
    };
    // 逐条显示变速、跳跃等时间线效果及其来源属性，数量与顺序保留原谱面语义。
    auto& timings = details["timing_points"];
    timings       = nlohmann::json::array();
    for ( const auto& timing : map.m_timings ) {
        // 保留原有顺序，避免重新排序改变相同时间点的效果优先级。
        // BPM、拍长和扩展效果参数同时展示，便于核对变速谱面。
        nlohmann::json item = {
            { "time", timing.m_timestamp },
            { "effect", timingEffectToString(timing.m_timingEffect) },
            { "bpm", timing.m_bpm },
            { "beat_length", timing.m_beat_length },
            { "parameter", timing.m_timingEffectParameter },
        };
        for ( const auto& [source, values] :
              timing.m_metadata.timing_properties ) {
            // Timing 的来源字段可能记录 osu! 继承时间点或 Malody 扩展值。
            const char* name = source == TimingMetadataType::OSU  ? "osu"
                               : source == TimingMetadataType::RM ? "imd"
                                                                  : "malody";
            for ( const auto& [key, value] : values ) {
                // 只复制原文，不在展示路径做有损数值转换。
                item["source_properties"][name][key] = value;
            }
        }
        timings.push_back(std::move(item));
    }
    // 普通物件及派生物件只在含有来源属性时列入，避免重复打印全部基础几何。
    auto& noteProperties            = details["note_source_properties"];
    noteProperties                  = nlohmann::json::array();
    const auto appendNoteProperties = [&noteProperties](const auto& container,
                                                        const char* type) {
        for ( const auto& note : container ) {
            // 无来源属性的常规对象由上方计数表示，减少冗余文本。
            if ( note.m_metadata.note_properties.empty() ) continue;
            // 时间和轨道用于定位特殊属性所属的原物件。
            // 不把协作 ID 当作转换稳定身份，外部格式未必保存它。
            nlohmann::json item = {
                { "type", type },
                { "time", note.m_timestamp },
                { "track", note.m_track },
            };
            for ( const auto& [source, values] :
                  note.m_metadata.note_properties ) {
                // 来源枚举转为固定字符串，便于下游 Lua 文本查看。
                const char* name = source == NoteMetadataType::OSU ? "osu"
                                   : source == NoteMetadataType::MALODY
                                       ? "malody"
                                   : source == NoteMetadataType::RM ? "imd"
                                                                    : "mmm";
                for ( const auto& [key, value] : values ) {
                    // 完整保留原始键值；未知属性也有机会由写出器继续传递。
                    item["source_properties"][name][key] = value;
                }
            }
            noteProperties.push_back(std::move(item));
        }
    };
    appendNoteProperties(notes.notes, "note");
    appendNoteProperties(notes.holds, "hold");
    appendNoteProperties(notes.flicks, "flick");
    appendNoteProperties(notes.polylines, "polyline");
    // 解析兼容提示可能解释为什么输入与输出计数不同，也与原始文件关联。
    auto& diagnostics = details["load_diagnostics"];
    diagnostics       = nlohmann::json::array();
    for ( const auto& diagnostic : map.m_loadDiagnostics ) {
        // 加载兼容诊断用于解释输入模型与原文件可能存在的迁移差异。
        // 关联路径明确指出可重新导入的原文件，不自动执行额外读取。
        diagnostics.push_back({
            { "severity", static_cast<int>(diagnostic.m_severity) },
            { "code", static_cast<int>(diagnostic.m_code) },
            { "message", diagnostic.m_message },
            { "related_path", Config::pathToUtf8(diagnostic.m_relatedPath) },
        });
    }
    return details.dump(2);
}

/// @brief 插件独立文件选择历史的持久化路径。
/// @param id 已验证的插件 ID。
/// @return 用户配置目录中的状态文件路径。
std::filesystem::path recentFilePath(std::string_view id)
{
    // ID 已按 ASCII 白名单校验，不能把目录分隔符注入状态文件名。
    // 文件位于配置根而非项目目录，避免工具插件污染谱面工程。
    return Config::AppPaths::configRootPath() / "plugin-state" /
           (std::string(id) + ".json");
}

/// @brief 读取单个插件的最近目录，不让损坏状态阻断插件加载。
/// @param id 已验证的插件 ID。
/// @return 用途键到目录的映射。
std::unordered_map<std::string, std::string> loadRecent(std::string_view id)
{
    // 最近目录属于便利状态，读取失败不应阻止插件窗口打开。
    // 只接受顶层 JSON 对象及字符串值，避免损坏配置触发脚本类型异常。
    // 插件 ID 决定独立文件，不允许一个工具读取另一工具的最近目录。
    std::ifstream input(recentFilePath(id), std::ios::binary);
    if ( !input ) return {};
    const auto document = nlohmann::json::parse(input, nullptr, false);
    if ( !document.is_object() ) return {};
    std::unordered_map<std::string, std::string> result;
    for ( auto it = document.begin(); it != document.end(); ++it ) {
        // 用途键由各插件自己定义；未知键可原样保留以支持脚本升级。
        if ( it.value().is_string() ) {
            result.emplace(it.key(), it.value().get<std::string>());
        }
    }
    return result;
}

/// @brief 只在用户完成一次文件选择后写出该插件的最近目录。
/// @param id 已验证的插件 ID。
/// @param recent 当前用途键到目录的映射。
/// @return 状态文件完整写出时为 true。
bool saveRecent(std::string_view                                    id,
                const std::unordered_map<std::string, std::string>& recent)
{
    // 对话框取消不会调用本函数；只在成功选择后写一次完整快照。
    // 使用 error_code 让不可写配置目录通过返回值降级，不抛出异常。
    // 当前实现写整份小型映射，目的不是热路径增量日志。
    const auto      path = recentFilePath(id);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if ( error ) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if ( !output ) return false;
    nlohmann::json document = nlohmann::json::object();
    for ( const auto& [key, directory] : recent ) {
        // 每个用途分开保存，音频输入与输出不会互相改变默认位置。
        document[key] = directory;
    }
    output << document.dump(2);
    return output.good();
}

/// @brief 依照 ImGui 官方 std::string 包装模式响应输入缓冲增长。
/// @param data 本次输入编辑的回调数据。
/// @return 始终为零，事件已在本回调处理。
int resizeInputText(ImGuiInputTextCallbackData* data)
{
    // ImGui 请求扩大缓冲时，先更新 string 大小再交回新的 data 指针。
    // 这个回调只由用户实际输入触发，不在静态显示帧分配内存。
    if ( data->EventFlag != ImGuiInputTextFlags_CallbackResize ) return 0;
    auto* value = static_cast<std::string*>(data->UserData);
    value->resize(static_cast<std::size_t>(data->BufTextLen));
    data->Buf = value->data();
    return 0;
}

/// @brief 绘制会随用户输入扩容的单行或多行编辑器。
/// @param label 控件显示标签。
/// @param value 由控件缓存独占的可变文本。
/// @param multiline 是否使用多行区域。
/// @return 用户本帧修改文本时为 true。
/// @warning UI 热路径：无编辑时不调整容量；扩容只发生在用户输入时。
bool inputTextDynamic(const char* label, std::string& value, bool multiline)
{
    // 两种控件共享同一 std::string 容量协议，避免固定字符数组截断谱面文本。
    // 文本内容由 Widget 独占，Lua 回调只收到动作发生时的值拷贝。
    const auto flags   = ImGuiInputTextFlags_CallbackResize;
    bool       changed = false;
    if ( multiline ) {
        // 固定初始高度让很长的预览仍留在可滚动区域内。
        changed = ImGui::InputTextMultiline(label,
                                            value.data(),
                                            value.capacity() + 1,
                                            ImVec2(-1.0F, 240.0F),
                                            flags,
                                            &resizeInputText,
                                            &value);
    } else {
        // 输入框只对用户编辑返回 true，静态帧不会构造动作字符串。
        changed = ImGui::InputText(label,
                                   value.data(),
                                   value.capacity() + 1,
                                   flags,
                                   &resizeInputText,
                                   &value);
    }
    // ImGui 在已有容量中直接编辑字符时不会触发 CallbackResize；动作值必须
    // 同步逻辑长度，否则 Lua 收到旧前缀，删除字符时还可能带上残留尾部。
    if ( changed ) value.resize(std::strlen(value.c_str()));
    return changed;
}

}  // namespace

/// @brief 所有 Lua 插件状态均留在 UI 线程；不将 Lua 对象交给工作线程。
struct ToolPluginView::Impl {
    // 所有 Lua 状态和窗口缓存都在 UI 线程拥有；工作线程只见下方 AudioTask。
    // 插件对象在每次显式重载时整体替换，不能从外部保留其地址。
    // TextureLoader 生命周期覆盖窗口使用的纹理资源。
    /// @brief 后台编码结果只在完成标志发布后由 UI 线程读取。
    struct AudioTask {
        // 单任务单写者，UI 线程只读者；不需要阻塞互斥锁。
        // result 是普通成员，只有 done 的 release/acquire 建立可见性后才能读。
        /// @brief 编码线程持续更新的轻量进度。
        /// @warning 工作线程写入、UI 每帧只读；进度不携带结果发布语义，
        /// 因此使用 relaxed，避免通过 Lua 重建控件来查询后台任务。
        std::atomic<float> progress{ 0.0F };
        /// @brief 工作完成标志，读取方使用 acquire。
        /// @warning 工作线程最后发布，UI 每帧只读；结果只在 acquire 观察
        /// 到 true 后访问，不能改成无同步读取普通 result 成员。
        std::atomic<bool> done{ false };
        /// @brief 工作线程独占写入的最终结果。
        Audio::AudioSpeedExportResult result;
    };

    /// @brief 已加载插件的可变运行状态。
    struct Plugin {
        // 定义顺序保证回调和 Lua 引用在 lua 析构前释放。
        // 这些字段只在加载和用户动作路径变化；普通帧只借用稳定引用。
        /// @brief 独立虚拟机，插件之间不共享全局变量。
        sol::state lua;
        /// @brief 构造声明式控件树的 Lua 函数。
        sol::protected_function build;
        /// @brief 用户动作回调，只在控件变更后执行。
        sol::protected_function onAction;
        /// @brief 向脚本提供低频宿主能力的表。
        sol::table api;
        /// @brief 稳定插件标识与窗口显示名称。
        ToolPluginInfo info;
        /// @brief 加载时构造的 ImGui 稳定窗口标题，避免逐帧字符串分配。
        std::string windowTitle;
        /// @brief C++ 逐帧渲染的控件缓存。
        std::vector<Widget> widgets;
        /// @brief 最近选择的目录，按输入、输出等用途隔离。
        std::unordered_map<std::string, std::string> recent;
        // 封面像素与 GPU 纹理分离：探测动作产生像素，资源阶段负责上传。
        // 新像素覆盖旧待办，只展示最近一次成功探测的封面。
        /// @brief 用户探测音频时解码的 RGBA 封面，等待资源阶段上传。
        std::vector<unsigned char> pendingCover;
        /// @brief 待上传封面的像素尺寸。
        int coverWidth{ 0 }, coverHeight{ 0 };
        /// @brief 当前封面的 Vulkan 纹理所有权。
        std::unique_ptr<Graphic::VKTexture> coverTexture;
        /// @brief 当前纹理是否对应最近一次音频探测，防止显示上一文件的封面。
        bool coverTextureCurrent{ false };
        /// @brief 离线任务由插件和工作线程共享，任务不借用 Lua 状态。
        std::shared_ptr<AudioTask> audioTask;
        // 即使窗口关闭，任务仍能安全完成；重载后工作线程保留任务自身所有权。
        /// @brief 每次完成仅重建一次控件树。
        bool audioCompletionShown{ false };
        /// @brief 从用户路径解析出的谱面，独立于当前编辑器项目。
        std::unique_ptr<BeatMap> beatmap;
        /// @brief 当前输入后缀，用于禁止输出相同格式。
        std::string inputMapExtension;
        // 路径与编辑器当前打开的工程相互独立，工具转换不会修改会话状态。
        /// @brief 文件选择器只允许从用户动作回调打开。
        bool actionActive{ false };
        /// @brief 窗口打开状态独立于插件视图本身。
        bool open{ false };
    };

    /// @brief 插件所有权向量，重载时整体替换。
    std::vector<std::unique_ptr<Plugin>> loaded;
    // 管理界面持有清单的短暂引用；重载只在用户动作后发生。
    /// @brief 管理界面借用的状态快照。
    std::vector<ToolPluginInfo> infos;
    /// @brief 已换下的纹理留到视图安全销毁，避免在途 GPU 命令引用失效。
    std::vector<std::unique_ptr<Graphic::VKTexture>> retiredTextures;
    // 该向量只在低频封面替换时增长，常态帧不会执行 GPU 销毁。

    /// @brief 将入口加载错误保留在工具列表，避免坏脚本静默消失。
    /// @param source 脚本文件或内置块名称。
    /// @param message 便于用户定位的原因。
    void recordLoadError(std::string source, std::string message)
    {
        // 错误来源可以是内置块名或配置目录绝对路径。
        // 仅管理列表保留该项，loaded 不新增对象，openPlugin 不会打开它。
        // 列表仍可显示错误，不要求用户查看终端才能发现语法问题。
        // 失败项不持有 Lua 状态，也不能打开窗口；路径作为列表内稳定行 ID。
        infos.push_back(ToolPluginInfo{
            source, std::move(source), std::move(message), false });
    }

    /// @brief 构造一份不含 Lua 对象的控件快照。
    /// @param plugin 待刷新插件。
    /// @return 声明合法且构造成功时为 true。
    /// @warning 用户动作或插件加载后的低频路径；允许 Lua 执行和分配。
    static bool rebuild(Plugin& plugin)
    {
        // 构建只发生在加载、用户动作或后台任务完成时，绝不逐帧执行脚本。
        // protected_function 将 Lua 错误作为结果返回，禁用 C++ 异常仍可诊断。
        // 失败时保留上一份完整控件树，避免窗口半更新。
        // 插件 build 只声明数据，真正的 ImGui Begin/End 由宿主掌控。
        // 这种限制防止脚本漏配对 UI 栈，并使绘制路径有确定的成本边界。
        sol::protected_function_result result = plugin.build(plugin.api);
        if ( !result.valid() ) {
            // 错误文字留在窗口顶部，插件作者可以修复后显式重载。
            const sol::error error = result;
            plugin.info.error      = error.what();
            return false;
        }
        sol::object object = result;
        if ( !object.is<sol::table>() ) {
            // 入口返回标量无法表示有序控件列表，拒绝替代旧快照。
            plugin.info.error = "build 必须返回控件数组";
            return false;
        }
        const sol::table    source = object.as<sol::table>();
        std::vector<Widget> candidate;
        // 先在局部向量中完成所有解析，最后一次性交换到可见缓存。
        // Lua 数组从 1 起始，按返回顺序绘制，不能按哈希遍历打乱布局。
        candidate.reserve(source.size());
        for ( std::size_t index = 1; index <= source.size(); ++index ) {
            // 本协议要求密集数组；缺失项视为错误，不在窗口里留下不可见空槽。
            const sol::object item = source[index];
            if ( !item.is<sol::table>() ) {
                // 错误项不能作为空控件吞掉，否则脚本布局缺损难以定位。
                plugin.info.error = "控件数组成员必须是表";
                return false;
            }
            const sol::table row = item.as<sol::table>();
            Widget           widget;
            widget.type  = tableString(row, "type");
            widget.id    = tableString(row, "id");
            widget.label = tableString(row, "label");
            widget.value = tableString(row, "value");
            // 控件类型做白名单检查，脚本不能注入任意 ImGui 调用。
            // ID 是窗口内部身份，空值会让多个元素共享输入焦点状态。
            // `audio_progress` 是唯一会在绘制阶段读取后台原子状态的控件。
            if ( widget.id.empty() ||
                 (widget.type != "text" && widget.type != "button" &&
                  widget.type != "input" && widget.type != "multiline" &&
                  widget.type != "checkbox" && widget.type != "combo" &&
                  widget.type != "separator" && widget.type != "image" &&
                  widget.type != "audio_progress") ) {
                plugin.info.error = "控件类型或 ID 无效";
                return false;
            }
            const sol::object choices = row["choices"];
            if ( choices.is<sol::table>() ) {
                // 下拉选项只接受数组中的字符串，忽略类型错误的单项。
                // 选项顺序影响用户选择索引，因此保留脚本数组顺序。
                // 选择后返回字符串值，脚本无需维护与 C++ 相同的数字索引。
                const sol::table options = choices.as<sol::table>();
                for ( std::size_t choice = 1; choice <= options.size();
                      ++choice ) {
                    const sol::object value = options[choice];
                    if ( value.is<std::string>() ) {
                        // 只复制值，不在 C++ 快照中留 Lua 对象引用。
                        widget.choices.push_back(value.as<std::string>());
                    }
                }
            }
            // 为常见短编辑留少量余量，长文本只在用户输入时按需增长。
            if ( widget.type == "input" || widget.type == "multiline" )
                widget.value.reserve(widget.value.size() + 64);
            // 每个候选值为独立所有权，swap 后普通帧不碰 Lua 分配器。
            candidate.push_back(std::move(widget));
        }
        // 一次交换建立完整的可见控件树；旧树在动作路径销毁。
        // 成功重建后清除上一次 Lua 错误，但不会清除插件自己的状态文本。
        plugin.widgets.swap(candidate);
        // 老控件值随局部 candidate 离开作用域释放，不延迟到下一帧。
        plugin.info.error.clear();
        return true;
    }

    /// @brief 为一个插件建立文件选择 API 与作用域独立的状态。
    /// @param plugin 插件运行实例。
    /// @warning 文件选择器只在用户动作回调中允许调用。
    static void bindApi(Plugin& plugin)
    {
        // 表只属于此插件虚拟机，绑定回调借用 Plugin 生命周期内的稳定地址。
        // 所有回调在 UI 线程、用户动作期间运行，后台线程不调用这些函数。
        // 每个文件或模型操作再次检查 actionActive，阻止 build 阶段滥用。
        // 返回的错误使用字符串或含 error 的表，脚本无需依赖 C++ 异常。
        plugin.api = plugin.lua.create_table();
        plugin.api.set_function(
            "beatmap_read", [&plugin](const std::string& path) {
                // 读取是一项显式动作，避免恶意 build 在每次控件重建时扫描磁盘。
                // 输出表只包含值语义数据，BeatMap 留在插件实例中供后续写出。
                sol::table output = plugin.lua.create_table();
                if ( !plugin.actionActive ) {
                    // 失败也返回结构化 error，Lua 不需要捕获异常。
                    // 这同时阻止 build 利用该 API 做重复磁盘读取。
                    output["error"] = "仅可在用户动作后读取谱面";
                    return output;
                }
                const auto        input = Config::utf8ToPath(path);
                const std::string extension =
                    Config::pathToUtf8(input.extension());
                // 当前转换链只支持四种完整读写格式，扩展名用于选择解析器。
                // 与输出格式是否可表达某对象是不同检查，后者交给写出器。
                if ( !supportedMapExtension(extension) ) {
                    output["error"] = "仅支持 .mmm、.mc、.osu 和 .imd";
                    return output;
                }
                std::error_code error;
                // 路径检查使用 error_code，损坏链接或权限问题表现为读取失败。
                if ( !std::filesystem::is_regular_file(input, error) ||
                     error ) {
                    // 输入缺失时不改变之前成功读到的模型。
                    output["error"] = "输入谱面文件不存在";
                    return output;
                }
                auto map =
                    std::make_unique<BeatMap>(BeatMap::loadFromFile(input));
                // 解析器以空 map_path 表达失败；不能把空模型当成零物件谱面。
                if ( map->m_baseMapMetadata.map_path.empty() ) {
                    output["error"] = "谱面解析失败";
                    return output;
                }
                map->sync();
                // sync 仅在低频加载后建立物件引用表，绝不放到普通 UI 帧。
                // 基础字段与来源详情分开，脚本可以先展示摘要再按需展开详情。
                const auto& meta          = map->m_baseMapMetadata;
                output["format"]          = extension;
                output["name"]            = meta.name;
                output["title"]           = meta.title;
                output["title_unicode"]   = meta.title_unicode;
                output["artist"]          = meta.artist;
                output["artist_unicode"]  = meta.artist_unicode;
                output["album"]           = meta.album;
                output["author"]          = meta.author;
                output["version"]         = meta.version;
                output["track_count"]     = meta.track_count;
                output["bgm_track_count"] = meta.bgm_track_count;
                output["bpm"]             = meta.preference_bpm;
                output["map_length"]      = meta.map_length;
                output["details"]         = describeMapDetails(*map);
                // 只有成功构造全部输出后才替换旧模型；失败保留上一次会话。
                plugin.inputMapExtension = extension;
                plugin.beatmap           = std::move(map);
                return output;
            });
        plugin.api.set_function(
            "beatmap_set_field",
            [&plugin](const std::string& key, const std::string& value) {
                // 通用字段采用白名单，防止脚本把任意键误写进内部结构。
                // 这个入口只改插件副本，不改当前编辑器工程或输入文件。
                if ( !plugin.actionActive || !plugin.beatmap ) {
                    // 清单入口阶段不应修改谱面，必须先有用户选择的输入。
                    return std::string("请先读取谱面");
                }
                auto& meta = plugin.beatmap->m_baseMapMetadata;
                if ( key == "name" )
                    meta.name = value;
                else if ( key == "title" )
                    meta.title = value;
                else if ( key == "title_unicode" )
                    meta.title_unicode = value;
                else if ( key == "artist" )
                    meta.artist = value;
                else if ( key == "artist_unicode" )
                    meta.artist_unicode = value;
                else if ( key == "album" )
                    meta.album = value;
                else if ( key == "author" )
                    meta.author = value;
                else if ( key == "version" )
                    meta.version = value;
                else if ( key == "map_length" || key == "bpm" ) {
                    // 数值必须被完整消费，拒绝 NaN、无穷和尾随乱码。
                    // BPM 需要严格正数；长度允许零代表尚未给出有效时长。
                    double number           = 0.0;
                    const auto [end, error] = std::from_chars(
                        value.data(), value.data() + value.size(), number);
                    if ( error != std::errc{} ||
                         end != value.data() + value.size() ||
                         !std::isfinite(number) || number < 0.0 ||
                         (key == "bpm" && number == 0.0) ) {
                        return std::string("数值格式无效");
                    }
                    if ( key == "map_length" )
                        meta.map_length = number;
                    else
                        meta.preference_bpm = number;
                } else {
                    // 拒绝未知字段使脚本拼写错误不会静默产生无效结果。
                    return std::string("未知元数据字段");
                }
                return std::string{};
            });
        plugin.api.set_function(
            "beatmap_set_property",
            [&plugin](const std::string& source,
                      const std::string& key,
                      const std::string& value) {
                // 来源属性并非通用字段，写出器可以按来源决定是否映射。
                // 所有值以原文保存，不在这里猜测目标格式的类型语义。
                if ( !plugin.actionActive || !plugin.beatmap || key.empty() ) {
                    return std::string("请先读取谱面并提供属性名");
                }
                MapMetadataType type;
                // 外部名称映射到三种内部来源枚举，未知来源无法安全保存。
                if ( source == "osu" )
                    type = MapMetadataType::OSU;
                else if ( source == "malody" )
                    type = MapMetadataType::MALODY;
                else if ( source == "imd" )
                    type = MapMetadataType::RM;
                else
                    return std::string("未知元数据来源");
                if ( source == "imd" && key == "mapLength" ) {
                    // IMD 写出器要求声明长度为 int32 毫秒数，提前验证避免
                    // 先写入非法字符串再由旧转换函数悄悄使用默认值。
                    // 其余来源属性仍作为原文保存，由目标写出器处理。
                    std::int32_t length     = 0;
                    const auto [end, error] = std::from_chars(
                        value.data(), value.data() + value.size(), length);
                    if ( error != std::errc{} ||
                         end != value.data() + value.size() || length < 0 ) {
                        return std::string(
                            "IMD 声明长度必须是非负 int32 毫秒数");
                    }
                }
                plugin.beatmap->m_metadata.map_properties[type][key] = value;
                // 属性修改留在插件模型中；真正写盘由 save 单独触发。
                return std::string{};
            });
        plugin.api.set_function(
            "beatmap_set_imd_first_bpm", [&plugin](double bpm) {
                // 首 BPM 是 Timing 点而不只是摘要字段，二者需要同步更新。
                // 如果原谱面没有 BPM 点，不能凭空猜测位置添加一个。
                if ( !plugin.actionActive || !plugin.beatmap ||
                     !std::isfinite(bpm) || bpm <= 0.0 ) {
                    return std::string("请先读取谱面并填写正数 BPM");
                }
                for ( auto& timing : plugin.beatmap->m_timings ) {
                    // 按原顺序只修改第一个 BPM；后续速度变化原样保留。
                    if ( timing.m_timingEffect == TimingEffect::BPM ) {
                        timing.m_bpm                                     = bpm;
                        plugin.beatmap->m_baseMapMetadata.preference_bpm = bpm;
                        return std::string{};
                    }
                }
                return std::string("谱面没有可调整的 BPM 点");
            });
        plugin.api.set_function(
            "beatmap_set_imd_note_parameter",
            [&plugin](const std::string& value) {
                // IMD Parameter 由写出器读取 int32 原文，必须先验证完整范围。
                // 这里只处理普通 Note；Hold、Flick 和折线保持各自元数据。
                if ( !plugin.actionActive || !plugin.beatmap ) {
                    return std::string("请先读取谱面");
                }
                std::int32_t number     = 0;
                const auto [end, error] = std::from_chars(
                    value.data(), value.data() + value.size(), number);
                if ( error != std::errc{} ||
                     end != value.data() + value.size() ) {
                    return std::string("普通 Note 参数必须是 int32");
                }
                for ( auto& note : plugin.beatmap->m_noteData.notes ) {
                    // 为所有普通物件设置相同来源属性，不调整其时间或轨道。
                    note.m_metadata
                        .note_properties[NoteMetadataType::RM]["Parameter"] =
                        value;
                }
                return std::string{};
            });
        plugin.api.set_function(
            "beatmap_preview", [&plugin](const std::string& outputPath) {
                // 预览必须使用真实 saveToFile
                // 结果，不能从输入格式拼接近似文本。
                // 只有目标后缀决定写出器；不会在此阶段改写用户最终路径。
                // 先以临时文件让同一写出路径运行格式特有的校验和文本生成。
                // Lua 收到的是最终内容副本，不会持有临时文件流。
                sol::table output = plugin.lua.create_table();
                if ( !plugin.actionActive || !plugin.beatmap ) {
                    output["error"] = "请先读取谱面";
                    return output;
                }
                const auto target    = Config::utf8ToPath(outputPath);
                const auto extension = Config::pathToUtf8(target.extension());
                if ( !supportedMapExtension(extension) ||
                     extension == plugin.inputMapExtension ) {
                    // 转换工具只允许另外三种格式，避免误当作原格式重保存。
                    output["error"] = "输出格式必须是另外三种格式之一";
                    return output;
                }
                if ( extension == ".imd" ) {
                    // IMD 二进制没有可编辑文本，脚本通过专用参数入口修改模型。
                    // binary 标志只表明预览不可编辑，不执行正式保存。
                    output["binary"] = true;
                    return output;
                }
                // 文本预览由真正的写出器产生；临时目录隔离用户目标文件。
                const auto previewDir = Config::AppPaths::configRootPath() /
                                        "plugin-state" / "preview" /
                                        plugin.info.id;
                // 每个插件拥有独立预览目录，名称来自目标文件而非输入文件。
                // 创建失败不降级为用户目标路径，防止预览意外覆盖正式输出。
                std::error_code error;
                std::filesystem::create_directories(previewDir, error);
                if ( error ) {
                    output["error"] = "无法建立预览目录";
                    return output;
                }
                const auto previewPath = previewDir / target.filename();
                // 写出器执行目标格式的可表达性检查，失败即返回给用户处理。
                // 目标文件名保留后缀，写出器才能选择正确格式。
                if ( !plugin.beatmap->saveToFile(previewPath) ) {
                    // 写出器失败后可能留下部分临时文件；清掉它以免下次读到残片。
                    std::filesystem::remove(previewPath, error);
                    // 用户目标文件没有被创建，失败可安全修改参数后重试。
                    output["error"] =
                        "目标格式写出失败，可能包含无法表达的数据";
                    return output;
                }
                std::ifstream input(previewPath, std::ios::binary);
                // 打开失败仍尝试移除临时结果，避免残留文件被后续操作误认。
                if ( !input ) {
                    std::filesystem::remove(previewPath, error);
                    output["error"] = "无法读取预览文本";
                    return output;
                }
                std::string text((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
                // 读回后立即清理临时文件；脚本只接收独立拥有的字符串。
                std::filesystem::remove(previewPath, error);
                // 删除错误不影响已经读到的文本；下一次预览会覆盖同名临时文件。
                if ( !input.eof() && input.bad() ) {
                    // 流错误不能把不完整的预览标记为可保存文本。
                    output["error"] = "读取预览文本失败";
                    return output;
                }
                output["text"] = std::move(text);
                // 返回的文本正是保存时应使用的原文，脚本可以现场编辑。
                return output;
            });
        plugin.api.set_function(
            "beatmap_save",
            [&plugin](const std::string& outputPath, const std::string& text) {
                // 保存与预览重复校验输入模型和目标格式，不依赖脚本按钮状态。
                // 写出是用户明确触发的低频操作，允许在此路径访问磁盘。
                // 当前接口故意不提供任意路径的通用文件写入函数。
                // 插件只能保存受支持的谱面后缀或走音频导出服务。
                if ( !plugin.actionActive || !plugin.beatmap ) {
                    return std::string("请先读取谱面");
                }
                const auto target    = Config::utf8ToPath(outputPath);
                const auto extension = Config::pathToUtf8(target.extension());
                if ( !supportedMapExtension(extension) ||
                     extension == plugin.inputMapExtension ) {
                    return std::string("输出格式必须是另外三种格式之一");
                }
                if ( extension == ".imd" ) {
                    // 二进制只通过 BeatMap 写出器生成，任意文本参数在此无意义。
                    // IMD 专用参数已在此前由脚本通过结构化入口写入模型。
                    return plugin.beatmap->saveToFile(target)
                               ? std::string{}
                               : std::string("IMD 写出失败");
                }
                // 文本格式保存用户当前预览原文，不重新序列化覆盖现场调整。
                std::ofstream output(target,
                                     std::ios::binary | std::ios::trunc);
                // 文本格式直接写入用户当前预览，不再调用序列化器覆盖人工编辑。
                // 调用方负责传入完整文本；空文本也是明确的用户内容。
                if ( !output ) return std::string("无法打开输出文件");
                output.write(text.data(),
                             static_cast<std::streamsize>(text.size()));
                return output.good() ? std::string{}
                                     : std::string("谱面文本写出失败");
            });
        plugin.api.set_function(
            "audio_probe", [&plugin](const std::string& path) {
                // ICE 提供媒体标签、格式、时长估计与封面字节；不完整解码音轨。
                // 探测由选择文件动作触发，不随每帧重绘重复打开输入文件。
                // 返回字段与 MediaInfo 一一对应，缺失标签保持空字符串。
                // 码率是探测值，不代表后续导出目标的实际码率。
                // 探测只允许在用户动作后执行，避免脚本在 build 中引入每帧文件
                // I/O。
                sol::table output = plugin.lua.create_table();
                if ( !plugin.actionActive || path.empty() ) return output;
                // 空路径不默认指向工程主音频，工具与当前项目保持隔离。
                ice::FFmpegDecoderFactory factory;
                ice::MediaInfo            media;
                if ( !factory.probe(path, media) ) {
                    // 失败返回 error，旧探测缓存由脚本按需要保留或覆盖。
                    output["error"] = "无法读取音频文件";
                    return output;
                }
                output["title"]       = media.title;
                output["artist"]      = media.artist;
                output["album"]       = media.album;
                output["bitrate"]     = media.bitrate;
                output["sample_rate"] = media.format.samplerate;
                output["channels"]    = media.format.channels;
                output["frames"]      = media.frame_count;
                // 探测帧数可能是估计值，不能用于精确编码预算。
                output["duration"] =
                    media.format.samplerate == 0
                        ? 0.0
                        : static_cast<double>(media.frame_count) /
                              media.format.samplerate;
                // 时长由媒体估计帧数计算，零采样率时不能除零或宣称精确时长。
                // 扩展标签通过 FFmpeg 原生字典补充，ICE 的值语义公开结构不变。
                // 部分容器无法二次读取详情时仍保留上面的基础探测结果。
                if ( !appendAudioDetails(plugin.lua, path, output) ) {
                    output["details_error"] = "无法读取完整容器元数据";
                }
                // 封面存在标志要等像素解码成功，避免永远显示上传中占位。
                output["cover_present"] = false;
                plugin.pendingCover.clear();
                // 上一次封面可能仍供在途帧使用，但不应代表新选择的文件显示。
                plugin.coverTextureCurrent = false;
                // 限制压缩封面输入大小和展开像素尺寸，避免异常附件占尽内存。
                // 解码在用户动作期完成；GPU 纹理上传推迟到资源准备阶段。
                if ( media.cover.isValid() &&
                     media.cover.size() <= 16 * 1024 * 1024 ) {
                    // 附件本身存在不代表图片像素可解码，最终标志稍后再设置。
                    int            width = 0, height = 0, channels = 0;
                    unsigned char* pixels = stbi_load_from_memory(
                        media.cover.data(),
                        static_cast<int>(media.cover.size()),
                        &width,
                        &height,
                        &channels,
                        4);
                    if ( pixels && width > 0 && height > 0 && width <= 4096 &&
                         height <= 4096 ) {
                        // 统一转 RGBA，上传阶段无需再次解析图片格式。
                        // 宽高上限避免恶意压缩图片在解码后造成巨大 GPU 纹理。
                        const auto count = static_cast<std::size_t>(width) *
                                           static_cast<std::size_t>(height) * 4;
                        plugin.pendingCover.assign(pixels, pixels + count);
                        plugin.coverWidth       = width;
                        plugin.coverHeight      = height;
                        output["cover_present"] = true;
                    }
                    if ( pixels ) stbi_image_free(pixels);
                    // stb 像素所有权在复制后立即释放，不跨插件生命周期。
                }
                return output;
            });
        plugin.api.set_function("audio_export", [&plugin](sol::table request) {
            // 提交只发生在用户动作；实际解码、DSP 和编码都交给文件线程池。
            // Lua 表在此转换成值语义选项，后台线程不接触 Lua 对象。
            // 参数能否被具体编码器支持仍由离线服务和 ICE 共同确认。
            // 提交成功返回空串不代表导出成功，脚本要读取任务最终状态。
            if ( !plugin.actionActive )
                return std::string("仅可由用户动作启动导出");
            // 同一插件只保留一项进行中的导出，避免状态栏指向错误任务。
            // done 的 acquire 与工作线程 release 配对，结果读取有序。
            if ( plugin.audioTask &&
                 !plugin.audioTask->done.load(std::memory_order_acquire) ) {
                return std::string("已有音频导出任务正在运行");
            }
            auto* pool = Runtime::AppThreadPool::instance().getFileThreadPool();
            // 文件线程池尚未启动时立即拒绝，不能在 UI 线程同步导出。
            if ( !pool ) return std::string("文件任务线程池尚未初始化");
            // 不退回 UI 同步导出；渲染帧必须继续正常推进。
            Audio::AudioSpeedExportOptions options;
            // 路径从 UTF-8 映射到平台文件系统类型，Windows 下保留 Unicode。
            options.inputPath =
                Config::utf8ToPath(request["input"].get_or(std::string{}));
            options.outputPath =
                Config::utf8ToPath(request["output"].get_or(std::string{}));
            options.speed          = request["speed"].get_or(1.0);
            options.pitchSemitones = request["pitch_semitones"].get_or(0.0);
            options.outputSampleRate =
                request["sample_rate"].get_or(std::uint32_t{ 0 });
            options.bitrate = request["bitrate"].get_or(std::uint64_t{ 0 });
            // 独立半音变调要求保持音高图，由拉伸器处理倍速和音调。
            options.preservePitch = true;
            if ( options.inputPath.empty() || options.outputPath.empty() ) {
                // 不允许把空路径交给后台服务，更不能默认写入当前工程目录。
                return std::string("请选择输入和输出文件");
            }
            auto task = std::make_shared<AudioTask>();
            // 进度是独立原子快照，不承载最终结果同步语义。
            // 后台回调只持有任务所有权，插件重载不会让回调访问已销毁对象。
            options.progressCallback =
                [task](const Audio::AudioSpeedExportProgress& progress) {
                    task->progress.store(progress.progress,
                                         std::memory_order_relaxed);
                };
            plugin.audioTask            = task;
            plugin.audioCompletionShown = false;
            // UI 在下一次 rebuild 中读到 running，普通帧仅直接绘制原子进度。
            // 工作线程只持有任务与参数副本，不借用插件视图或 Lua 对象。
            pool->enqueue_void([task, options = std::move(options)]() {
                // 异步回调不更新插件或窗口状态，避免重载期间竞争 Lua 生命周期。
                task->result =
                    Audio::AudioSpeedExportService::exportWav(options);
                // 写完普通 result 后再发布完成位，UI 不需等待锁。
                task->done.store(true, std::memory_order_release);
            });
            return std::string{};
        });
        plugin.api.set_function("audio_status", [&plugin]() {
            // Lua 查询只在 build 期发生，用于初始、完成与错误状态文字。
            // 实时进度控件绕过 Lua，直接读同一任务的原子进度。
            sol::table  status = plugin.lua.create_table();
            const auto* task   = plugin.audioTask.get();
            if ( !task ) {
                // 尚未提交任务时不制造虚假的百分比或结果。
                status["state"] = "idle";
            } else if ( !task->done.load(std::memory_order_acquire) ) {
                // 未完成时只能读原子进度，不能读正在写入的 result。
                status["state"] = "running";
                status["progress"] =
                    task->progress.load(std::memory_order_relaxed);
            } else {
                // acquire 之后结果稳定，可复制到独立 Lua 表中供窗口构建。
                status["state"]    = task->result.success ? "success" : "error";
                status["error"]    = task->result.errorMessage;
                status["frames"]   = task->result.outputFrames;
                status["duration"] = task->result.outputDurationSeconds;
            }
            return status;
        });
        plugin.api.set_function(
            "pick_file", [&plugin](const std::string& purpose) {
                // purpose 由插件定义，输入与输出或不同数据类型可使用独立历史。
                // 禁止从 build 调用文件对话框，避免加载期或重复绘制阻塞 UI。
                if ( !plugin.actionActive || purpose.empty() ) {
                    // 用途键为空时不共享默认最近目录，避免插件间混淆。
                    return std::string{};
                }
                const auto found = plugin.recent.find(purpose);
                // 只借用映射值直到对话框返回；本段期间不会修改同一映射。
                const char*  defaultPath = found == plugin.recent.end()
                                               ? nullptr
                                               : found->second.c_str();
                nfdu8char_t* selected    = nullptr;
                const auto   result      = NativeFileDialog::openFile(
                    &selected, nullptr, 0, defaultPath);
                // 取消选择不会覆盖之前的目录，也不向 Lua 暴露空指针。
                if ( result != NFD_OKAY || !selected ) return std::string{};
                std::string path(selected);
                // NFD 自己分配的 UTF-8 路径必须在复制后通过对应释放函数回收。
                NFD_FreePathU8(selected);
                plugin.recent[purpose] =
                    Config::pathToUtf8(Config::utf8ToPath(path).parent_path());
                (void)saveRecent(plugin.info.id, plugin.recent);
                // 历史写盘失败不否定已成功选中的文件，用户仍能继续动作。
                return path;
            });
        plugin.api.set_function(
            "save_file",
            [&plugin](const std::string& purpose,
                      const std::string& suggestedName) {
                // 保存对话框只返回用户选定路径，不在此处打开或截断目标文件。
                // 文件后缀由脚本和后端保存 API 校验，建议名称不能替代校验。
                if ( !plugin.actionActive || purpose.empty() ) {
                    return std::string{};
                }
                const auto found = plugin.recent.find(purpose);
                // 每个插件 ID 的用途键独立持久化，重载后仍能恢复最近位置。
                const char*  defaultPath = found == plugin.recent.end()
                                               ? nullptr
                                               : found->second.c_str();
                nfdu8char_t* selected    = nullptr;
                const auto   result      = NativeFileDialog::saveFile(
                    &selected,
                    nullptr,
                    0,
                    defaultPath,
                    suggestedName.empty() ? nullptr : suggestedName.c_str());
                // 取消时返回空串，调用方负责保留当前输出路径。
                if ( result != NFD_OKAY || !selected ) return std::string{};
                std::string path(selected);
                NFD_FreePathU8(selected);
                // 只记住父目录，不能把上一次文件名误当成下一次起始目录。
                plugin.recent[purpose] =
                    Config::pathToUtf8(Config::utf8ToPath(path).parent_path());
                (void)saveRecent(plugin.info.id, plugin.recent);
                return path;
            });
    }

    /// @brief 从脚本文本创建独立状态并缓存声明式控件。
    /// @param script 完整 Lua 脚本文本。
    /// @param chunkName 错误诊断中的脚本来源名称。
    /// @warning 加载低频路径：执行 Lua、分配控件与读取插件状态。
    void loadScript(std::string_view script, std::string chunkName)
    {
        // 每个脚本独立创建虚拟机，插件之间不能读写彼此的全局表。
        // 入口来源名称进入 Lua 错误栈，列表中可以定位具体文件。
        // 不运行 io/os/package，文件和输出能力必须走受控宿主 API。
        auto plugin = std::make_unique<Plugin>();
        plugin->lua.open_libraries(
            sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
        // base 库默认还暴露 dofile/loadfile；移除磁盘读入口，文件访问只经宿主
        // API。
        plugin->lua["dofile"]   = sol::lua_nil;
        plugin->lua["loadfile"] = sol::lua_nil;
        // safe_script 将语法及运行错误作为结果，不允许错误跨到 C++ 异常域。
        // 插件入口可有本地 state 闭包，但必须返回一份声明表。
        const auto result = plugin->lua.safe_script(
            std::string(script), sol::script_pass_on_error, chunkName);
        if ( !result.valid() ) {
            // 失败脚本不创建可打开窗口，但应留在插件列表供作者排查。
            const sol::error error = result;
            recordLoadError(std::move(chunkName), error.what());
            return;
        }
        const sol::object object = result;
        if ( !object.is<sol::table>() ) {
            // 单一布尔或字符串不足以描述稳定插件 ID 和回调函数。
            recordLoadError(std::move(chunkName), "入口必须返回表");
            return;
        }
        const sol::table definition = object.as<sol::table>();
        if ( tableString(definition, "type") != "tool" ) {
            // 主题脚本误放到 tools 目录时指出类型错误，不交给主题加载器。
            recordLoadError(std::move(chunkName), "type 必须是 tool");
            return;
        }
        plugin->info.id   = tableString(definition, "id");
        plugin->info.name = tableString(definition, "name");
        // 稳定 ID 用作窗口内部身份及状态文件名，必须拒绝路径分隔符。
        // 可见名称允许本地化或后续修改，不能用于持久化键。
        if ( !validPluginId(plugin->info.id) || plugin->info.name.empty() ) {
            recordLoadError(std::move(chunkName), "插件 ID 或名称无效");
            return;
        }
        plugin->windowTitle =
            plugin->info.name + "###ToolPlugin_" + plugin->info.id;
        // 同一 ID 不能有两个窗口或最近目录文件，先加载的定义保持所有权。
        if ( std::any_of(
                 loaded.begin(), loaded.end(), [&plugin](const auto& old) {
                     return old->info.id == plugin->info.id;
                 }) ) {
            recordLoadError(std::move(chunkName), "插件 ID 与已加载工具重复");
            return;
        }
        const sol::object build    = definition["build"];
        const sol::object onAction = definition["on_action"];
        // 回调缺失时没有途径构造和更新声明式控件树，拒绝半成品插件。
        if ( !build.is<sol::protected_function>() ||
             !onAction.is<sol::protected_function>() ) {
            recordLoadError(std::move(chunkName),
                            "build 和 on_action 必须是函数");
            return;
        }
        plugin->build    = build.as<sol::protected_function>();
        plugin->onAction = onAction.as<sol::protected_function>();
        // 最近目录按 ID 载入，不与主题插件的启用状态共用配置键。
        plugin->recent = loadRecent(plugin->info.id);
        // API 绑定发生在首次 build 前，入口自身只定义清单，不做文件 I/O。
        bindApi(*plugin);
        (void)rebuild(*plugin);
        // build 失败的合法插件仍保留窗口和错误文字，重载后可以恢复。
        infos.push_back(plugin->info);
        loaded.push_back(std::move(plugin));
    }

    /// @brief 从用户插件目录读取一个脚本并交给同一入口加载。
    /// @param path Lua 脚本完整路径。
    /// @warning 显式重载低频路径：读取磁盘，不进入 UI 每帧绘制。
    void loadFile(const std::filesystem::path& path)
    {
        // 只在应用启动或用户点击重载时访问目录，不在普通帧检查修改时间。
        // 读取失败保留来源项，避免作者误以为脚本没有被扫描到。
        std::ifstream file(path, std::ios::binary);
        if ( !file ) {
            recordLoadError(Config::pathToUtf8(path), "无法读取 Lua 文件");
            return;
        }
        const std::string script((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
        // 文件全量复制到内存后交给加载器，Lua chunk 不借用流缓冲生命周期。
        loadScript(script, Config::pathToUtf8(path));
    }
};

/// @brief 创建工具插件视图并扫描一次用户插件目录。
ToolPluginView::ToolPluginView()
    : IUIView("ToolPluginView")
    , ITextureLoader("ToolPluginView")
    , m_impl(std::make_unique<Impl>())
{
    // 构造发生在 UI 注册阶段，不是每帧路径；此时载入首批内置与外置脚本。
    // 后续目录刷新只能由用户显式操作触发。
    reload();
}

/// @brief 确保 Lua 状态早于视图基类销毁。
ToolPluginView::~ToolPluginView() = default;

/// @brief 替换旧插件状态并扫描工具目录中的 Lua 文件。
void ToolPluginView::reload()
{
    // 先转移旧 GPU 资源，再销毁 Lua 状态与控件缓存。
    // 音频工作线程只持有任务和参数，重载不会留下对插件对象的借用。
    // 该函数允许文件系统扫描，但调用方必须保证不是 UI 每帧调用。
    // 旧纹理不能在用户点击重载时立即销毁，在途帧可能仍借用其 descriptor。
    for ( const auto& plugin : m_impl->loaded ) {
        if ( plugin->coverTexture ) {
            m_impl->retiredTextures.push_back(std::move(plugin->coverTexture));
        }
    }
    m_impl->loaded.clear();
    m_impl->infos.clear();
    // 内置工具先占用固定 ID，外置脚本不能无提示地替换它们。
    // 嵌入文本不依赖运行目录或安装包中的 assets 文件。
    // 内置脚本不依赖安装包资源目录，也不会覆写用户自己的插件文件。
    m_impl->loadScript(BuiltinToolPlugins::AUDIO, "builtin:audio");
    m_impl->loadScript(BuiltinToolPlugins::BEATMAP, "builtin:beatmap");
    const auto      directory = Config::AppPaths::pluginsRootPath() / "tools";
    std::error_code error;
    // 用户没有安装工具时目录可自动创建；失败仍保留内置脚本。
    std::filesystem::create_directories(directory, error);
    if ( error ) return;
    std::vector<std::filesystem::path> paths;
    // 只扫描 tools 顶层 Lua 文件，避免走进第三方或项目资源子目录。
    // 用 error_code 处理权限等文件系统故障，不跨 C++ 异常边界。
    for ( std::filesystem::directory_iterator it(directory, error), end;
          !error && it != end;
          it.increment(error) ) {
        if ( it->is_regular_file(error) && !error &&
             it->path().extension() == ".lua" ) {
            paths.push_back(it->path());
        }
    }
    // 排序使重复 ID 的先后顺序可预测，也稳定插件列表顺序。
    std::sort(paths.begin(), paths.end());
    for ( const auto& path : paths ) m_impl->loadFile(path);
}

/// @brief 打开指定 ID 对应的插件窗口。
bool ToolPluginView::openPlugin(std::string_view id)
{
    // 只修改内存窗口状态，真正的 ImGui Begin 发生在后续 update。
    // 错误列表项不在 loaded 中，因此不能打开为空窗口。
    for ( const auto& plugin : m_impl->loaded ) {
        if ( plugin->info.id == id ) {
            // 内部 ID 不变；重复打开不会重置已有停靠位置。
            plugin->open = true;
            return true;
        }
    }
    return false;
}

/// @brief 借用当前插件状态快照。
const std::vector<ToolPluginInfo>& ToolPluginView::plugins() const
{
    // 调用方只在当前 UI 帧借用快照，重载后应重新获取引用。
    return m_impl->infos;
}

/// @brief 绘制缓存控件，并在窗口关闭后处理本帧用户动作。
void ToolPluginView::update(UIManager* sourceManager)
{
    // 此函数每个 UI 帧执行，只借用已加载插件和控件缓存。
    // 不得在无用户动作时读写磁盘、运行 Lua 或等待音频工作线程。
    // 允许一个完成标志触发一次低频 rebuild，以刷新最终状态文本。
    // 热路径不遍历谱面实体，也不从用户目录重新读取 Lua 脚本。
    // 所有按钮和文本字段都由预解析控件描述驱动。
    (void)sourceManager;
    for ( const auto& owner : m_impl->loaded ) {
        // unique_ptr 向量拥有插件，循环中借用稳定引用而不复制共享所有权。
        auto& plugin = *owner;
        // 后台先写结果再发布 done，普通帧只做不阻塞的原子检查。
        if ( plugin.audioTask && !plugin.audioCompletionShown &&
             plugin.audioTask->done.load(std::memory_order_acquire) ) {
            // acquire 保证后台结果完整发布；标记先于 rebuild 防止重复执行。
            plugin.audioCompletionShown = true;
            (void)Impl::rebuild(plugin);
        }
        if ( !plugin.open ) continue;
        // 关闭窗口时跳过所有 ImGui 控件，后台任务仍可继续执行。
        // 标题在加载时缓存，避免逐帧拼接字符串和生成新的停靠 ID。
        // 不因名称文字改变而修改窗口内部 ID，用户并排停靠布局可恢复。
        const bool wasOpen = plugin.open;
        const bool visible =
            ImGui::Begin(plugin.windowTitle.c_str(), &plugin.open);
        FeedbackCurrentWindowCloseButton(wasOpen, &plugin.open);
        // 一个窗口本帧最多提交一次动作，所有控件先完成绘制后才进入 Lua。
        // 这样回调重建控件树不会让当前 for 遍历的 Widget 引用失效。
        std::string actionId;
        std::string actionValue;
        if ( visible ) {
            // ImGui::Begin 返回折叠状态；End 仍需无条件执行。
            // 折叠窗口不会遍历控件，也就不会产生输入动作。
            if ( !plugin.info.error.empty() ) {
                // 脚本错误只作为窗口文字显示，不破坏后续控件栈。
                ImGui::TextWrapped("%s", plugin.info.error.c_str());
            }
            for ( auto& widget : plugin.widgets ) {
                // 插件内稳定 ID 形成局部 ImGui 命名空间，可见标签可以重复。
                ImGui::PushID(widget.id.c_str());
                if ( widget.type == "text" ) {
                    // 只读文字不触发回调，适合元数据摘要和错误状态。
                    ImGui::TextWrapped("%s", widget.label.c_str());
                } else if ( widget.type == "separator" ) {
                    // 分组标题由宿主绘制，Lua 无法改变 ImGui 栈结构。
                    ImGui::SeparatorText(widget.label.c_str());
                } else if ( widget.type == "button" ) {
                    // 可见按钮必须走项目反馈封装，保留统一声音和悬浮效果。
                    if ( FeedbackButton(widget.label.c_str()) ) {
                        actionId = widget.id;
                    }
                } else if ( widget.type == "checkbox" ) {
                    // Lua 只见 "true"/"false" 字符串，不暴露栈上 bool 地址。
                    // 变更当帧记录新值，脚本完成后再重建声明。
                    bool checked = widget.value == "true";
                    if ( FeedbackCheckbox(widget.label.c_str(), &checked) ) {
                        actionId    = widget.id;
                        actionValue = checked ? "true" : "false";
                    }
                } else if ( widget.type == "combo" ) {
                    // 选项来自构建期复制的 C++ 向量，逐帧不遍历 Lua table。
                    if ( ImGui::BeginCombo(widget.label.c_str(),
                                           widget.value.c_str()) ) {
                        for ( const auto& choice : widget.choices ) {
                            // 选中值按字符串返回，脚本可用稳定文本保存选择。
                            if ( ImGui::Selectable(choice.c_str(),
                                                   choice == widget.value) ) {
                                actionId    = widget.id;
                                actionValue = choice;
                            }
                        }
                        ImGui::EndCombo();
                        // BeginCombo 成功时必须在同一帧关闭 popup 栈。
                    }
                } else if ( widget.type == "image" ) {
                    // 资源阶段尚未上传封面时显示占位文本，不在此构建纹理。
                    // 即使脚本声明图片，也不允许通过任意路径隐式加载文件。
                    if ( plugin.coverTexture && plugin.coverTextureCurrent ) {
                        // 固定可见宽度并按原始宽高比缩放，避免封面拉伸。
                        const float width = 180.0F;
                        const float height =
                            plugin.coverWidth > 0
                                ? width * plugin.coverHeight / plugin.coverWidth
                                : width;
                        ImGui::Image(plugin.coverTexture->getImTextureID(),
                                     ImVec2(width, height));
                        // descriptor 由 VKTexture 持有，旧纹理延迟回收。
                    } else {
                        ImGui::TextDisabled("%s", widget.label.c_str());
                    }
                } else if ( widget.type == "audio_progress" ) {
                    // 进度只读共享任务原子快照；普通帧不调用 Lua 或文件系统。
                    // relaxed 读取只需数值近似，完成结果另由 done 同步。
                    // clamp 防止异常回调或浮点舍入导致进度条超出有效范围。
                    const float progress =
                        plugin.audioTask ? plugin.audioTask->progress.load(
                                               std::memory_order_relaxed)
                                         : 0.0F;
                    ImGui::TextUnformatted(widget.label.c_str());
                    ImGui::ProgressBar(std::clamp(progress, 0.0F, 1.0F));
                } else if ( widget.type == "input" ||
                            widget.type == "multiline" ) {
                    // 文本由 Widget 的 std::string
                    // 拥有，增长只发生在用户输入时。 大预览动作可返回 false
                    // 保留这份已编辑的 C++ 缓存。
                    const bool changed =
                        inputTextDynamic(widget.label.c_str(),
                                         widget.value,
                                         widget.type == "multiline");
                    if ( changed ) {
                        // 修改值复制一次给动作回调，普通静态帧不复制全文。
                        actionId    = widget.id;
                        actionValue = widget.value;
                    }
                }
                ImGui::PopID();
                // 每个控件自身配对 PushID/PopID，不依赖插件脚本正确性。
            }
        }
        ImGui::End();
        // 退出当前 ImGui 窗口后才允许 Lua 改变控件树。
        if ( actionId.empty() ) continue;
        // actionActive 只覆盖当前同步回调，不传到音频后台任务。
        plugin.actionActive = true;
        // 回调期间允许打开文件对话框；它由明确用户动作触发。
        const auto result = plugin.onAction(actionId, actionValue, plugin.api);
        plugin.actionActive = false;
        if ( !result.valid() ) {
            // protected_function 的错误保留在窗口，下帧仍能关闭或重载。
            // 出错时不使用可能已部分修改的 Lua 声明重建控件树。
            const sol::error error = result;
            plugin.info.error      = error.what();
        } else {
            // 返回 false 表示控件值已在 C++ 缓存中更新，无需复制整份长预览。
            if ( result.return_count() == 0 ) {
                // 默认重建让按钮结果、元数据字段和状态信息立即反映到界面。
                (void)Impl::rebuild(plugin);
            } else {
                const sol::object response = result;
                if ( !response.is<bool>() || response.as<bool>() ) {
                    // 只有明确返回 false 才跳过重建；其它返回值维持默认协议。
                    (void)Impl::rebuild(plugin);
                }
            }
        }
    }
}

/// @brief 检查是否有新探测出的封面等待 GPU 上传。
bool ToolPluginView::needReload()
{
    // 渲染器每帧可能查询此标志，因此只检查内存中待上传像素。
    // 不能在这里解析封面、访问路径、创建 descriptor 或等待 GPU。
    return std::any_of(
        m_impl->loaded.begin(), m_impl->loaded.end(), [](const auto& plugin) {
            // 空向量代表没有新图像待办，不代表旧纹理应被销毁。
            return !plugin->pendingCover.empty();
        });
}

/// @brief 上传新封面并把旧纹理保留到视图安全销毁。
void ToolPluginView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                    vk::Device&         logicalDevice,
                                    vk::CommandPool& cmdPool, vk::Queue& queue)
{
    // 只有资源准备阶段可上传 Vulkan 纹理，不在普通 ImGui 绘制中做 GPU 操作。
    // 像素已经由用户探测动作解码为 RGBA，当前只消费最新待办。
    for ( const auto& plugin : m_impl->loaded ) {
        if ( plugin->pendingCover.empty() ) continue;
        // 上传使用渲染器提供的设备、命令池与队列，视图不单独创建 Vulkan
        // 上下文。
        auto texture = std::make_unique<Graphic::VKTexture>(
            plugin->pendingCover.data(),
            static_cast<std::uint32_t>(plugin->coverWidth),
            static_cast<std::uint32_t>(plugin->coverHeight),
            physicalDevice,
            logicalDevice,
            cmdPool,
            queue);
        plugin->pendingCover.clear();
        // 上传失败保留旧纹理供在途帧使用，但窗口不会把它误认成新封面。
        if ( !texture->isValid() ) continue;
        // 资源阶段预先取得 ImGui descriptor，逐帧 Image 只引用已有句柄。
        static_cast<void>(texture->getImTextureID());
        if ( plugin->coverTexture ) {
            // 当前仍可能有在途帧借用旧 descriptor，保留所有权直到安全销毁。
            m_impl->retiredTextures.push_back(std::move(plugin->coverTexture));
        }
        // 新纹理成为后续 UI 帧显示的封面；尺寸来自同次解码结果。
        plugin->coverTexture        = std::move(texture);
        plugin->coverTextureCurrent = true;
    }
}

}  // namespace MMM::UI
