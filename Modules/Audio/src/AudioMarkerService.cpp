#include "audio/AudioMarkerService.h"

#include "AudioMarkerInternal.h"
#include "audio/AudioSpeedExportService.h"
#include "config/Utf8Path.h"
#include "mmm/timing/BpmNormalization.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <system_error>

namespace MMM::Audio
{
namespace MarkerInternal
{
/// @brief 校验外部时间边界，避免秒数换算整数时溢出。
/// @details 允许负首拍，但限制在可安全保存为纳秒的范围内。
static bool validSeconds(double value)
{
    return std::isfinite(value) && std::abs(value) <= 1e9;
}

/// @brief 验证标记 DTO 的数量、字符串、时间和 BPM 不变量。
/// @param data 尚未序列化或刚从外部读取的测量数据。
/// @return 所有字段均可安全进入文件格式时返回 true。
/// @details 不排序、不去重、不钳位，保证校验不会改变来源语义。
/// 章节可以不按时间排列，命名列表按原顺序往返。
/// BPM 段必须严格递增，重复起点不具有确定的节拍区间。
/// 负首拍是合法的测量锚点，不能因为标准章节不能表示它就删除。
/// 章节名称不能包含内嵌零，因为 FFmpeg 和 RIFF 标签使用零结尾文本。
/// 文本总预算预留 Unicode 转义和 JSON 字段的膨胀空间。
/// 数值必须有限，避免 JSON null 或整数位置溢出掩盖原始错误。
/// @warning 只在后台文件边界调用，不参与每帧绘制校验。
bool validData(const AudioMarkerData& data)
{
    // 数量预算也用于导入，不能只限制导出的标准章节列表。
    if ( data.chapters.size() > MAX_MARKERS ||
         data.bpmSegments.size() > MAX_MARKERS )
        return false;
    /// 文本总量预算按编码前字节计数。
    /// 后续 ASCII 转义最多扩大每个 Unicode 单元的表示。
    /// 保守预留八倍空间后，版本化载荷仍受统一读取预算约束。
    std::size_t textBytes = 0;
    for ( const auto& chapter : data.chapters ) {
        textBytes += chapter.title.size();
        if ( textBytes > MAX_METADATA_BYTES / 8 ) return false;
        // 字符串上限保护 JSON、ID3 和 WAV 标签三个写出路径。
        if ( !validSeconds(chapter.seconds) || chapter.title.size() > 4096 ||
             chapter.title.find('\0') != std::string::npos )
            return false;
    }
    /// 从时间下界之外开始比较，允许合法的负首拍锚点。
    /// 只验证 BPM 顺序，章节顺序由作者保留。
    /// 重复或倒序 BPM 会让同一音频位置对应多个拍长，不能静默修复。
    double previous = -1e10;
    for ( const auto& segment : data.bpmSegments ) {
        // 相同时间的两段 BPM 无法确定拍线来源，必须明确拒绝。
        if ( !validSeconds(segment.seconds) || segment.seconds <= previous ||
             !std::isfinite(segment.bpm) || segment.bpm < MIN_NORMALIZED_BPM ||
             segment.bpm > MAX_NORMALIZED_BPM )
            return false;
        previous = segment.seconds;
    }
    return true;
}

/// @brief 将经过验证的测量快照序列化为版本一载荷。
/// @param data 调用方持有的不可变章节和 BPM 快照。
/// @return 不带容器边界标记的紧凑 JSON 文本。
/// @details 时间以秒为单位保存，double 的往返不依赖容器章节的毫秒粒度。
/// 章节列表和 BPM 列表独立，允许存在没有 BPM 的普通章节。
/// ASCII 转义只改变文件表示，不改变恢复后的 Unicode 名称。
/// 替换非法 UTF-8 字节可避免第三方备注触发异常路径。
/// 调用方通过回读比较载荷，识别实际编码替换导致的数据变化。
/// 容器 comment 的边界包装由重封装实现负责。
/// WAV 私有块和配套文件直接保存本函数产生的结构。
std::string encodeData(const AudioMarkerData& data)
{
    nlohmann::json json = { { "version", 1 },
                            { "chapters", nlohmann::json::array() },
                            { "bpm_segments", nlohmann::json::array() } };
    for ( const auto& chapter : data.chapters ) {
        json["chapters"].push_back(
            { { "seconds", chapter.seconds }, { "title", chapter.title } });
    }
    for ( const auto& segment : data.bpmSegments ) {
        json["bpm_segments"].push_back(
            { { "seconds", segment.seconds }, { "bpm", segment.bpm } });
    }
    // 精确 double 与 ASCII Unicode 转义一起保存，不依赖容器原有的文本编码。
    return json.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
}

/// @brief 在没有异常机制的条件下解析精确测量载荷。
/// @param text 私有块或标签边界内的完整 JSON。
/// @param data 成功时替换的输出 DTO，失败时保持原值。
/// @return 结构、版本和所有标量校验均成功时返回 true。
/// @details parse 的 allow_exceptions 为 false，不接受部分语法解析。
/// 先检查根对象、版本和数组，再访问嵌套字段。
/// 每个 get 前验证 JSON 类型，避免类型转换异常。
/// 数量预算在 vector 扩容前检查，文本预算在解析前检查。
/// 使用临时 DTO 完成校验后提交，坏 BPM 不留下已读章节。
/// 未知版本被拒绝，不能按当前字段猜测未来版本的单位。
/// 配套文件的音频大小字段由上层验证，不属于测量数据本身。
bool decodeData(std::string_view text, AudioMarkerData& data)
{
    // 非异常解析配合逐字段类型检查，坏标签不能终止音频导入流程。
    if ( text.size() > MAX_METADATA_BYTES ) return false;
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if ( !json.is_object() || !json.contains("version") ||
         json["version"] != 1 || !json.contains("chapters") ||
         !json["chapters"].is_array() || !json.contains("bpm_segments") ||
         !json["bpm_segments"].is_array() ||
         json["chapters"].size() > MAX_MARKERS ||
         json["bpm_segments"].size() > MAX_MARKERS )
        return false;
    /// 所有临时数组都属于本次解析，失败无需撤回调用方修改。
    /// 外部标签可能只包含章节或只包含 BPM，空数组本身是合法结构。
    /// 字段类型与数值范围分开检查，兼容整数形式保存的秒数和 BPM。
    AudioMarkerData parsed;
    for ( const auto& chapter : json["chapters"] ) {
        if ( !chapter.is_object() || !chapter.contains("seconds") ||
             !chapter["seconds"].is_number() || !chapter.contains("title") ||
             !chapter["title"].is_string() )
            return false;
        parsed.chapters.push_back({ chapter["seconds"].get<double>(),
                                    chapter["title"].get<std::string>() });
    }
    for ( const auto& segment : json["bpm_segments"] ) {
        if ( !segment.is_object() || !segment.contains("seconds") ||
             !segment["seconds"].is_number() || !segment.contains("bpm") ||
             !segment["bpm"].is_number() )
            return false;
        parsed.bpmSegments.push_back(
            { segment["seconds"].get<double>(), segment["bpm"].get<double>() });
    }
    if ( !validData(parsed) ) return false;
    // 只有完整验证后替换调用方容器，不留下部分恢复的 BPM。
    data = std::move(parsed);
    return true;
}

/// @brief 将精确测量数据展开为外部软件可见的有序标记。
/// @param options 用户冻结的测量数据和整拍导出开关。
/// @param duration 原始音频的实际时长，单位秒。
/// @param chapters 成功时包含章节起点、BPM 锚点和可选整拍。
/// @return 展开后的数量不超过协议预算时返回 true。
/// @details 每段的整拍区间在下一 BPM 起点结束，不延用上一段拍长。
/// 负首拍仍决定拍相位，但文件开头之前的可见拍点被跳过。
/// 时间通过首拍加整数拍号乘拍长计算，不累加浮点间隔。
/// 提前计算数量，不能先分配无限拍点再裁切。
/// 私有载荷保存精确锚点，标准章节只保存音频内的非负点。
/// 同一位置的用户命名优先于自动 BPM 标签和整拍标签。
/// 使用稳定排序保留同位置名称的优先级。
/// 整拍只是外部显示数据，导回工具时由 BPM 段重新生成网格。
/// @warning 仅离线展开，禁止在频谱或波形每帧路径调用。
bool makeVisibleChapters(const AudioMarkerExportOptions& options,
                         double duration, std::vector<AudioChapter>& chapters)
{
    /// 先加入用户章节以确定相同起点的名称优先级。
    /// 生成标记不会写回 options，也不会扩大私有载荷中的章节列表。
    /// 这样整拍导出开关只影响外部显示，不改变测量模型。
    chapters = options.data.chapters;
    for ( std::size_t i = 0; i < options.data.bpmSegments.size(); ++i ) {
        const auto& segment = options.data.bpmSegments[i];
        // 标准章节不能表示负时间；精确私有载荷仍保留负首拍。
        if ( segment.seconds >= 0.0 && segment.seconds <= duration ) {
            chapters.push_back(
                { segment.seconds, "BPM " + std::to_string(segment.bpm) });
        }
        if ( !options.includeBeats ) continue;
        const double end =
            i + 1 < options.data.bpmSegments.size()
                ? std::min(duration, options.data.bpmSegments[i + 1].seconds)
                : duration;
        const double step  = 60.0 / segment.bpm;
        const double first = std::max(0.0, std::ceil(-segment.seconds / step));
        const double count =
            std::max(0.0, std::ceil((end - segment.seconds) / step) - first);
        if ( count > static_cast<double>(
                         MAX_MARKERS - std::min(MAX_MARKERS, chapters.size())) )
            return false;
        // 按整数拍号计算位置，避免连续累加拍长造成漂移。
        for ( std::size_t n = 0; n < static_cast<std::size_t>(count); ++n ) {
            const double beat = first + static_cast<double>(n);
            chapters.push_back(
                { segment.seconds + beat * step,
                  "Beat " +
                      std::to_string(static_cast<std::uint64_t>(beat) + 1) });
        }
    }
    std::erase_if(chapters, [duration](const auto& chapter) {
        return chapter.seconds < 0.0 || chapter.seconds > duration;
    });
    // 用户命名章节先插入，稳定排序和去重保留名称而不是生成的 BPM 标签。
    std::stable_sort(
        chapters.begin(), chapters.end(), [](const auto& a, const auto& b) {
            return a.seconds < b.seconds;
        });
    chapters.erase(std::unique(chapters.begin(),
                               chapters.end(),
                               [](const auto& a, const auto& b) {
                                   return std::abs(a.seconds - b.seconds) <
                                          1e-7;
                               }),
                   chapters.end());
    return chapters.size() <= MAX_MARKERS;
}

/// @brief 产生与音频扩展名绑定的配套标记路径。
/// @param audio 原始或导出的完整音频路径。
/// @return 在完整文件名后追加 .mmm-timing.json 的路径。
/// @details 不替换音频扩展名，因此同主体不同编码可独立携带标记。
/// 输入使用 filesystem::path，Windows 中文路径保持原生宽字符。
/// 配套文件必须与对应音频一起移动或分享。
/// 命名不包含项目绝对根目录，跨机器复制不依赖旧项目布局。
std::filesystem::path sidecarPath(const std::filesystem::path& audio)
{
    // 使用完整文件名，song.wav 与 song.mp3 的配套标记不会混用。
    auto result = audio;
    result += ".mmm-timing.json";
    return result;
}
}  // namespace MarkerInternal

namespace
{
/// @brief 文件事务临时路径的所有者，失败时清理但永不删除来源。
/// @details 事务成功后路径已经 rename，析构的 remove 对不存在路径无害。
struct StagedFiles {
    /// @brief 本事务创建的文件，按值持有并在离线出口释放。
    std::vector<std::filesystem::path> paths;
    /// @brief 失败、取消或早退时清理临时输出。
    ~StagedFiles()
    {
        for ( const auto& path : paths ) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }
};

/// @brief 根据最终路径产生同目录临时名，保持扩展名供编码器选择格式。
static std::filesystem::path stagingPath(
    const std::filesystem::path& destination)
{
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return destination.parent_path() /
           Config::utf8ToPath(Config::pathToUtf8(destination.stem()) +
                              ".mmm-export-" + std::to_string(stamp) +
                              Config::pathToUtf8(destination.extension()));
}

/// @brief 统一扩展名大小写，不依赖当前用户区域设置。
static std::string extension(const std::filesystem::path& path)
{
    auto value = Config::pathToUtf8(path.extension());
    for ( auto& c : value )
        if ( c >= 'A' && c <= 'Z' ) c = static_cast<char>(c - 'A' + 'a');
    return value;
}
}  // namespace

/// @brief 从一份音频恢复章节和精确 BPM 数据。
/// @param path 音频路径，读取时不修改音频或项目设置。
/// @return 成功无标记与损坏标记通过 success 区分。
/// @details 配套文件优先，因它用于无法可靠保存私有标签的容器。
/// 存在配套文件但损坏时明确失败，不能假装没有标记。
/// 配套文件绑定音频大小，防止最常见的同名资源替换误配。
/// WAV 优先解析 cue 和私有 RIFF 块，不依赖 FFmpeg 的章节映射。
/// 其他格式读取全局和音频流标签，再读取标准章节。
/// 版本化载荷优先于通用 BPM 标签，避免精度损失和变速段遗漏。
/// 普通无标记文件成功返回空列表，调用方可继续手工或自动测量。
/// @warning 后台低频文件读取，禁止因 UI 每帧显示章节而重复调用。
AudioMarkerReadResult AudioMarkerService::read(
    const std::filesystem::path& path)
{
    using namespace MarkerInternal;
    std::error_code ec;
    const auto      companion = sidecarPath(path);
    if ( std::filesystem::exists(companion, ec) && !ec ) {
        // 配套载荷读取预算必须在建立字符串前检查。
        const auto size = std::filesystem::file_size(companion, ec);
        if ( ec || size > MAX_METADATA_BYTES )
            return { false, "标记文件过大或不可读取", {} };
        // 输入先完成大小预算检查再分配缓冲，目录和特殊文件不会当作配套数据。
        // 读取仅持有本次局部流，成功数据通过值语义交付 UI。
        std::ifstream         input(companion, std::ios::binary);
        const std::string     text((std::istreambuf_iterator<char>(input)), {});
        AudioMarkerReadResult result;
        result.success = input.good() && decodeData(text, result.data);
        // 以音频大小绑定配套文件，防止常见的替换音频后误读旧标记。
        const auto json = nlohmann::json::parse(text, nullptr, false);
        if ( result.success && json.contains("audio_size") ) {
            const auto audioSize = std::filesystem::file_size(path, ec);
            result.success =
                !ec && json["audio_size"].is_number_unsigned() &&
                json["audio_size"].get<std::uint64_t>() == audioSize;
        }
        if ( !result.success )
            result.error = "标记文件损坏、版本不支持或与音频不匹配";
        return result;
    }
    // WAV cue 的读取不依赖解码器是否把 cue 暴露为 AVChapter。
    if ( extension(path) == ".wav" || extension(path) == ".wave" ) {
        auto wave = readWave(path);
        if ( wave.success || !wave.error.empty() ) return wave;
    }
    return readContainer(path);
}

/// @brief 将经过校验的音频配套数据迁移到资源导入的新路径。
/// @details 音频复制由调用方负责；这里不删除文件、不覆盖同名标记。
/// 来源没有配套文件时不会启动容器探测，普通导入保持原有成本。
/// 完整音频文件名是绑定键，重命名资源时必须同步重命名配套文件。
/// @warning 用户导入的低频文件操作，不得在每帧资源扫描中调用。
std::error_code AudioMarkerService::copyCompanion(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    std::error_code error;
    const auto      companion = MarkerInternal::sidecarPath(source);
    if ( !std::filesystem::exists(companion, error) ) return error;
    // 损坏或绑定到另一音频的配套数据不能混入新项目。
    if ( !read(source).success )
        return std::make_error_code(std::errc::invalid_argument);
    // 沿用不覆盖复制语义，既有项目标记的生命周期属于用户。
    std::filesystem::copy_file(
        companion, MarkerInternal::sidecarPath(destination), error);
    return error;
}

/// @brief 以新文件事务导出音频标记，保持来源只读。
/// @param options 输入路径、目标路径、测量快照和整拍选项。
/// @return 成功时 sidecarPath 指明是否需要携带配套文件。
/// @details 规范路径及已有文件检查先于任何目标文件创建。
/// 来源与目标相同、已有硬链接或符号链接目标均不会被截断。
/// 同格式路径只复制编码包，WAV 则直接复制原始 RIFF 音频块。
/// 不同扩展名只允许用户显式选择 WAV 或 MP3 进行转换。
/// 转换使用恒定一倍速且没有变调，不插入提示音或节拍声。
/// 临时文件与目标同目录，完成后使用 rename 发布。
/// 容器写出后先回读精确数据，容器丢弃标签必须触发回退。
/// 回退复制音频字节，并把精确数据写入版本化配套文件。
/// 配套文件也完成回读后，才与音频一起进入发布流程。
/// 失败不发布部分音频，临时文件由作用域所有者清理。
/// 输入音频即使来自另一次标记导出，也不会被就地更新。
/// @warning 后台事务可读写完整音频，不能在 UI 线程执行。
AudioMarkerExportResult AudioMarkerService::exportFile(
    const AudioMarkerExportOptions& options)
{
    using namespace MarkerInternal;
    AudioMarkerExportResult result;
    if ( !validData(options.data) || options.inputPath.empty() ||
         options.outputPath.empty() ) {
        result.error = "章节或 BPM 数据无效";
        return result;
    }
    std::error_code ec;
    // 同文件、硬链接、符号链接以及尚不存在的规范同路径都必须拒绝。
    const auto source =
        std::filesystem::weakly_canonical(options.inputPath, ec);
    if ( ec || !std::filesystem::is_regular_file(source, ec) || ec ) {
        result.error = "无法读取原始音频";
        return result;
    }
    const auto destination =
        std::filesystem::weakly_canonical(options.outputPath, ec);
    if ( ec || source == destination ||
         std::filesystem::exists(destination, ec) || ec ||
         std::filesystem::exists(sidecarPath(destination), ec) || ec ) {
        // 新文件事务从不截断已有文件，包括原始音频和配套文件。
        result.error = "请使用尚不存在的新文件名，不能覆盖原始音频或已有导出";
        return result;
    }
    /// 此前检查已确保最终文件未存在。
    /// 下面的临时文件只属于当前导出，不复用来源所在的文件名。
    /// 临时名保留最终扩展名，使编码器和 muxer 使用正确容器。
    StagedFiles staged;
    const auto  encoded = stagingPath(destination);
    const auto  tagged =
        encoded.parent_path() /
        Config::utf8ToPath(Config::pathToUtf8(encoded.stem()) + "-marked" +
                           Config::pathToUtf8(encoded.extension()));
    staged.paths                = { encoded, tagged, sidecarPath(tagged) };
    std::filesystem::path input = source;
    /// 比较扩展名采用固定 ASCII 大小写归一化。
    /// 同格式时不能因为重新挂载标签而经过音频 DSP 图。
    /// 显式不同格式时才允许进入既有离线编码服务。
    const bool converting = extension(source) != extension(destination);
    if ( converting ) {
        // 只在用户明确选择 WAV/MP3 时转码，原格式路径绝不重新编码。
        if ( extension(destination) != ".wav" &&
             extension(destination) != ".mp3" ) {
            result.error = "转换格式仅支持 WAV 或 MP3；其他格式请保留原扩展名";
            return result;
        }
        AudioSpeedExportOptions conversion;
        conversion.inputPath     = source;
        conversion.outputPath    = encoded;
        conversion.preservePitch = false;
        const auto converted = AudioSpeedExportService::exportWav(conversion);
        if ( !converted.success ) {
            result.error = converted.errorMessage;
            return result;
        }
        input = encoded;
    }
    const double              duration = audioDuration(input);
    std::vector<AudioChapter> visible;
    if ( (options.includeBeats && !(duration > 0.0)) ||
         (duration > 0.0 &&
          !makeVisibleChapters(options, duration, visible)) ) {
        result.error = "音频时长无效或整拍标记超过 65536 个，请关闭整拍导出";
        return result;
    }
    /// WAV 由块复制实现，其 fmt、位深及声道信息保持来源值。
    /// 压缩容器由重封装实现，原编码包的负时间戳也保留。
    /// 两种实现都必须把失败原因交回事务层。
    bool embedded = false;
    if ( !(duration > 0.0) ) {
        // 未知容器不能凭空推算整拍；关闭整拍时仍可原样复制并配套保存。
        result.error = "容器时长未知，使用配套标记";
    } else if ( extension(destination) == ".wav" ||
                extension(destination) == ".wave" ) {
        embedded =
            writeWave(input, tagged, options.data, visible, result.error);
    } else {
        embedded =
            remuxContainer(input, tagged, options.data, visible, result.error);
    }
    // 写入后必须回读精确载荷；不把容器悄悄丢掉标签当成成功。
    /// 回读必须在临时输出 IO 已关闭后执行。
    /// 比较完整载荷可以发现容器截断 comment、忽略标签或修改精确值。
    /// 成功创建音频文件本身不代表标记导出成功。
    if ( embedded ) {
        const auto restored = read(tagged);
        embedded = restored.success &&
                   encodeData(restored.data) == encodeData(options.data);
    }
    if ( !embedded ) {
        // 不可靠的元数据容器保留原编码字节，配套数据不会污染波形。
        std::filesystem::remove(tagged, ec);
        ec.clear();
        std::filesystem::copy_file(
            input, tagged, std::filesystem::copy_options::none, ec);
        if ( ec ) {
            result.error = "复制音频失败: " + ec.message();
            return result;
        }
        // 这里的 JSON 来自已校验 DTO，不接受外部路径或可执行表达式。
        // 音频大小在临时副本落盘后绑定，避免使用转码前的来源大小。
        // 配套文件不是声音轨道，生成过程不触碰 PCM 或编码包。
        auto json =
            nlohmann::json::parse(encodeData(options.data), nullptr, false);
        json["audio_size"] = std::filesystem::file_size(tagged, ec);
        if ( ec ) {
            result.error = ec.message();
            return result;
        }
        std::ofstream companion(sidecarPath(tagged), std::ios::binary);
        companion << json.dump(
            -1, ' ', true, nlohmann::json::error_handler_t::replace);
        companion.close();
        if ( !companion ) {
            result.error = "标记文件写入失败";
            return result;
        }
        const auto restored = read(tagged);
        if ( !restored.success ||
             encodeData(restored.data) != encodeData(options.data) ) {
            result.error = "导出标记回读校验失败";
            return result;
        }
    }
    // 回退路径中的临时音频和标记已经完整回读校验。
    // 两次重命名期间不会有修改原音频的操作。
    // 若最终发布失败，只删除本次发布的配套路径。
    // 发布配套文件后再发布音频，失败时回收刚发布的配套文件。
    if ( !embedded ) {
        result.sidecarPath = sidecarPath(destination);
        std::filesystem::rename(sidecarPath(tagged), result.sidecarPath, ec);
        if ( ec ) {
            result.error = ec.message();
            return result;
        }
    }
    std::filesystem::rename(tagged, destination, ec);
    if ( ec ) {
        if ( !embedded ) {
            std::error_code cleanup;
            std::filesystem::remove(result.sidecarPath, cleanup);
        }
        result.error = ec.message();
        return result;
    }
    /// 只有所有发布步骤完成后才清除先前内嵌失败的回退原因。
    /// 用户依据 sidecarPath 判断内嵌或配套模式，而非依据旧错误字符串。
    /// success 的设置是最后一步，不能在音频写完但标记未写完时提前设置。
    result.error.clear();
    result.success = true;
    return result;
}
}  // namespace MMM::Audio
