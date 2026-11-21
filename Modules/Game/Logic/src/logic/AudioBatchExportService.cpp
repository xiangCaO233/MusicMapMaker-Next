#include "logic/AudioBatchExportService.h"

#include "config/Utf8Path.h"
#include "logic/ProjectResourceService.h"
#include "logic/ProjectStorage.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BmsTextEncoding.h"
#include "mmm/project/PackageFileTypes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace MMM::Logic
{
namespace
{
/// @brief 输入规范路径到已提交输出的对应表。
using AudioPaths = std::map<std::filesystem::path, std::filesystem::path>;

/// @brief 将文件身份规范为绝对路径，解析失败时保留词法路径。
std::filesystem::path canonicalPath(const std::filesystem::path& path)
{
    std::error_code error;
    auto            resolved = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : resolved;
}

/// @brief 转为谱面通用路径分隔符，不改变音频文件名大小写。
std::filesystem::path referencePath(std::string text)
{
    std::replace(text.begin(), text.end(), '\\', '/');
    return Config::utf8ToPath(text);
}

/// @brief 只对协议标签和后缀使用 ASCII 大小写折叠。
std::string lowerAscii(std::string value)
{
    for ( auto& ch : value ) {
        if ( ch >= 'A' && ch <= 'Z' ) ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return value;
}

/// @brief 导出使用的临时目录；删除范围只属于这次成功创建的目录。
struct StagingDirectory {
    /// @brief 编码器和谱面改写器共享的暂存目录。
    std::filesystem::path m_path;
    /// @brief 退出时清理已关闭的临时文件。
    ~StagingDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }
};

/// @brief RM 加载器识别的同前缀音频，按查找优先级排列。
constexpr std::array<std::string_view, 7> IMD_AUDIO_EXTENSIONS{
    ".mp3", ".wav", ".ogg", ".flac", ".opus", ".aac", ".m4a"
};

/// @brief 检查此 IMD 前缀是否已被可能自动匹配的音频或封面占用。
/// @param candidate 将要写出的音频路径。
/// @param error 查询失败时保留系统诊断。
/// @return 已有媒体或查询失败时为真，调用方区分避让与错误。
bool imdPrefixOccupied(const std::filesystem::path& candidate,
                       std::error_code&             error)
{
    const auto existsWith = [&](std::string_view extension) {
        auto path = candidate;
        path.replace_extension(Config::utf8ToPath(std::string(extension)));
        return std::filesystem::exists(path, error) || error;
    };
    // 一组难度共用这个前缀；任何旧封面都不能被新谱面误识别。
    return std::ranges::any_of(IMD_AUDIO_EXTENSIONS, existsWith) ||
           existsWith(".png") || existsWith(".jpg") || existsWith(".jpeg");
}

/// @brief 将完整暂存文件复制到空闲文件名，已有文件始终保留。
/// @param staged 已完整关闭的编码或谱面文件。
/// @param preferred 目标首选名称。
/// @param error 除重名外的复制或探测错误。
/// @param imdPrefix 是否同时为隐式伴随资源保留整个文件名前缀。
/// @return 实际写入的路径，失败时为空。
/// @note copy_file 的 no-overwrite 行为也处理两个独立任务同时争用名字。
std::filesystem::path copyToFreePath(const std::filesystem::path& staged,
                                     const std::filesystem::path& preferred,
                                     std::error_code&             error,
                                     bool imdPrefix = false)
{
    auto candidate = preferred;
    for ( unsigned suffix = 2;; ++suffix ) {
        if ( imdPrefix && imdPrefixOccupied(candidate, error) ) {
            if ( error ) return {};
            error = std::make_error_code(std::errc::file_exists);
        } else if ( std::filesystem::copy_file(
                        staged,
                        candidate,
                        std::filesystem::copy_options::none,
                        error) )
            return candidate;
        if ( error != std::errc::file_exists ) return {};
        // 编号在扩展名前附加，保持容器和谱面类型可识别。
        candidate =
            preferred.parent_path() /
            Config::utf8ToPath(Config::pathToUtf8(preferred.stem()) + " (" +
                               std::to_string(suffix) + ")" +
                               Config::pathToUtf8(preferred.extension()));
    }
}

/// @brief 查找包含谱面的项目根；没有配置时按普通目录解析路径。
Project loadOwningProject(const std::filesystem::path& chart)
{
    auto directory = chart.parent_path();
    while ( !directory.empty() ) {
        if ( ProjectStorage::hasProjectConfiguration(directory) ) {
            auto stored                    = ProjectStorage{}.load(directory);
            stored.m_project.m_projectRoot = directory;
            return std::move(stored.m_project);
        }
        if ( directory == directory.root_path() ) break;
        directory = directory.parent_path();
    }
    Project project;
    project.m_projectRoot = chart.parent_path();
    return project;
}

/// @brief 去掉字段外围空白，返回原行中的非拥有视图。
std::string_view trimmed(std::string_view text)
{
    while ( !text.empty() &&
            std::isspace(static_cast<unsigned char>(text.front())) )
        text.remove_prefix(1);
    while ( !text.empty() &&
            std::isspace(static_cast<unsigned char>(text.back())) )
        text.remove_suffix(1);
    return text;
}

/// @brief 不展开谱面事件，只改写 BMS 声明表中的外部音频路径。
/// @note KEY ZIP 内的 WAV 声明引用音包条目，整曲 BGM 则仍可独立替换。
bool rewriteBms(
    std::string& source,
    const std::function<std::optional<std::string>(const std::string&)>&
          resolve,
    bool& changed)
{
    bool myO2    = false;
    bool hasBank = false;
    // 同一份定义中 #KEYSOUND 可能出现在 #WAV 之后，先确定命名空间。
    for ( std::size_t start = 0; start < source.size(); ) {
        const auto end = source.find('\n', start);
        const auto line =
            trimmed(std::string_view(source).substr(start, end - start));
        const auto key =
            lowerAscii(std::string(line.substr(0, line.find_first_of(" \t"))));
        myO2 |= key == "#audio_mode" || key == "#bgm" || key == "#keysound";
        hasBank |= key == "#keysound";
        if ( end == std::string::npos ) break;
        start = end + 1;
    }
    auto decoded = Bms::decodeBmsText(std::move(source), myO2);
    if ( !decoded ) return false;
    const auto& text = *decoded;
    source.clear();
    source.reserve(text.size());
    for ( std::size_t start = 0; start < text.size(); ) {
        const auto end     = text.find('\n', start);
        const auto line    = std::string_view(text).substr(start, end - start);
        const auto content = trimmed(line);
        const auto separator = content.find_first_of(" \t");
        auto       key = lowerAscii(std::string(content.substr(0, separator)));
        if ( key.starts_with("\xef\xbb\xbf") ) key.erase(0, 3);
        const bool wav = key.size() == 6 && key.starts_with("#wav") &&
                         std::isalnum(static_cast<unsigned char>(key[4])) &&
                         std::isalnum(static_cast<unsigned char>(key[5]));
        if ( separator == std::string_view::npos ||
             (key != "#bgm" && (!wav || hasBank)) ) {
            source.append(line);
        } else {
            auto field = trimmed(content.substr(separator));
            if ( field.size() >= 2 && field.front() == '"' &&
                 field.back() == '"' ) {
                field.remove_prefix(1);
                field.remove_suffix(1);
            }
            const auto replacement = resolve(std::string(field));
            if ( replacement && replacement->empty() ) return false;
            if ( replacement && *replacement != field ) {
                // 保留原引号、空格、换行和同一小节内的所有原始事件。
                const auto offset =
                    static_cast<std::size_t>(field.data() - line.data());
                source.append(line.substr(0, offset));
                source.append(*replacement);
                source.append(line.substr(offset + field.size()));
                changed = true;
            } else {
                source.append(line);
            }
        }
        if ( end == std::string::npos ) break;
        source.push_back('\n');
        start = end + 1;
    }
    return true;
}

/// @brief 只在 MMM 或 Malody 声明的字段内替换音频引用。
/// @note 在已有 JSON 对象上更新，不重建物件和来源扩展元数据。
bool rewriteJson(
    std::string& source, bool native,
    const std::function<std::optional<std::string>(const std::string&)>&
          resolve,
    bool& changed)
{
    auto doc = nlohmann::ordered_json::parse(source, nullptr, false);
    if ( !doc.is_object() ) return false;
    bool       unrepresentable = false;
    const auto field           = [&](auto& object, const char* key) {
        const auto item = object.find(key);
        if ( item == object.end() || !item->is_string() ) return;
        const auto& old         = item->template get_ref<const std::string&>();
        const auto  replacement = resolve(old);
        if ( replacement && replacement->empty() ) {
            unrepresentable = true;
        } else if ( replacement && *replacement != old ) {
            *item   = *replacement;
            changed = true;
        }
    };
    if ( native ) {
        if ( auto metadata = doc.find("metadata"); metadata != doc.end() ) {
            if ( auto base = metadata->find("base"); base != metadata->end() ) {
                field(*base, "song_file_hint");
                field(*base, "audio");
            }
        }
        if ( auto samples = doc.find("audio_samples");
             samples != doc.end() && samples->is_array() )
            for ( auto& sample : *samples ) field(sample, "audio_ref");
    } else if ( auto metadata = doc.find("meta"); metadata != doc.end() ) {
        if ( auto song = metadata->find("song"); song != metadata->end() )
            field(*song, "file");
    }

    const auto nativeBinding = [&](auto& note) {
        field(note, "bound_sound");
        if ( auto sample = note.find("sample"); sample != note.end() )
            field(*sample, "audio_ref");
    };
    if ( auto notes = doc.find("note");
         notes != doc.end() && notes->is_array() ) {
        for ( auto& note : *notes ) {
            if ( !native ) {
                field(note, "sound");
                continue;
            }
            nativeBinding(note);
            if ( auto nodes = note.find("sub_notes");
                 nodes != note.end() && nodes->is_array() )
                for ( auto& node : *nodes ) nativeBinding(node);
        }
    }
    if ( changed && !unrepresentable ) source = doc.dump(2) + '\n';
    return !unrepresentable;
}

/// @brief 记录单项失败并保留源文件身份供插件状态显示。
void addError(AudioBatchExportResult& result, const std::filesystem::path& path,
              std::string_view message)
{
    result.m_errors.push_back(Config::pathToUtf8(path) + "：" +
                              std::string(message));
}

/// @brief 根据 IMD 实际加载的前缀音频找到本次生成的对应输出。
/// @note 没有显式路径字段，需要整体复制谱面，并让新前缀关联音频与封面。
bool copyImd(const std::filesystem::path& chart, const AudioPaths& audioPaths,
             AudioBatchExportResult& result)
{
    const auto map = BeatMap::loadFromFile(chart);
    if ( map.m_baseMapMetadata.map_path.empty() ) return false;
    const auto original = map.m_baseMapMetadata.song_file_hint;
    const auto converted =
        audioPaths.find(canonicalPath(chart.parent_path() / original));
    if ( original.empty() || converted == audioPaths.end() ) {
        ++result.m_chartsUnchanged;
        return true;
    }
    const auto& audio     = converted->second;
    const auto  extension = Config::pathToUtf8(audio.extension());
    if ( std::ranges::find(IMD_AUDIO_EXTENSIONS, extension) ==
         IMD_AUDIO_EXTENSIONS.end() )
        return false;
    const auto prefix   = Config::pathToUtf8(audio.stem());
    const auto filename = Config::pathToUtf8(chart.filename());
    if ( filename.find('_') == std::string::npos ||
         prefix.find('_') != std::string::npos )
        return false;
    // 音频输出此前缀若已有其他格式，IMD 可能按优先级绑定到错误文件。
    for ( const auto ext :
          { ".mp3", ".wav", ".ogg", ".flac", ".opus", ".aac", ".m4a" } ) {
        std::error_code error;
        if ( extension != ext &&
             std::filesystem::exists(
                 audio.parent_path() / Config::utf8ToPath(prefix + ext),
                 error) )
            return false;
    }
    std::error_code error;
    const auto      cover = map.m_baseMapMetadata.main_cover_path;
    if ( !cover.empty() ) {
        const auto destination =
            audio.parent_path() /
            Config::utf8ToPath(prefix + Config::pathToUtf8(cover.extension()));
        std::filesystem::copy_file(chart.parent_path() / cover,
                                   destination,
                                   std::filesystem::copy_options::skip_existing,
                                   error);
        if ( error ) return false;
    }
    const auto preferred =
        audio.parent_path() /
        Config::utf8ToPath(prefix + filename.substr(filename.find('_')));
    if ( copyToFreePath(chart, preferred, error).empty() ) return false;
    ++result.m_chartsWritten;
    return true;
}

/// @brief 写出同目录的显式引用谱面副本，保留其余相对资源的基准。
bool copyChart(const std::filesystem::path& chart,
               const std::filesystem::path& stage, const AudioPaths& audioPaths,
               AudioBatchExportResult& result)
{
    const auto extension = lowerAscii(Config::pathToUtf8(chart.extension()));
    if ( extension == ".imd" ) return copyImd(chart, audioPaths, result);
    if ( !isKnownPackageResourceExtension(PackageResourceType::Beatmap,
                                          extension) )
        return false;
    std::error_code readError;
    constexpr auto  MAX_BMS_BYTES = 64U * 1024U * 1024U;
    if ( extension != ".osu" && extension != ".mmm" && extension != ".mc" &&
         std::filesystem::file_size(chart, readError) > MAX_BMS_BYTES )
        return false;
    std::ifstream stream(chart, std::ios::binary);
    if ( !stream ) return false;
    std::string source(std::istreambuf_iterator<char>(stream), {});
    if ( stream.bad() ) return false;

    const bool native  = extension == ".mmm";
    const auto parent  = chart.parent_path();
    const auto project = native ? loadOwningProject(chart) : Project{};
    // MMM 的 ID 会从项目资源表解析；缺少 ID 表时仍支持路径型引用。
    std::map<std::string, const std::filesystem::path*> ids;
    if ( native ) {
        for ( const auto& resource : project.m_audioResources ) {
            const auto match = audioPaths.find(canonicalPath(
                project.m_projectRoot / referencePath(resource.m_path)));
            if ( match != audioPaths.end() )
                ids.try_emplace(resource.m_id, &match->second);
        }
    }
    const auto base = native ? project.m_projectRoot : parent;
    std::map<std::string, std::optional<std::string>> cache;
    const auto                                        resolve =
        [&](const std::string& text) -> std::optional<std::string> {
        if ( text.empty() ) return std::nullopt;
        auto [cached, inserted] = cache.try_emplace(text);
        if ( !inserted ) return cached->second;
        const auto ref = referencePath(text);
        const auto id  = ids.find(text);
        // 优先按本格式的相对基准匹配，不用同名文件猜测资源身份。
        const auto  path   = audioPaths.find(canonicalPath(base / ref));
        const auto* target = id != ids.end()            ? id->second
                             : path != audioPaths.end() ? &path->second
                                                        : nullptr;
        if ( !target && native && base != parent ) {
            const auto relative = audioPaths.find(canonicalPath(parent / ref));
            if ( relative != audioPaths.end() ) target = &relative->second;
        }
        if ( target ) {
            const auto relative = target->lexically_relative(base);
            // MMM 能表达跨卷路径；外部格式仅替换能表达为相对路径的文件。
            cached->second = relative.empty() && native
                                 ? Config::pathToUtf8Generic(*target)
                                 : Config::pathToUtf8Generic(relative);
            // 文本格式没有通用引号和换行转义，非法路径不能写入声明行。
            if ( !native && extension != ".mc" &&
                 cached->second->find_first_of("\r\n\"") != std::string::npos )
                cached->second = std::string{};
        }
        return cached->second;
    };

    bool changed = false;
    if ( extension == ".osu" ) {
        std::string rewritten;
        if ( !rewriteOsuAudioReferences(source, resolve, rewritten, changed) )
            return false;
        source = std::move(rewritten);
    } else if ( extension == ".mc" || native ) {
        if ( !rewriteJson(source, native, resolve, changed) ) return false;
    } else if ( !rewriteBms(source, resolve, changed) ) {
        return false;
    }
    if ( !changed ) {
        ++result.m_chartsUnchanged;
        return true;
    }
    std::ofstream rewritten(stage, std::ios::binary | std::ios::trunc);
    rewritten.write(source.data(), static_cast<std::streamsize>(source.size()));
    rewritten.close();
    if ( !rewritten ) return false;
    auto            preferred = parent / Config::utf8ToPath(
                                             Config::pathToUtf8(chart.stem()) + " (转码)" +
                                             Config::pathToUtf8(chart.extension()));
    std::error_code error;
    if ( copyToFreePath(stage, preferred, error).empty() ) return false;
    ++result.m_chartsWritten;
    return true;
}
}  // namespace

/// @brief 执行一批独立转码，然后使用成功输出作为谱面改链的唯一映射来源。
/// @warning 后台文件任务，不能从 UI 渲染路径同步调用。
AudioBatchExportResult exportAudioBatch(AudioBatchExportRequest request)
{
    AudioBatchExportResult result;
    std::error_code        error;
    const auto             outputRoot = canonicalPath(request.m_directory);
    if ( request.m_inputs.empty() || request.m_directory.empty() ||
         !isKnownPackageResourceExtension(PackageResourceType::Audio,
                                          request.m_extension) ||
         !std::filesystem::is_directory(outputRoot, error) ) {
        addError(result,
                 request.m_directory,
                 "请选择输入文件、编码格式和有效输出目录");
        return result;
    }
    if ( !request.m_charts.empty() && request.m_options.speed != 1.0 ) {
        // 替换路径不会改变谱面时间，只有 1 倍速能维持物件与采样间的原有同步。
        addError(result, outputRoot, "同步谱面引用时倍速必须为 1");
        return result;
    }
    const auto staging =
        outputRoot /
        (".mmm-audio-export-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    if ( !std::filesystem::create_directory(staging, error) ) {
        addError(result, outputRoot, "无法创建转码暂存目录");
        return result;
    }
    const StagingDirectory          cleanup{ staging };
    AudioPaths                      converted;
    std::set<std::filesystem::path> seen, imdInputs;
    for ( const auto& chart : request.m_charts ) {
        if ( lowerAscii(Config::pathToUtf8(chart.extension())) != ".imd" )
            continue;
        const auto map = BeatMap::loadFromFile(chart);
        if ( !map.m_baseMapMetadata.song_file_hint.empty() )
            imdInputs.insert(canonicalPath(
                chart.parent_path() / map.m_baseMapMetadata.song_file_hint));
    }
    const auto  originalProgress = request.m_options.progressCallback;
    const auto  count = request.m_inputs.size() + request.m_charts.size();
    std::size_t index = 0;
    // 进度按项目计数；单项失败也完成该阶段，后续有效文件仍继续。
    const auto progress = [&](float value, std::string_view label) {
        if ( originalProgress )
            originalProgress({ .progress = (static_cast<float>(index) + value) /
                                           static_cast<float>(count),
                               .message  = std::string(label) });
    };
    request.m_options.outputPath =
        staging / Config::utf8ToPath("audio" + request.m_extension);
    request.m_options.progressCallback = [&](const auto& item) {
        progress(item.progress, item.message);
    };
    for ( const auto& value : request.m_inputs ) {
        const auto input = canonicalPath(value);
        if ( seen.insert(input).second ) {
            request.m_options.inputPath = input;
            const auto item =
                Audio::AudioSpeedExportService::exportWav(request.m_options);
            if ( item.success ) {
                auto       stem      = Config::pathToUtf8(value.stem());
                const bool imdPrefix = imdInputs.contains(input);
                if ( imdPrefix ) stem += " (转码)";
                const auto preferred =
                    outputRoot / Config::utf8ToPath(stem + request.m_extension);
                auto output = copyToFreePath(
                    request.m_options.outputPath, preferred, error, imdPrefix);
                if ( output.empty() )
                    addError(result, input, "无法写入音频：" + error.message());
                else {
                    ++result.m_audioWritten;
                    converted.emplace(input, std::move(output));
                }
            } else {
                addError(result, input, item.errorMessage);
            }
            // 每项使用相同的暂存名，失败留下的半成品必须先清理再开始下一项。
            std::filesystem::remove(request.m_options.outputPath, error);
        }
        ++index;
    }
    seen.clear();
    for ( const auto& chart : request.m_charts ) {
        const auto input = canonicalPath(chart);
        if ( seen.insert(input).second &&
             !copyChart(input, staging / "chart", converted, result) )
            addError(result, chart, "无法写出保持音频关联的谱面副本");
        ++index;
        progress(0.0F, "正在同步谱面引用");
    }
    return result;
}
}  // namespace MMM::Logic
