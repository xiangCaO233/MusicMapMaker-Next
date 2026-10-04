#include "audio/AudioMarkerService.h"
#include "audio/AudioSpeedExportService.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace
{
/// @brief 将固定整数编码为小端，夹具不依赖宿主字节序。
/// @param output 测试 WAV 的二进制输出流。
/// @param value 固定位宽的无符号字段。
/// @details 逐字节输出保证大端平台也建立标准 RIFF 夹具。
/// 只用于夹具构造，不复用产品写出实现以避免同源错误。
/// 字段长度来自 T 的明确位宽，不依赖 long 的平台大小。
template<typename T> void little(std::ostream& output, T value)
{
    for ( std::size_t i = 0; i < sizeof(T); ++i )
        output.put(static_cast<char>((value >> (8 * i)) & 255));
}

/// @brief 建立非静音的双声道 48kHz PCM 夹具，完全写入构建输出目录。
/// @details 使用逐帧不同值，PCM 被替换或混入点击声时能通过包身份检测。
/// @param path CTest 隔离目录内的 WAV 文件位置。
/// @return 头和全部样本均写入成功时返回 true。
/// @details 双声道使用相反符号，使声道误合并不能被静音数据掩盖。
/// 采样率选择 48kHz，避免测试仅覆盖产品内部的固定解码率。
/// 样本数固定为两秒，整拍展开测试具有确定的停止边界。
/// RIFF 长度和字节率由实际帧布局计算。
/// 每次测试重新创建来源，旧测试标记不参与下一轮结果。
/// 输出流显式关闭后才查询状态，捕获缓冲刷新失败。
bool fixture(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    little<std::uint32_t>(output, 36 + 48000 * 2 * 4);
    output.write("WAVEfmt ", 8);
    little<std::uint32_t>(output, 16);
    little<std::uint16_t>(output, 1);
    little<std::uint16_t>(output, 2);
    little<std::uint32_t>(output, 48000);
    little<std::uint32_t>(output, 48000 * 4);
    little<std::uint16_t>(output, 4);
    little<std::uint16_t>(output, 16);
    output.write("data", 4);
    little<std::uint32_t>(output, 48000 * 2 * 4);
    for ( int i = 0; i < 96000; ++i ) {
        little<std::uint16_t>(output, static_cast<std::uint16_t>(i % 32000));
        little<std::uint16_t>(output, static_cast<std::uint16_t>(-i % 32000));
    }
    output.close();
    return static_cast<bool>(output);
}

/// @brief 读取测试文件的全部字节，用于检查来源及配套模式完全未改变。
/// @param path 待比较的夹具文件。
/// @return 文件字节快照，打不开时为空。
/// @details 对原格式配套模式比较完整文件身份。
/// 对内嵌模式只用于确认来源文件没有被修改。
/// 测试夹具体积有限，整文件读取不进入产品热路径。
/// 编码包比较使用另一函数，不能混淆容器头变化与音频变化。
std::string bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), {} };
}

/// @brief 抽取真实解复用后的音频编码载荷，验证无二次有损编码。
/// @details 不比较可变化的容器头、章节或编码器延迟标签。
/// @param path 来源或导出容器。
/// @return 顺序拼接的全部音频编码载荷。
/// @details 容器解析使用正式 FFmpeg 解复用，不能按文件后缀截取字节。
/// 只收集音频流，章节轨或封面不参与声音身份比较。
/// 所有读取失败都返回空载荷，调用方禁止把两个空结果视为相同音频。
/// 包引用每次读完立即释放，文件上下文在最后关闭。
/// 时基和章节改变可以合法改变封装，但不能改变编码载荷。
/// 原格式导出不应进入有损编码器，身份比较直接验证这一契约。
std::vector<unsigned char> packets(const std::filesystem::path& path)
{
    AVFormatContext*           input = nullptr;
    const auto                 name  = MMM::Config::pathToUtf8(path);
    std::vector<unsigned char> payload;
    if ( avformat_open_input(&input, name.c_str(), nullptr, nullptr) < 0 )
        return payload;
    /// 必须先取得真实流类型才能判断包是否属于声音。
    /// 读取失败返回空结果，由外层非空断言报告错误。
    /// 测试不依赖容器中的标题或编码器名称来确认格式。
    if ( avformat_find_stream_info(input, nullptr) >= 0 ) {
        auto* packet = av_packet_alloc();
        if ( packet ) {
            while ( av_read_frame(input, packet) >= 0 ) {
                if ( input->streams[packet->stream_index]
                         ->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ) {
                    payload.insert(payload.end(),
                                   packet->data,
                                   packet->data + packet->size);
                }
                av_packet_unref(packet);
            }
            av_packet_free(&packet);
        }
    }
    avformat_close_input(&input);
    return payload;
}

/// @brief 统计解复用器恢复的标准章节，不借助产品私有 JSON。
/// @param path 已经由正式服务导出的音频。
/// @return 成功时为章节数量，打开或探测失败时为负值。
/// @details Ogg 读取器会将 CHAPTER 评论消耗为 AVChapter，而不是保留原键。
/// 因此断言章节对象数量，不能把字典中没有原键误判为没有章节。
/// 来源必须有多章，新输出只有一章，覆盖缩短后旧尾部标签的清理。
/// 上下文只读，函数不会更改标记或重新编码声音。
/// 此读取路径与产品精确 JSON 恢复独立，不会掩盖标准标签的重复。
int standardChapterCount(const std::filesystem::path& path)
{
    AVFormatContext* input = nullptr;
    const auto       name  = MMM::Config::pathToUtf8(path);
    if ( avformat_open_input(&input, name.c_str(), nullptr, nullptr) < 0 )
        return -1;
    // 完成流探测后章节列表才完整，读取失败不能假装得到零章。
    const int count = avformat_find_stream_info(input, nullptr) >= 0
                          ? static_cast<int>(input->nb_chapters)
                          : -1;
    avformat_close_input(&input);
    return count;
}

/// @brief 比较导入的所有字段；不以成功标志代替真实内容验证。
/// @param read 正式读取服务的结果，不绕过解析入口。
/// @param expected 原始作者数据，包含高精度时间、中文名称和变速段。
/// @return 数量、顺序和双精度标量全部精确一致时返回 true。
/// @details 私有 JSON 往返使用精确比较，不能以粗略毫秒容差掩盖截断。
/// 章节和 BPM 单独计数，不能通过补齐默认段落伪造成功。
/// 标准 WAV cue 的采样率量化使用独立断言覆盖。
/// 失败时不访问缺失数组元素，避免测试自身越界。
bool matches(const MMM::Audio::AudioMarkerReadResult& read,
             const MMM::Audio::AudioMarkerData&       expected)
{
    if ( !read.success ||
         read.data.chapters.size() != expected.chapters.size() ||
         read.data.bpmSegments.size() != expected.bpmSegments.size() )
        return false;
    for ( std::size_t i = 0; i < expected.chapters.size(); ++i ) {
        if ( read.data.chapters[i].title != expected.chapters[i].title ||
             read.data.chapters[i].seconds != expected.chapters[i].seconds )
            return false;
    }
    for ( std::size_t i = 0; i < expected.bpmSegments.size(); ++i ) {
        if ( read.data.bpmSegments[i].seconds !=
                 expected.bpmSegments[i].seconds ||
             read.data.bpmSegments[i].bpm != expected.bpmSegments[i].bpm )
            return false;
    }
    return true;
}

/// @brief 失败时写出明确的检查名称，不引入异常或标准输出。
/// @param condition 本次行为断言。
/// @param name 可定位的断言名称或服务返回错误。
/// @return 原条件，方便汇总多个不依赖的失败场景。
/// @details 非短路汇总让每个格式都产生可检查的日志证据。
/// 必须继续通过成功标志控制后续读取，不能对失败结果盲目取字段。
/// 不把测试日志写入源码资源目录。
bool check(bool condition, const char* name)
{
    if ( !condition ) XERROR("Audio marker check failed: {}", name);
    return condition;
}
}  // namespace

/// @brief 检查真实容器往返、来源保护、标准 cue、Unicode 和坏标签处理。
/// @details 夹具由正式音频服务编码；只在当前预编译包没有编码器时跳过可选格式。
/// @param argc 必须包含独立输出目录参数。
/// @param argv argv[1] 指向构建树的测试输出根目录。
/// @return 所有必选能力满足契约时为零，任一失败为一。
/// @details 格式集合同时覆盖 RIFF、ID3、VorbisComment 和 MP4 容器。
/// WAV/MP3 是必选格式；其他格式仅因缺少夹具编码器可以跳过。
/// 能生成夹具后，正式标记服务的写出或回读失败不能跳过。
/// 内嵌成功还要求没有配套文件，防止标签丢弃由回退掩盖。
/// 原始 ADTS 没有可靠章节，必须通过完整字节复制和配套 JSON 往返。
/// 非法数据、别名路径、Unicode 名称和坏版本都有独立检查。
/// 文件线程池生命周期只属于本测试，停止时不会影响真实编辑器进程。
/// 测试从不打开声音设备，也不以人工听感判定音频是否被污染。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 2;
    XLogger::init("AudioMarkerServiceTest");
    MMM::Runtime::AppThreadPool::instance().init();
    const auto      root = MMM::Config::utf8ToPath(argv[1]);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if ( ec ) return 2;
    const auto source = root / "source.wav";
    if ( !fixture(source) ) return 2;
    // 来源快照在第一次导出前建立，覆盖整个测试过程的只读契约。
    // 后续来源路径别名场景不能重建快照，以免掩盖已发生的覆盖。
    // 所有夹具都位于 CTest 传入的输出目录，不访问用户真实音频。
    const auto original = bytes(source);
    // 非整数毫秒起点、中文章节及负首拍都必须由精确载荷保留。
    // 两个章节与两个 BPM 起点刻意不同，验证数据列表彼此独立。
    // BPM 首段起点在音频外，标准章节不能替代它的精确恢复。
    // 名称含引号和中文，验证序列化转义不会改变用户内容。
    const MMM::Audio::AudioMarkerData data{
        { { 0.123456789, "序章 \"intro\"" }, { 0.912345678, "副歌" } },
        { { -0.125, 100.123456 }, { 0.7, 180.987654 } }
    };
    MMM::Audio::AudioMarkerExportOptions options{
        source, root / "marked.wav", data, true
    };
    bool ok = true;
    for ( const auto* suffix : { "wav",
                                 "mp3",
                                 "flac",
                                 "ogg",
                                 "opus",
                                 "m4a",
                                 "wma",
                                 "aiff",
                                 "aac",
                                 "mka" } ) {
        // 每种编码建立独立来源，不能把输出后缀改名作为编码夹具。
        // WAV 直接复制已知 PCM，压缩格式通过正式编码服务建立。
        // 标记服务随后只做原格式写出，区分夹具编码与被测重封装。
        const auto input = root / (std::string("fixture.") + suffix);
        if ( std::string_view(suffix) == "wav" ) {
            std::filesystem::copy_file(
                source,
                input,
                std::filesystem::copy_options::overwrite_existing,
                ec);
        } else {
            MMM::Audio::AudioSpeedExportOptions encode;
            encode.inputPath  = source;
            encode.outputPath = input;
            // 一倍速且不变调，不让夹具生成引入待测的 DSP 状态。
            // 编码器可用性属于平台能力，编码失败必须在日志中标明格式。
            // 必选 MP3 编码失败仍使测试失败，不会被可选格式规则放过。
            encode.preservePitch = false;
            const auto encoded =
                MMM::Audio::AudioSpeedExportService::exportWav(encode);
            if ( !encoded.success ) {
                // 各平台包的编码能力可不同，但必选 WAV/MP3 不能被跳过。
                if ( std::string_view(suffix) == "mp3" )
                    ok &= check(false, encoded.errorMessage.c_str());
                XWARN("Optional marker fixture {} unavailable: {}",
                      suffix,
                      encoded.errorMessage);
                continue;
            }
        }
        // 冻结当前来源之后再导出，不从上一轮结果继续累积标签。
        // 每种格式都使用完全相同的精确 BPM 和章节快照。
        // 目标清理仅涉及本测试专属路径，不能清理输入文件。
        options.inputPath  = input;
        options.outputPath = root / (std::string("marked.") + suffix);
        std::filesystem::remove(options.outputPath, ec);
        auto companion = options.outputPath;
        companion += ".mmm-timing.json";
        std::filesystem::remove(companion, ec);
        // 源文件字节快照与解复用载荷快照承担不同契约。
        // 前者检查输入未被修改，后者检查输出没有重编码或混入音效。
        // 标记成功不意味着声音未变化，两个检查必须独立成立。
        const auto before      = bytes(input);
        const auto audioBefore = packets(input);
        const auto exported =
            MMM::Audio::AudioMarkerService::exportFile(options);
        ok &= check(exported.success, exported.error.c_str());
        /// 失败格式已计入最终结果，不继续读取可能不完整的输出。
        /// 每个成功格式同时验证标记精度、声音身份和来源保护。
        /// 只查看成功标志不足以证明元数据实际进入分发文件。
        if ( !exported.success ) continue;
        ok &= check(
            matches(MMM::Audio::AudioMarkerService::read(options.outputPath),
                    data),
            "exact chapters and BPM roundtrip");
        ok &= check(
            !audioBefore.empty() && packets(options.outputPath) == audioBefore,
            "encoded audio payload unchanged");
        ok &= check(bytes(input) == before, "source file unchanged");
        /// 配套模式要求音频本身逐字节不变。
        /// 常见内嵌格式则允许容器头变化，但上面的编码包必须相同。
        /// 两类断言明确区分无损重封装与未改动的文件复制。
        if ( std::string_view(suffix) == "aac" ) {
            ok &= check(!exported.sidecarPath.empty() &&
                            bytes(options.outputPath) == before,
                        "raw AAC uses identical copy and companion");
        } else if ( std::string_view(suffix) == "wav" ||
                    std::string_view(suffix) == "mp3" ||
                    std::string_view(suffix) == "flac" ||
                    std::string_view(suffix) == "ogg" ||
                    std::string_view(suffix) == "opus" ||
                    std::string_view(suffix) == "m4a" ) {
            // 主流容器必须真正在文件内嵌入，不可由回退模式掩盖错误。
            ok &= check(exported.sidecarPath.empty(),
                        "common format embeds markers");
        }
        XINFO("Marker roundtrip {}: {}",
              suffix,
              exported.sidecarPath.empty() ? "embedded" : "companion");
    }
    // Ogg 的旧标准章节评论必须替换，不只替换产品私有载荷。
    // 先前导出有多个章节和整拍，缩短到一章后不能残留 CHAPTER002。
    // 来源多章和输出单章断言同时验证探测成功，不把打开失败当成清理成功。
    if ( std::filesystem::exists(root / "marked.ogg", ec) ) {
        options.inputPath    = root / "marked.ogg";
        options.outputPath   = root / "shortened.ogg";
        options.data         = { { { 0.1, "only chapter" } }, {} };
        options.includeBeats = false;
        std::filesystem::remove(options.outputPath, ec);
        const auto shortened =
            MMM::Audio::AudioMarkerService::exportFile(options);
        ok &= check(
            shortened.success && standardChapterCount(options.inputPath) > 1 &&
                standardChapterCount(options.outputPath) == 1 &&
                matches(
                    MMM::Audio::AudioMarkerService::read(options.outputPath),
                    options.data),
            "re-export replaces standard chapter comments");
    }
    // 后续转换和覆盖保护场景继续使用完整的多段测量快照。
    // 不允许前面的简化章节夹具改变其覆盖的 BPM 与负首拍契约。
    options.data         = data;
    options.includeBeats = true;
    // 显式格式转换必须经过公开标记服务，而不只验证夹具编码器。
    // 两个方向均恢复精确测量数据，并检查来源文件没有被就地替换。
    for ( const auto* suffix : { "mp3", "wav" } ) {
        // 两种转换都通过标记导出服务的扩展名分支执行。
        // 不要求转换后编码包相同，有损转码合法改变声音编码。
        // 仍要求来源不变、声音流非空和精确元数据完整恢复。
        const bool toMp3   = std::string_view(suffix) == "mp3";
        options.inputPath  = toMp3 ? source : root / "fixture.mp3";
        options.outputPath = root / (std::string("converted.") + suffix);
        std::filesystem::remove(options.outputPath, ec);
        // 转换目标的旧配套文件也清理，避免回读优先级误命中上一次产物。
        // 这种清理只适用于隔离测试，不代表产品允许覆盖既有用户文件。
        // 正式服务始终拒绝已有目标或同名配套路径。
        auto sidecar = options.outputPath;
        sidecar += ".mmm-timing.json";
        std::filesystem::remove(sidecar, ec);
        const auto before = bytes(options.inputPath);
        const auto converted =
            MMM::Audio::AudioMarkerService::exportFile(options);
        ok &= check(
            converted.success && converted.sidecarPath.empty() &&
                matches(
                    MMM::Audio::AudioMarkerService::read(options.outputPath),
                    data) &&
                !packets(options.outputPath).empty() &&
                bytes(options.inputPath) == before,
            "explicit WAV MP3 conversion restores markers and protects source");
    }
    // 未知容器也允许保留原格式导出，关闭整拍后无需臆造时长。
    const auto opaque = root / "opaque.audio";
    {
        std::ofstream output(opaque, std::ios::binary);
        output << "unchanged opaque media";
    }
    options.inputPath    = opaque;
    options.outputPath   = root / "marked.audio";
    options.includeBeats = false;
    std::filesystem::remove(options.outputPath, ec);
    auto opaqueSidecar = options.outputPath;
    opaqueSidecar += ".mmm-timing.json";
    std::filesystem::remove(opaqueSidecar, ec);
    // 未知扩展名场景关闭整拍展开，服务不需要估计不存在的音频时长。
    // 输出以字节身份确认内容保留，标记只进入独立数据文件。
    // 该测试证明回退策略，不宣称未知编码能够被音频引擎播放。
    const auto retained = MMM::Audio::AudioMarkerService::exportFile(options);
    ok &= check(
        retained.success && !retained.sidecarPath.empty() &&
            bytes(opaque) == bytes(options.outputPath) &&
            matches(MMM::Audio::AudioMarkerService::read(options.outputPath),
                    data),
        "opaque original format uses companion without guessing duration");
    // 配套数据应随着项目导入的新名称一起迁移，不依赖可选编码器。
    const auto paired        = root / "marked.audio";
    const auto copiedAudio   = root / "project-copy.audio";
    auto       copiedSidecar = copiedAudio;
    copiedSidecar += ".mmm-timing.json";
    // 导入时目标音频可被资源系统重命名，配套键必须随之迁移。
    // 先删除本测试旧标记后建立首次导入，再单独测量冲突行为。
    // 复制来源是刚通过往返的配套输出，不能人工伪造成功标记。
    std::filesystem::remove(copiedSidecar, ec);
    std::filesystem::copy_file(
        paired,
        copiedAudio,
        std::filesystem::copy_options::overwrite_existing,
        ec);
    ok &= check(
        !ec &&
            !MMM::Audio::AudioMarkerService::copyCompanion(paired,
                                                           copiedAudio) &&
            matches(MMM::Audio::AudioMarkerService::read(copiedAudio), data),
        "renamed imported audio retains companion markers");
    // 记录已导入的数据再触发第二次复制，检查冲突不会覆盖它。
    // 错误码必须明确为 file_exists，不接受未知错误作为保护成功。
    // 音频副本及原标记的所有权仍属于调用方，服务不能顺手清理。
    const auto savedSidecar = bytes(copiedSidecar);
    ok &= check(MMM::Audio::AudioMarkerService::copyCompanion(
                    paired, copiedAudio) == std::errc::file_exists &&
                    bytes(copiedSidecar) == savedSidecar,
                "companion import never replaces existing metadata");
    options.includeBeats = true;
    options.inputPath    = source;
    // 直接把来源选为目标的请求必须在创建临时文件前拒绝。
    // 随后来源字节比较确保拒绝没有先截断文件再返回失败。
    // 该保护独立于 UI 默认 -marked 名称，直接服务调用同样安全。
    options.outputPath = source;
    ok &= check(!MMM::Audio::AudioMarkerService::exportFile(options).success &&
                    bytes(source) == original,
                "source overwrite rejected");
    // 硬链接也指向原始音频，禁止通过另一个文件名破坏来源。
    const auto alias = root / "alias.wav";
    std::filesystem::remove(alias, ec);
    // 硬链接在文件系统支持时建立，覆盖不同名称指向同一 inode 的情况。
    // 不支持硬链接的平台仅跳过这一别名夹具，不跳过直接覆盖保护。
    // 原文件早已存在，正式导出也必须拒绝覆盖任何既有目标。
    std::filesystem::create_hard_link(source, alias, ec);
    if ( !ec ) {
        options.outputPath = alias;
        ok &=
            check(!MMM::Audio::AudioMarkerService::exportFile(options).success,
                  "hard link rejected");
    }
    /// Windows 使用原生宽路径，中文文件名不能只在 JSON 中覆盖。
    /// 这一场景同时验证文件服务和格式读取器的路径转换。
    /// 文件名的空格也必须经由路径 API 保留。
    const auto unicode = root / MMM::Config::utf8ToPath("中文 标记.wav");
    std::filesystem::remove(unicode, ec);
    options.outputPath = unicode;
    ok &=
        check(MMM::Audio::AudioMarkerService::exportFile(options).success &&
                  matches(MMM::Audio::AudioMarkerService::read(unicode), data),
              "Unicode filename");
    options.outputPath = root / "invalid.wav";
    // NaN 无法代表可恢复 BPM，不能被 JSON 库写成 null 后伪装成功。
    // 校验失败应先于目标文件创建，目录中不得遗留半成品。
    // 随后恢复合法 DTO，避免后面的 WAV 行为被这一坏输入影响。
    options.data.bpmSegments[0].bpm = std::numeric_limits<double>::quiet_NaN();
    ok &= check(!MMM::Audio::AudioMarkerService::exportFile(options).success &&
                    !std::filesystem::exists(options.outputPath),
                "invalid BPM produces no output");
    options.data = data;
    // 将私有块变为 JUNK，验证外部软件的标准 cue/labl 仍可独立读取。
    auto       wave = bytes(unicode);
    const auto tag  = wave.find("mmmt");
    ok &= check(tag != std::string::npos, "WAV private metadata present");
    // 只替换四字节类型，RIFF 长度、对齐和其他块都仍合法。
    // 外部标准读取测试因此明确区分无私有块与整个音频损坏。
    // 测试不删除 cue 和名称标签，正式解析必须读取它们。
    if ( tag != std::string::npos ) wave.replace(tag, 4, "JUNK");
    const auto standard = root / "standard.wav";
    {
        std::ofstream output(standard, std::ios::binary);
        output.write(wave.data(), wave.size());
    }
    /// 移除私有块后仍需能被通用 cue 读取，不可依赖产品扩展。
    /// 标准 cue 使用样本位置，误差上限只能是一帧。
    /// 整拍选项应产生比命名章节更多的标准标记。
    const auto imported = MMM::Audio::AudioMarkerService::read(standard);
    ok &= check(imported.success &&
                    imported.data.chapters.size() > data.chapters.size(),
                "standard cue includes beat positions");
    ok &= check(std::any_of(imported.data.chapters.begin(),
                            imported.data.chapters.end(),
                            [](const auto& chapter) {
                                return chapter.title == "副歌" &&
                                       std::abs(chapter.seconds - 0.912345678) <
                                           1.0 / 48000;
                            }),
                "standard cue label and sample position");
    // 重新导出标记 WAV 不能积累第二个 cue 块。
    options.inputPath  = unicode;
    options.outputPath = root / "second.wav";
    std::filesystem::remove(options.outputPath, ec);
    /// 重新标记属于替换旧测量信息，不应追加互相矛盾的 cue 表。
    /// 输入本身带章节时同样需要保持来源只读。
    /// 输出必须仍可解复用，长度回写错误会被正式服务的回读发现。
    const auto second = MMM::Audio::AudioMarkerService::exportFile(options);
    const auto twice  = bytes(options.outputPath);
    const auto cue    = twice.find("cue ");
    ok &= check(second.success && cue != std::string::npos &&
                    twice.find("cue ", cue + 4) == std::string::npos,
                "single cue after re-export");
    // 坏版本化标签必须报告失败，不能回退到臆造的默认 BPM。
    // 坏版本仅修改协议版本字符，音频样本和 RIFF 结构保持完整。
    // 这样失败原因来自测量数据的不支持版本，而非声音无法解码。
    // 即使标准 cue 存在，私有块损坏也不能静默退化为丢失 BPM。
    const auto payload = wave.find("\"version\":1");
    if ( tag != std::string::npos ) wave.replace(tag, 4, "mmmt");
    if ( payload != std::string::npos ) wave[payload + 10] = '9';
    const auto corrupt = root / "corrupt.wav";
    {
        std::ofstream output(corrupt, std::ios::binary);
        output.write(wave.data(), wave.size());
    }
    ok &= check(!MMM::Audio::AudioMarkerService::read(corrupt).success,
                "unknown metadata version rejected");
    /// 所有同步导出均已结束，再关闭后台解码所用线程池。
    /// 最终退出码来自完整行为集合，日志中出现可选编码器警告不代表失败。
    /// 夹具保留在构建目录，便于失败时复核容器和配套数据。
    MMM::Runtime::AppThreadPool::instance().shutdown();
    return ok ? 0 : 1;
}
