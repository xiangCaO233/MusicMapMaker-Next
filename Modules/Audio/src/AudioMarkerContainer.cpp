#include "AudioMarkerInternal.h"

#include "config/Utf8Path.h"
#include "mmm/SafeParse.h"
#include "mmm/timing/BpmNormalization.h"

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <utility>

namespace MMM::Audio::MarkerInternal
{
namespace
{
/// @brief FFmpeg 输入上下文的 RAII 释放器，不暴露给公开 DTO。
struct InputCloser {
    /// @brief 无论探测还是读取失败，都释放输入 IO 和上下文。
    void operator()(AVFormatContext* input) const
    {
        if ( input ) avformat_close_input(&input);
    }
};
/// @brief FFmpeg 输出上下文的 RAII 释放器，IO 必须先关闭。
struct OutputCloser {
    /// @brief 文件结束或早退时释放 muxer 分配的章节与流。
    void operator()(AVFormatContext* output) const
    {
        if ( !output ) return;
        if ( output->pb ) avio_closep(&output->pb);
        avformat_free_context(output);
    }
};
/// @brief 每个复用数据包在退出时自动清理引用。
struct PacketCloser {
    /// @brief 释放包头及尚未移交 muxer 的载荷。
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
/// @brief 输出选项的作用域所有者，避免早退泄漏字典。
struct Dictionary {
    /// @brief 交给 FFmpeg 修改的选项字典。
    AVDictionary* value{ nullptr };
    /// @brief 清理未消费的选项。
    ~Dictionary() { av_dict_free(&value); }
};

/// @brief 建立只读输入上下文，使用跨平台 UTF-8 路径。
/// @param path 音频的原生文件路径。
/// @return 成功探测音频流的输入上下文，失败返回空所有者。
/// @details 输入 IO 始终由 FFmpeg 管理，不复用输出上下文。
/// 先接管 raw 的释放责任，再检查状态，覆盖部分初始化失败。
/// find_stream_info 仅在离线路径调用，不重复放入 UI 轮询。
/// UTF-8 路径来自项目统一转换入口，Windows 不依赖系统 ANSI 代码页。
static std::unique_ptr<AVFormatContext, InputCloser> openInput(
    const std::filesystem::path& path)
{
    // FFmpeg 打开失败也可能建立部分上下文，立即交给作用域释放器。
    // 输入路径通过项目 UTF-8 转换，不能直接使用 Windows path.string。
    // 探测只为获取流和容器时钟，不生成 PCM 缓存。
    AVFormatContext* raw  = nullptr;
    const auto       utf8 = Config::pathToUtf8(path);
    const auto       status =
        avformat_open_input(&raw, utf8.c_str(), nullptr, nullptr);
    std::unique_ptr<AVFormatContext, InputCloser> input(raw);
    if ( status < 0 || !input ||
         avformat_find_stream_info(input.get(), nullptr) < 0 )
        return {};
    return input;
}

/// @brief 错误码转为可报告的文本，固定栈缓冲不依赖异常。
/// @param status FFmpeg 返回的负错误码。
/// @return 可放入用户文件事务反馈的错误文本。
/// @details 固定大小缓冲由 libavutil 定义，调用方不拼接未终止的 C 文本。
/// 不抛出异常，也不在错误发生后改写输出成功状态。
static std::string describeError(int status)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(status, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

/// @brief 从字典中检索精确载荷；用户普通评论不属于 BPM 数据。
/// @return 0 为没有载荷，1 为已解析，-1 为版本化标签损坏。
/// @param metadata 全局、章节或流级字典，可为空。
/// @param data 成功解析后替换的精确测量快照。
/// @return 三态结果区分不存在、合法载荷和损坏载荷。
/// @details 只识别带版本边界的载荷，普通音乐备注不会当作 BPM。
/// 预算限制在 JSON 解析前执行，避免长标签扩大解析内存。
/// 边界必须完整，未终止的私有评论明确报错。
/// 未知版本也属于损坏或不支持，不能回退到全局单一 BPM。
/// 数据只在解析完全成功后提交，多个字典之间不混合部分内容。
static int findPayload(const AVDictionary* metadata, AudioMarkerData& data)
{
    const AVDictionaryEntry* entry = nullptr;
    while (
        (entry = av_dict_get(metadata, "", entry, AV_DICT_IGNORE_SUFFIX)) ) {
        // FFmpeg 提供零结尾值；检查预算后才交给 JSON 解析器。
        const std::string_view value(entry->value);
        if ( value.size() > MAX_METADATA_BYTES ) continue;
        const auto begin = value.find(PAYLOAD_BEGIN);
        if ( begin == std::string_view::npos ) continue;
        const auto content = begin + PAYLOAD_BEGIN.size();
        const auto end     = value.find(PAYLOAD_END, content);
        if ( end == std::string_view::npos ||
             !decodeData(value.substr(content, end - content), data) )
            return -1;
        return 1;
    }
    return 0;
}

/// @brief 从通用 BPM 标签读取有限十进制数，兼容 Apple 旧部署目标。
/// @param metadata 尚未包含私有载荷的普通音频标签。
/// @return 合法 BPM 值；没有有效标签时返回零。
/// @details 兼容常见 bpm、TBPM 和 tempo 字段名。
/// 不读取地域小数点、十六进制或带单位的宽松前缀。
/// 浮点解析使用项目公共 helper，保证旧 macOS 部署目标可用。
/// 全局标签只能代表一个 BPM，不能从它恢复作者没有写出的变速段。
/// 范围与测量工具一致，但错误值不会通过归一化伪装成合法测量。
static double taggedBpm(const AVDictionary* metadata)
{
    for ( const auto* key : { "bpm", "TBPM", "tempo" } ) {
        const auto* entry = av_dict_get(metadata, key, nullptr, 0);
        if ( !entry ) continue;
        const std::string_view text(entry->value);
        // 不接受十六进制、区域小数点或尾随文字。
        if ( text.empty() || text.find_first_not_of("0123456789.-+eE") !=
                                 std::string_view::npos )
            continue;
        const auto parsed = MMM::Internal::parseFloatingPrefix(text);
        if ( parsed.error == std::errc{} &&
             parsed.parsedLength == text.size() &&
             std::isfinite(parsed.value) &&
             parsed.value >= MIN_NORMALIZED_BPM &&
             parsed.value <= MAX_NORMALIZED_BPM )
            return parsed.value;
    }
    return 0.0;
}

/// @brief 用新载荷替换旧载荷边界，保留普通 comment 内容。
/// @param metadata 当前输出上下文持有的可修改字典。
/// @param payload 不带边界的精确测量 JSON。
/// @details 原评论的载荷之外部分保留，新数据替换旧版本边界。
/// 不向源上下文的字典写入，因此来源元数据仍只读。
/// comment 使用广泛支持的字段，容器是否真正保存由回读决定。
/// 插入换行只分隔普通文本和私有部分，不改变 JSON 中的标题。
/// 输出字典独立拥有 C 字符串，参数缓冲可以在调用结束后释放。
static void setPayload(AVDictionary** metadata, const std::string& payload)
{
    // 复用来源 comment，普通曲目信息不能因为保存 BPM 而清空。
    // 只有明确包裹在版本边界内的旧数据可以替换。
    // 新旧载荷不会并存，读取优先级无需依赖修改日期。
    const auto* entry   = av_dict_get(*metadata, "comment", nullptr, 0);
    std::string comment = entry ? entry->value : "";
    const auto  begin   = comment.find(PAYLOAD_BEGIN);
    if ( begin != std::string::npos ) {
        const auto end =
            comment.find(PAYLOAD_END, begin + PAYLOAD_BEGIN.size());
        comment.erase(begin,
                      end == std::string::npos
                          ? std::string::npos
                          : end + PAYLOAD_END.size() - begin);
    }
    if ( !comment.empty() && comment.back() != '\n' ) comment.push_back('\n');
    comment += PAYLOAD_BEGIN;
    comment += payload;
    comment += PAYLOAD_END;
    av_dict_set(metadata, "comment", comment.c_str(), 0);
}

/// @brief 清除旧的标准章节评论，防止缩短章节列表后残留尾部标签。
/// @param metadata 来源已复制到输出的全局或音频流字典。
/// @details 只识别 CHAPTER 加三位编号的章节标签，不删除普通用户字段。
/// 名称字段共享相同编号前缀，因此与章节位置一同移除。
/// 先收集键再删除，避免 FFmpeg 字典扩缩容使遍历指针失效。
/// 旧章节丢弃之后由本次完整的可见标记快照重建。
/// @warning 离线导出路径执行，不在每帧或播放回调中调用。
static void clearChapterTags(AVDictionary** metadata)
{
    std::vector<std::string> keys;
    const AVDictionaryEntry* entry = nullptr;
    while (
        (entry = av_dict_get(*metadata, "", entry, AV_DICT_IGNORE_SUFFIX)) ) {
        std::string key(entry->key);
        auto        normalized = key;
        // 标签比较仅归一化 ASCII，不依赖用户区域设置。
        for ( auto& c : normalized )
            if ( c >= 'a' && c <= 'z' ) c = static_cast<char>(c - 'a' + 'A');
        if ( normalized.size() >= 10 && normalized.starts_with("CHAPTER") &&
             std::all_of(normalized.begin() + 7,
                         normalized.begin() + 10,
                         [](char c) { return c >= '0' && c <= '9'; }) )
            keys.push_back(std::move(key));
    }
    // 删除只影响输出字典；输入的原始标签始终保持只读。
    for ( const auto& key : keys )
        av_dict_set(metadata, key.c_str(), nullptr, 0);
}

/// @brief 生成标准章节表及 VorbisComment 章节，使用真实音频时间。
/// @details 章节结束取下一起点或音频末尾，私有数据不依赖毫秒取整。
/// @param output 事务独占的输出上下文，尚未调用 write_header。
/// @param chapters 有序非负的外部可见时间点。
/// @param duration 真实音频末尾，用作最后章节终点。
/// @return 所有章节分配均成功时返回 true。
/// @details AVChapter 数组由 FFmpeg 分配器建立并交给上下文释放。
/// nb_chapters 只统计已交付的对象，部分失败也可安全清理。
/// 章节表使用微秒时基，私有 double 数据不依赖标准表的取整精度。
/// 同时写入 VorbisComment 章节约定，支持没有原生章节表的 Ogg。
/// 每个章节的 NAME 与时间使用同编号，外部软件可恢复显示名称。
/// 输入数据仍保留作者列表，生成顺序不回写原 DTO。
/// @warning 离线一次性分配，禁止为显示一根拍线每帧建立章节对象。
static bool setChapters(AVFormatContext&                 output,
                        const std::vector<AudioChapter>& chapters,
                        double                           duration)
{
    if ( chapters.empty() ) return true;
    output.chapters = static_cast<AVChapter**>(
        av_calloc(chapters.size(), sizeof(AVChapter*)));
    if ( !output.chapters ) return false;
    for ( std::size_t i = 0; i < chapters.size(); ++i ) {
        auto* chapter = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
        if ( !chapter ) return false;
        // 立即移交上下文所有权，后续任何分配失败由 RAII 完整释放。
        output.chapters[output.nb_chapters++] = chapter;
        chapter->id                           = static_cast<std::int64_t>(i);
        // 统一章节时基采用微秒，覆盖常见精度同时避免浮点累加。
        // 章节 ID 只服务新输出，不继承来源可能重复的 ID。
        // 私有载荷独立保存 double，标准时基量化不影响产品内往返。
        chapter->time_base = { 1, 1000000 };
        chapter->start =
            static_cast<std::int64_t>(std::llround(chapters[i].seconds * 1e6));
        const double end =
            i + 1 < chapters.size() ? chapters[i + 1].seconds : duration;
        chapter->end = std::max(
            chapter->start, static_cast<std::int64_t>(std::llround(end * 1e6)));
        av_dict_set(&chapter->metadata, "title", chapters[i].title.c_str(), 0);
        const auto milliseconds = static_cast<std::int64_t>(
            std::llround(chapters[i].seconds * 1000.0));
        const auto key  = fmt::format("CHAPTER{:03}", i + 1);
        const auto time = fmt::format("{:02}:{:02}:{:02}.{:03}",
                                      milliseconds / 3600000,
                                      milliseconds / 60000 % 60,
                                      milliseconds / 1000 % 60,
                                      milliseconds % 1000);
        // Ogg/FLAC 使用标准章节评论扩展；其他容器仍有原生章节表。
        av_dict_set(&output.metadata, key.c_str(), time.c_str(), 0);
        av_dict_set(&output.metadata,
                    (key + "NAME").c_str(),
                    chapters[i].title.c_str(),
                    0);
    }
    return true;
}
}  // namespace

/// @brief 查询音频原始时间域的时长。
/// @param path 只读音频路径。
/// @return 可确定的正时长；没有可用时钟时返回零。
/// @details 优先使用容器总时长，其次使用音频流的 time_base 和 duration。
/// 不按内部解码缓存帧数推导，避免重采样和格式延迟影响原时钟。
/// 查找不持有项目或 UI 锁，上下文仅属于当前后台调用。
/// 失败零值交给导出事务决定是否可展开整拍，不臆造无限时长。
double audioDuration(const std::filesystem::path& path)
{
    const auto input = openInput(path);
    if ( !input ) return 0.0;
    // 只读容器时间，不通过重采样后的内部 PCM 时钟反推。
    if ( input->duration > 0 && input->duration != AV_NOPTS_VALUE )
        return static_cast<double>(input->duration) / AV_TIME_BASE;
    for ( unsigned i = 0; i < input->nb_streams; ++i ) {
        const auto* stream = input->streams[i];
        if ( stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
             stream->duration > 0 && stream->duration != AV_NOPTS_VALUE )
            return stream->duration * av_q2d(stream->time_base);
    }
    return 0.0;
}

/// @brief 读取常见音频容器的章节及 BPM 元数据。
/// @param path 来源音频文件路径。
/// @return 精确私有数据或外部标准数据；无标签时返回成功空快照。
/// @details 全局标签和流标签按明确优先级查找，不逐帧轮询。
/// 私有载荷存在时必须完整有效，它保留负首拍和多段 BPM。
/// 没有私有数据时使用标准 AVChapter 的真实时基转换秒数。
/// 只包含名称的章节不会被强行赋予默认 BPM。
/// 标准章节若有 bpm 标签，可恢复对应的独立节拍锚点。
/// 全局 BPM 不具有 offset，恢复为音频零点起始而非猜测第一章起点。
/// 章节数量和标量类型在交付 UI 前统一检查。
/// @warning 完整容器探测属于后台文件路径，不在实时音频回调调用。
AudioMarkerReadResult readContainer(const std::filesystem::path& path)
{
    const auto input = openInput(path);
    if ( !input ) return { false, "无法读取音频容器", {} };
    AudioMarkerReadResult result{ true, {}, {} };
    int                   payload = findPayload(input->metadata, result.data);
    for ( unsigned i = 0; payload == 0 && i < input->nb_streams; ++i ) {
        // Ogg 等格式把评论保存在音频流，不在全局字典中。
        payload = findPayload(input->streams[i]->metadata, result.data);
    }
    if ( payload < 0 ) return { false, "音频内嵌测量标记损坏或版本不支持", {} };
    // 精确载荷与标准章节粒度不同，不能把两者合并成重复标记。
    // 私有数据已有校验，直接返回以保留原始列表顺序。
    // 普通第三方文件则继续读取标准章节和 BPM 标签。
    if ( payload > 0 ) return result;
    if ( input->nb_chapters > MAX_MARKERS )
        return { false, "音频章节数量超限", {} };
    for ( unsigned i = 0; i < input->nb_chapters; ++i ) {
        const auto*  chapter = input->chapters[i];
        const double seconds = chapter->start * av_q2d(chapter->time_base);
        const auto* title = av_dict_get(chapter->metadata, "title", nullptr, 0);
        result.data.chapters.push_back({ seconds, title ? title->value : "" });
        const double bpm = taggedBpm(chapter->metadata);
        if ( bpm > 0.0 ) result.data.bpmSegments.push_back({ seconds, bpm });
    }
    double bpm = taggedBpm(input->metadata);
    for ( unsigned i = 0; bpm == 0.0 && i < input->nb_streams; ++i )
        bpm = taggedBpm(input->streams[i]->metadata);
    if ( result.data.bpmSegments.empty() && bpm > 0.0 ) {
        // 普通全局 BPM 标签没有 offset，不凭章节起点臆造首拍偏移。
        result.data.bpmSegments.push_back({ 0.0, bpm });
    }
    std::stable_sort(
        result.data.bpmSegments.begin(),
        result.data.bpmSegments.end(),
        [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
    if ( !validData(result.data) )
        return { false, "音频章节或 BPM 标签无效", {} };
    return result;
}

/// @brief 复制原音频编码包并为输出容器挂载章节与 BPM。
/// @param inputPath 保持只读的原始音频路径。
/// @param outputPath 新容器的事务临时路径。
/// @param data 精确测量载荷，不因标准章节粒度而取整。
/// @param visible 有序外部时间点，包括可选整拍。
/// @param error 失败原因，调用方决定是否回退到配套文件。
/// @return 音频包和容器尾部均写入成功时返回 true。
/// @details 不建立解码器或编码器，原始压缩载荷按包直接移交。
/// 流编号映射只用于输出容器，输入对象不被修改。
/// 保留音频流、附属封面、流级语言和描述标签。
/// 旧 MP4 章节的数据轨不复制，使用新的章节表重建。
/// codec_tag 清零让 muxer 使用自身合法标识，不误带其他容器的标记。
/// packet 时间只做 time_base 换算，不改变实际播放秒数。
/// EOF 与读取错误严格区分，损坏来源不能被当作正常结束。
/// 输出私有标签是否保留由上层回读校验，不以 write_header 成功作为证明。
/// 所有上下文、选项与包都用 RAII 管理，早退仍关闭 IO。
/// @warning 离线 O(音频包数) 操作，必须在文件线程池执行。
bool remuxContainer(const std::filesystem::path&     inputPath,
                    const std::filesystem::path&     outputPath,
                    const AudioMarkerData&           data,
                    const std::vector<AudioChapter>& visible,
                    std::string&                     error)
{
    auto input = openInput(inputPath);
    if ( !input ) {
        error = "无法打开原始音频容器";
        return false;
    }
    AVFormatContext* raw         = nullptr;
    const auto       destination = Config::pathToUtf8(outputPath);
    auto             ext         = Config::pathToUtf8(outputPath.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    // 部分发布包仅启用 mp4 muxer，不能依赖 m4a 的扩展名推断。
    const char* format    = ext == ".m4a"                     ? "mp4"
                            : ext == ".mka"                   ? "matroska"
                            : ext == ".wma"                   ? "asf"
                            : ext == ".aif" || ext == ".aiff" ? "aiff"
                                                              : nullptr;
    const int   allocated = avformat_alloc_output_context2(
        &raw, nullptr, format, destination.c_str());
    std::unique_ptr<AVFormatContext, OutputCloser> output(raw);
    if ( allocated < 0 || !output ) {
        error = "此格式不支持内嵌标记";
        return false;
    }
    /// 初始 -1 表示没有复制的流，包循环据此过滤旧章节数据轨。
    /// 编号取 output 的真实 index，不能假定输入输出流数量相同。
    /// 来源音频的编码参数按值复制，不共享可写上下文。
    std::vector<int> streams(input->nb_streams, -1);
    for ( unsigned i = 0; i < input->nb_streams; ++i ) {
        const auto* source = input->streams[i];
        // 旧 MP4 章节轨由新的章节表重建，音频与附属封面则保留编码数据。
        if ( source->codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
             !(source->disposition & AV_DISPOSITION_ATTACHED_PIC) )
            continue;
        auto* target = avformat_new_stream(output.get(), nullptr);
        if ( !target ||
             avcodec_parameters_copy(target->codecpar, source->codecpar) < 0 ) {
            error = "复制音频流失败";
            return false;
        }
        streams[i]                  = target->index;
        target->codecpar->codec_tag = 0;
        target->time_base           = source->time_base;
        target->disposition         = source->disposition;
        av_dict_copy(&target->metadata, source->metadata, 0);
    }
    if ( output->nb_streams == 0 ) {
        error = "没有可复制的音频流";
        return false;
    }
    // 先复制来源全局标签，再写入当前测量信息。
    // 其他合法标签继续由输出 muxer 映射，不建立解码或滤镜图。
    // 流级标签在前面的 stream 构造中复制，避免仅保留全局标题。
    av_dict_copy(&output->metadata, input->metadata, 0);
    const auto payload = encodeData(data);
    clearChapterTags(&output->metadata);
    setPayload(&output->metadata, payload);
    const double duration =
        input->duration > 0
            ? static_cast<double>(input->duration) / AV_TIME_BASE
            : audioDuration(inputPath);
    if ( !setChapters(*output, visible, duration) ) {
        error = "章节分配失败";
        return false;
    }
    for ( unsigned i = 0; i < output->nb_streams; ++i ) {
        if ( output->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO )
            continue;
        // 流级旧章节同样需要清理，Ogg 的读取端优先使用流标签。
        clearChapterTags(&output->streams[i]->metadata);
        setPayload(&output->streams[i]->metadata, payload);
        // 将章节评论复制到 Ogg 的流级字典，保留原本的其他流标签。
        const AVDictionaryEntry* entry = nullptr;
        while (
            (entry = av_dict_get(
                 output->metadata, "CHAPTER", entry, AV_DICT_IGNORE_SUFFIX)) )
            av_dict_set(
                &output->streams[i]->metadata, entry->key, entry->value, 0);
        if ( !data.bpmSegments.empty() ) {
            const auto bpm = std::to_string(data.bpmSegments.front().bpm);
            av_dict_set(&output->streams[i]->metadata, "bpm", bpm.c_str(), 0);
            av_dict_set(&output->metadata, "bpm", bpm.c_str(), 0);
        }
    }
    /// 选项只影响输出容器的标签表示，不触发音频转码。
    /// ID3v2.3 面向常见 MP3 软件，CHAP 仍使用标准章节结构。
    /// MP4 的 mdta 模式保存普通 comment 及扩展标签，避免只保留白名单字段。
    Dictionary             options;
    const std::string_view muxer(output->oformat->name);
    if ( muxer == "mp3" || muxer == "aiff" )
        av_dict_set(&options.value, "id3v2_version", "3", 0);
    if ( muxer.find("mp4") != std::string_view::npos || muxer == "ipod" ||
         muxer == "mov" )
        av_dict_set(&options.value, "movflags", "use_metadata_tags", 0);
    // 负 packet 时间戳是编码器延迟，不能无意平移所有音频和章节。
    // 编码器可能用负时间戳表示预滚或延迟，不能强制平移为零。
    // 保持原始播放时钟有助于章节和首拍仍对应同一段声音。
    // 目标时基换算只改变整数刻度，不改变声音与秒数关系。
    output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    if ( !(output->oformat->flags & AVFMT_NOFILE) ) {
        const int opened =
            avio_open(&output->pb, destination.c_str(), AVIO_FLAG_WRITE);
        if ( opened < 0 ) {
            error = describeError(opened);
            return false;
        }
    }
    /// 写文件头之后 muxer 可能改变流时基。
    /// 包循环必须使用当前 target->time_base，而不是配置时的旧值。
    /// 若文件头写入失败，临时文件由上层清理，不进入发布流程。
    int status = avformat_write_header(output.get(), &options.value);
    if ( status < 0 ) {
        error = describeError(status);
        return false;
    }
    std::unique_ptr<AVPacket, PacketCloser> packet(av_packet_alloc());
    if ( !packet ) {
        error = "数据包分配失败";
        return false;
    }
    /// 一个可复用包覆盖整个复制过程，内存不随音频时长增长。
    /// 成功写入会移交包引用，显式 unref 也覆盖过滤和错误分支。
    /// 附属封面仍由对应视频流携带，不把它混入音频样本。
    while ( (status = av_read_frame(input.get(), packet.get())) >= 0 ) {
        const int sourceIndex = packet->stream_index;
        if ( sourceIndex < 0 ||
             static_cast<std::size_t>(sourceIndex) >= streams.size() ||
             streams[sourceIndex] < 0 ) {
            av_packet_unref(packet.get());
            continue;
        }
        const auto* source = input->streams[sourceIndex];
        auto*       target = output->streams[streams[sourceIndex]];
        // 官方 remux 流程：只换容器时钟，不修改 packet 编码载荷。
        av_packet_rescale_ts(
            packet.get(), source->time_base, target->time_base);
        // 流映射来自创建顺序，复制包不能沿用被过滤的数据轨编号。
        // 每个包只向同一音频编码流提交，不交给编码器重建。
        // 错误分支直接返回，包和 IO 在各自释放器中收尾。
        packet->stream_index = target->index;
        packet->pos          = -1;
        status = av_interleaved_write_frame(output.get(), packet.get());
        av_packet_unref(packet.get());
        if ( status < 0 ) {
            error = describeError(status);
            return false;
        }
    }
    // 真实读错误不能按 EOF 成功结束，否则会发布截短音频。
    // 只有正常文件结尾允许写出完整容器尾。
    // 磁盘读取失败可能留下部分包，不能按成功导出处理。
    // 临时文件是否清理由上层事务统一负责。
    if ( status != AVERROR_EOF ) {
        error = describeError(status);
        return false;
    }
    /// muxer 收尾负责章节索引、文件长度和内部缓冲刷新。
    /// 未成功收尾的文件不能用于精确元数据回读。
    /// RAII 析构还会关闭 AVIO，保证回读看到完整落盘输出。
    // 容器尾完整写出后，真正成功还需上层回读精确载荷。
    // 输出上下文析构关闭 IO，回读不会竞争尚未刷新的缓冲。
    // 重封装完成不代表所有外部播放器都识别扩展 BPM。
    status = av_write_trailer(output.get());
    if ( status < 0 ) {
        error = describeError(status);
        return false;
    }
    return true;
}
}  // namespace MMM::Audio::MarkerInternal
