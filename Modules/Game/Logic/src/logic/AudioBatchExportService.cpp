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

/// @file AudioBatchExportService.cpp
/// @brief 批量转码源音频，按格式保存引用已完成输出的谱面副本。
/// @details
/// 两个阶段通过源文件身份与实际制品路径相连；前置排重、命名避让和编码失败
/// 均在发布谱面前发生。路径型引用按文件身份匹配，不用基名猜测；
/// 资源 ID 只在拥有该谱面的项目内解释。
/// @note 某个输入失败不会取消其他项目。结果保留文件级诊断与成功计数，
/// 因此调用方可以显示部分成功，并用报告定位未改链的谱面。
namespace MMM::Logic
{
namespace
{
/// @brief 输入规范路径到已提交输出的对应表。
using AudioPaths = std::map<std::filesystem::path, std::filesystem::path>;

// 相同磁盘文件可以由不同的目录拼写或符号链接到达。对仍可解析的路径
// 统一使用系统身份，后续正选与引用检索才能使用同一个键。
/// @brief 将文件身份规范为绝对路径，解析失败时保留词法路径。
std::filesystem::path canonicalPath(const std::filesystem::path& path)
{
    std::error_code error;
    // 输入可能尚未被验证。weakly_canonical 先解析已存在的路径前缀；
    // 探测错误时保留供诊断的原始词法身份，由真正读取文件的入口处理失败。
    auto resolved = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : resolved;
}

/// @brief 转为谱面通用路径分隔符，不改变音频文件名大小写。
std::filesystem::path referencePath(std::string text)
{
    // 部分旧谱面把反斜杠写作路径分隔符，跨系统读入时需要兼容。
    // 只规范分隔，不对文件名做大小写折叠，以保留区分大小写文件系统中的身份。
    std::replace(text.begin(), text.end(), '\\', '/');
    return Config::utf8ToPath(text);
}

/// @brief 只对协议标签和后缀使用 ASCII 大小写折叠。
std::string lowerAscii(std::string value)
{
    // 约定中的标签是固定 ASCII；用户文件名和路径使用独立的 UTF-8 转换。
    // 按字节只改 ASCII 大写，不让当前区域设置改变扩展名白名单的结果。
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
        // 暂存目录只有成功创建后才注册清理，正式制品在这个树之外。
        // 清理是尽力操作；某个临时文件仍被外部进程占用时不改变已返回的业务结果。
        std::filesystem::remove_all(m_path, error);
    }
};

/// @brief RM 加载器识别的同前缀音频，按查找优先级排列。
/// @details 音频目标命名必须同时避开所有这些后缀，因为 IMD 通过前缀探测，
/// 扩展名不同的文件也可能成为更优先的匹配项。
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
    // 用同一候选基名逐个换扩展，保持目录身份不变。
    // 将探测错误也视为占用以尽早停止后缀枚举，将失败留给调用方报告。
    const auto existsWith = [&](std::string_view extension) {
        // 校验错误会留在 error_code；逻辑短路避免后一次存在性查询覆盖前一诊断。
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
        // IMD 预留的是可解析的命名前缀，文本谱面只需避免目标文件冲突。
        // 两类冲突走相同的编号策略，但探测错误应立即结束，不能用下一个编号掩盖。
        if ( imdPrefix && imdPrefixOccupied(candidate, error) ) {
            if ( error ) return {};
            error = std::make_error_code(std::errc::file_exists);
        } else if ( std::filesystem::copy_file(
                        staged,
                        candidate,
                        std::filesystem::copy_options::none,
                        error) )
            return candidate;
        // 普通目标由 copy_file 自身判断重名，避免先查存在再覆盖的窗口。
        // 仅重名可重试；权限与介质错误不应触发无限换名。
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
    // 从谱面目录向上搜索，嵌套项目必须绑定最近那份资源 ID 表。
    // 不能从应用当前打开项目借用映射，因为批处理可以跨多个工程。
    auto directory = chart.parent_path();
    while ( !directory.empty() ) {
        // 最近配置是资源 ID 的作用域。上层工程可能也包含同名 ID，
        // 但该 ID 在当前谱面中的含义必须由最内层项目决定。
        if ( ProjectStorage::hasProjectConfiguration(directory) ) {
            auto stored                    = ProjectStorage{}.load(directory);
            stored.m_project.m_projectRoot = directory;
            return std::move(stored.m_project);
        }
        // 根目录的 parent_path 可能返回根目录本身，必须显式终止。
        // 无配置时由谱面目录解释相对引用，与独立谱面打开行为一致。
        if ( directory == directory.root_path() ) break;
        directory = directory.parent_path();
    }
    Project project;
    project.m_projectRoot = chart.parent_path();
    return project;
}

/// @brief 去掉字段外围空白，返回原行中的非拥有视图。
/// @return 与输入共享原始缓冲的子区间，调用方可以计算字段在原行的偏移。
/// @note 将字节提升为 unsigned char 后才调用字符分类，兼容高位字节。
std::string_view trimmed(std::string_view text)
{
    // 只移动视图边界，调用者可以按原字节范围重建引用行。
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
/// @param source 输入可能为旧编码，成功时成为解码并局部改写的文本。
/// @param resolve 空 optional 保留旧路径，空字符串表示新路径不可表达。
/// @param changed 有实际字段替换时标记，防止只因解码就输出新副本。
/// @return 解码和全部参与改链的字段都可处理时为真。
bool rewriteBms(
    std::string& source,
    const std::function<std::optional<std::string>(const std::string&)>&
          resolve,
    bool& changed)
{
    // 初始化两个独立探测位，分别约束文本解码和样本路径解析。
    bool myO2    = false;
    bool hasBank = false;
    // 同一份定义中 #KEYSOUND 可能出现在 #WAV 之后，先确定命名空间。
    for ( std::size_t start = 0; start < source.size(); ) {
        const auto end = source.find('\n', start);
        const auto line =
            trimmed(std::string_view(source).substr(start, end - start));
        const auto key =
            lowerAscii(std::string(line.substr(0, line.find_first_of(" \t"))));
        // MyO2 声明决定沿用哪组旧编码规则；音包标记独立决定 WAV 值的解释，
        // 不能因发现 BGM 声明就把同一谱面的全部 WAV 字段当成音包成员。
        myO2 |= key == "#audio_mode" || key == "#bgm" || key == "#keysound";
        hasBank |= key == "#keysound";
        if ( end == std::string::npos ) break;
        start = end + 1;
    }
    // 先完成文本解码，再对 UTF-8 字段调用统一路径解析器。
    // 输出另写回 source；本轮切出的原行视图仍由 decoded 持有。
    auto decoded = Bms::decodeBmsText(std::move(source), myO2);
    if ( !decoded ) return false;
    const auto& text = *decoded;
    source.clear();
    source.reserve(text.size());
    for ( std::size_t start = 0; start < text.size(); ) {
        // 行内容保留原始空白，另用去除边缘空白的视图识别键。
        // 这让错落排版的传统 BMS 保持原样，又能识别键后的实际样本名。
        const auto end     = text.find('\n', start);
        const auto line    = std::string_view(text).substr(start, end - start);
        const auto content = trimmed(line);
        const auto separator = content.find_first_of(" \t");
        auto       key = lowerAscii(std::string(content.substr(0, separator)));
        // BOM 只从用于判型的键上移除，原行仍用于拼接。
        // 声明必须包含完整两位 WAV 编号；时间轴事件、注释与标题不参加音频改链。
        if ( key.starts_with("\xef\xbb\xbf") ) key.erase(0, 3);
        const bool wav = key.size() == 6 && key.starts_with("#wav") &&
                         std::isalnum(static_cast<unsigned char>(key[4])) &&
                         std::isalnum(static_cast<unsigned char>(key[5]));
        if ( separator == std::string_view::npos ||
             (key != "#bgm" && (!wav || hasBank)) ) {
            source.append(line);
        } else {
            // 外层引号用于声明语法；只把里面的路径值传给解析器。
            // 路径包含空白时也按整个字段查找，不能继续按空白拆成多个词。
            auto field = trimmed(content.substr(separator));
            if ( field.size() >= 2 && field.front() == '"' &&
                 field.back() == '"' ) {
                field.remove_prefix(1);
                field.remove_suffix(1);
            }
            const auto replacement = resolve(std::string(field));
            // 不可表达必须使整份谱面写入失败，避免只换部分已成功转码的引用。
            // 未命中映射则保留原引用，因为该资源可能没列入请求，或该项转码失败。
            if ( replacement && replacement->empty() ) return false;
            if ( replacement && *replacement != field ) {
                // 用字段视图相对原行的字节偏移拼接，避免替换这一行内其他同名字符串。
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
        // 按输入是否存在行结束符逐行重建，避免补写原来不存在的最后换行。
        // 被视为行内容的回车字符也随原行拼接保留。
        source.push_back('\n');
        start = end + 1;
    }
    return true;
}

/// @brief 只在 MMM 或 Malody 声明的字段内替换音频引用。
/// @note 在已有 JSON 对象上更新，不重建物件和来源扩展元数据。
/// @param native 区分 MMM 的项目资源键和 Malody 的本地文件名。
/// @param resolve 空 optional 表示未命中，空值字符串表示路径无法写入。
/// @param changed 新副本需要真实引用变化，单纯重新排版不计入。
/// @return 根必须是 JSON 对象，且没有命中但不可表示的路径。
bool rewriteJson(
    std::string& source, bool native,
    const std::function<std::optional<std::string>(const std::string&)>&
          resolve,
    bool& changed)
{
    // 保留其余键的顺序和未知字段，对嵌套数组仅探测既有字段。
    // 错误输入使用显式失败结果；上层据此跳过写出，不构造看似有效的空对象。
    auto doc = nlohmann::ordered_json::parse(source, nullptr, false);
    if ( !doc.is_object() ) return false;
    bool       unrepresentable = false;
    const auto field           = [&](auto& object, const char* key) {
        // 字段不存在或不是字符串时保持原样，避免给旧谱面补造字段。
        // 同一字段只由调用点按协议命名，文本中偶然出现的路径不会被全文替换。
        const auto item = object.find(key);
        if ( item == object.end() || !item->is_string() ) return;
        const auto& old         = item->template get_ref<const std::string&>();
        const auto  replacement = resolve(old);
        // 先累计不可表示标记，整份 JSON 只有所有引用都安全时才发布。
        // 扫描期间 source 保留原正文，局部 DOM 修改可以随失败结果一并丢弃。
        if ( replacement && replacement->empty() ) {
            unrepresentable = true;
        } else if ( replacement && *replacement != old ) {
            *item   = *replacement;
            changed = true;
        }
    };
    // MMM 的作品声明和时间轴采样是不同层次，都可引用已转码的主音频。
    // Malody 的曲目信息放在 meta.song，下方再独立处理物件声音。
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
        // Malody 字段是本地文件名，不进入 MMM 项目资源 ID 表。
        if ( auto song = metadata->find("song"); song != metadata->end() )
            field(*song, "file");
    }

    // 同一 MMM 播放绑定可能来自旧平面字段或新采样对象。
    // 主音符和折线子节点都沿用这套结构；业务描述字段保持原样。
    const auto nativeBinding = [&](auto& note) {
        field(note, "bound_sound");
        if ( auto sample = note.find("sample"); sample != note.end() )
            field(*sample, "audio_ref");
    };
    if ( auto notes = doc.find("note");
         notes != doc.end() && notes->is_array() ) {
        for ( auto& note : *notes ) {
            // 旧版整数样本与其他音符共享 Malody 的 sound
            // 字段，由字段存在性判断。
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
    // 只有已发生语义变化才序列化。没有命中时上层应记录为未改写，
    // 从而不为格式化、空白和换行差异生成额外输出。
    if ( changed && !unrepresentable ) source = doc.dump(2) + '\n';
    return !unrepresentable;
}

/// @brief 记录单项失败并保留源文件身份供插件状态显示。
void addError(AudioBatchExportResult& result, const std::filesystem::path& path,
              std::string_view message)
{
    // 路径和诊断使用同一条记录，重复的底层错误仍可按实际输入定位。
    result.m_errors.push_back(Config::pathToUtf8(path) + "：" +
                              std::string(message));
}

/// @brief 根据 IMD 实际加载的前缀音频找到本次生成的对应输出。
/// @note 没有显式路径字段，需要整体复制谱面，并让新前缀关联音频与封面。
/// @param chart 原始二进制谱面，其文件名承担格式元数据的一部分。
/// @param audioPaths 本批已经发布且可作为新谱面伴随资源的音频。
/// @param result 仅成功提交或确实无需变化时累计对应计数。
/// @return 谱面无需变化或完整复制成功时为真。
bool copyImd(const std::filesystem::path& chart, const AudioPaths& audioPaths,
             AudioBatchExportResult& result)
{
    // 用正式解析器找出加载后实际匹配到的音频，避免和主程序
    // 对前缀规则、扩展名优先级或大小写的判断不一致。
    const auto map = BeatMap::loadFromFile(chart);
    if ( map.m_baseMapMetadata.map_path.empty() ) return false;
    const auto original = map.m_baseMapMetadata.song_file_hint;
    const auto converted =
        audioPaths.find(canonicalPath(chart.parent_path() / original));
    // 无音频或所用音频未成功导出时，原谱面仍是有效输入，
    // 计为无需改写；只有能构成完整新关联时才写出副本。
    if ( original.empty() || converted == audioPaths.end() ) {
        ++result.m_chartsUnchanged;
        return true;
    }
    const auto& audio     = converted->second;
    const auto  extension = Config::pathToUtf8(audio.extension());
    // 输出容器合法并不等于 IMD 加载器会按前缀识别它；这里只允许其识别集合。
    if ( std::ranges::find(IMD_AUDIO_EXTENSIONS, extension) ==
         IMD_AUDIO_EXTENSIONS.end() )
        return false;
    const auto prefix   = Config::pathToUtf8(audio.stem());
    const auto filename = Config::pathToUtf8(chart.filename());
    // IMD 以首个下划线为前缀边界，新音频名自身不能引入这一分隔符。
    // 余下的原文件名仍携带键数和难度；在新前缀后完整保留。
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
    // 封面同样靠前缀推导，应与新音频和谱面副本相邻。
    // 已有目标可能来自同曲的另一难度；复用该伴随文件，避免覆盖首次写入。
    const auto cover = map.m_baseMapMetadata.main_cover_path;
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
    // 只有音频绑定和封面准备完成后才放入 IMD 副本。
    // 复用通用非覆盖发布，可保留用户已存在的同难度版本。
    const auto preferred =
        audio.parent_path() /
        Config::utf8ToPath(prefix + filename.substr(filename.find('_')));
    if ( copyToFreePath(chart, preferred, error).empty() ) return false;
    ++result.m_chartsWritten;
    return true;
}

/// @brief 写出同目录的显式引用谱面副本，保留其余相对资源的基准。
/// @param chart 待写出副本的源谱面。
/// @param stage 本批复用的暂存正文路径。
/// @param audioPaths 只读发布表，解析期间其中元素地址保持稳定。
/// @param result 累计已输出和未变更的谱面。
/// @return 改链并发布成功、或本次没有引用需要更新时为真。
bool copyChart(const std::filesystem::path& chart,
               const std::filesystem::path& stage, const AudioPaths& audioPaths,
               AudioBatchExportResult& result)
{
    const auto extension = lowerAscii(Config::pathToUtf8(chart.extension()));
    // 先按格式分发：二进制 IMD 的资源位置属于文件命名契约，
    // 不能沿用显式路径字段的文本处理，其余后缀受谱面白名单约束。
    if ( extension == ".imd" ) return copyImd(chart, audioPaths, result);
    if ( !isKnownPackageResourceExtension(PackageResourceType::Beatmap,
                                          extension) )
        return false;
    // 读取上限是 BMS 路径独立的前置检查；其他文本解析器各有输入契约。
    std::error_code readError;
    // BMS 系列整文件解码，对大输入先限额，避免为随后会拒绝的载荷
    // 分配全文缓冲。文本复制保留实际行结束符，由格式专用转换器解释。
    constexpr auto MAX_BMS_BYTES = 64U * 1024U * 1024U;
    if ( extension != ".osu" && extension != ".mmm" && extension != ".mc" &&
         std::filesystem::file_size(chart, readError) > MAX_BMS_BYTES )
        return false;
    // 以二进制模式保留源编码、换行和潜在 BOM。
    // 若读入未完成即拒绝改写，不能产生只包含有效前缀的谱面副本。
    std::ifstream stream(chart, std::ios::binary);
    if ( !stream ) return false;
    std::string source(std::istreambuf_iterator<char>(stream), {});
    if ( stream.bad() ) return false;

    const bool native = extension == ".mmm";
    const auto parent = chart.parent_path();
    // 外部格式不读项目配置；它们的同名引用仅按源谱面自己的目录解释。
    const auto project = native ? loadOwningProject(chart) : Project{};
    // MMM 的 ID 会从项目资源表解析；缺少 ID 表时仍支持路径型引用。
    std::map<std::string, const std::filesystem::path*> ids;
    if ( native ) {
        // 项目 ID 对应实际登记路径；只有该源已经成功发布时才记录 ID。
        // 值借用 audioPaths 中稳定路径，当前调用只读，避免为每个 ID
        // 再复制路径。
        for ( const auto& resource : project.m_audioResources ) {
            const auto match = audioPaths.find(canonicalPath(
                project.m_projectRoot / referencePath(resource.m_path)));
            if ( match != audioPaths.end() )
                ids.try_emplace(resource.m_id, &match->second);
        }
    }
    const auto base = native ? project.m_projectRoot : parent;
    // 同一个采样可以被整张谱面反复引用。缓存成功、未命中和格式不支持，
    // 避免每个物件都重复规范化路径或查询文件系统。
    std::map<std::string, std::optional<std::string>> cache;
    const auto                                        resolve =
        [&](const std::string& text) -> std::optional<std::string> {
        if ( text.empty() ) return std::nullopt;
        // 原始字段值作为键，缓存解析首选项以及保持原引用的结论。
        auto [cached, inserted] = cache.try_emplace(text);
        if ( !inserted ) return cached->second;
        const auto ref = referencePath(text);
        const auto id  = ids.find(text);
        // 优先按本格式的相对基准匹配，不用同名文件猜测资源身份。
        const auto  path   = audioPaths.find(canonicalPath(base / ref));
        const auto* target = id != ids.end()            ? id->second
                             : path != audioPaths.end() ? &path->second
                                                        : nullptr;
        // 旧 MMM 可能用谱面目录作为相对路径基准。只有项目基准与 ID 都没匹配，
        // 才尝试兼容基准，不能令同名的本地样本覆盖项目资源。
        if ( !target && native && base != parent ) {
            const auto relative = audioPaths.find(canonicalPath(parent / ref));
            if ( relative != audioPaths.end() ) target = &relative->second;
        }
        // 始终按写出的谱面格式计算新路径。外部文本仍保存到源目录，
        // MMM 则保持项目根基准，避免路径形式取决于转码输出目录。
        if ( target ) {
            // 制品路径已完成发布；此处计算语法上的相对路径，不再次解析磁盘别名。
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

    // 各格式使用独立声明语法，共用上方的文件身份解析和不可表达约定。
    // osu! 转换器产出独立缓冲，成功才接管，失败时不向暂存文件写入。
    bool changed = false;
    if ( extension == ".osu" ) {
        std::string rewritten;
        if ( !rewriteOsuAudioReferences(source, resolve, rewritten, changed) )
            return false;
        source = std::move(rewritten);
    } else if ( extension == ".mc" || native ) {
        // 两个 JSON 格式共享对象遍历策略，仅字段白名单与资源基准不同。
        if ( !rewriteJson(source, native, resolve, changed) ) return false;
    } else if ( !rewriteBms(source, resolve, changed) ) {
        // 格式白名单在本函数入口限制；剩余的是使用 BMS 文本声明的扩展族。
        return false;
    }
    // 没有任何可改引用时无需创建和源一致的副本。
    // 部分转码失败且另一资源成功时仍可发布新版，未命中的字段保持旧路径。
    if ( !changed ) {
        ++result.m_chartsUnchanged;
        return true;
    }
    // 每份谱面复用暂存文件，必须以截断模式重新写入并检查关闭。
    // 只有正文写入完成后才尝试不覆盖地复制到正式目录。
    std::ofstream rewritten(stage, std::ios::binary | std::ios::trunc);
    rewritten.write(source.data(), static_cast<std::streamsize>(source.size()));
    rewritten.close();
    if ( !rewritten ) return false;
    // 发布到源谱面同目录，其余未改的相对资源仍能沿原基准解析。
    // 新增后缀用于区分原件和转码副本，并保留谱面扩展名供编辑器分派。
    auto preferred = parent / Config::utf8ToPath(
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
/// @param request 输入和选项的任务副本，允许本工作者替换其中的输出与进度回调。
/// @return 分别计数实际发布的音频和谱面，并归集未改写项与失败诊断。
/// @note 计数以公开输出完成为准，临时文件和重复输入不重复计入。
/// @pre 调用方保证进度回调可以在文件池线程运行。
AudioBatchExportResult exportAudioBatch(AudioBatchExportRequest request)
{
    AudioBatchExportResult result;
    std::error_code        error;
    // 批级参数先验证，失败时无需创建暂存或触碰源谱面。
    // 编码器按扩展名选择容器，不能把未知后缀直接拿去生成制品。
    const auto outputRoot = canonicalPath(request.m_directory);
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
    // 以本轮独有的目录隔离固定暂存名；只能在排他创建成功后取得清理责任。
    // 暂存放在用户选择的输出树，路径有效性和权限错误在此明确报告。
    const auto staging =
        outputRoot /
        (".mmm-audio-export-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    if ( !std::filesystem::create_directory(staging, error) ) {
        addError(result, outputRoot, "无法创建转码暂存目录");
        return result;
    }
    // 发布操作复制到暂存树之外，因此这个守卫只清理未发布的内部副本。
    const StagingDirectory cleanup{ staging };
    AudioPaths             converted;
    // seen 按阶段记录身份，避免路径别名多次读取同一输入。
    // imdInputs 跨两个阶段记录需要为隐式前缀保留命名空间的源音频。
    std::set<std::filesystem::path> seen, imdInputs;
    // 预读 IMD 是为编码制品先选安全前缀，不能等音频发布后再猜同名资源。
    // 读取不成功的谱面仍由后面的谱面阶段报告相应失败。
    for ( const auto& chart : request.m_charts ) {
        if ( lowerAscii(Config::pathToUtf8(chart.extension())) != ".imd" )
            continue;
        const auto map = BeatMap::loadFromFile(chart);
        if ( !map.m_baseMapMetadata.song_file_hint.empty() )
            imdInputs.insert(canonicalPath(
                chart.parent_path() / map.m_baseMapMetadata.song_file_hint));
    }
    // 单文件编码器报告的是本任务内比例。保存原回调再建立桥接，
    // 使复制失败、重复输入和谱面步骤仍采用同一批级分母。
    const auto  originalProgress = request.m_options.progressCallback;
    const auto  count = request.m_inputs.size() + request.m_charts.size();
    std::size_t index = 0;
    // 进度按项目计数；单项失败也完成该阶段，后续有效文件仍继续。
    // 用当前项目编号平移单文件百分比，完成一个项目后从下一格起点继续。
    const auto progress = [&](float value, std::string_view label) {
        if ( originalProgress )
            originalProgress({ .progress = (static_cast<float>(index) + value) /
                                           static_cast<float>(count),
                               .message = std::string(label) });
    };
    // 内部暂存文件需要真实扩展名，编码器由该后缀选容器。
    // 稳定的暂存名仅在串行处理下复用，正式路径按输入名及冲突规则分配。
    request.m_options.outputPath =
        staging / Config::utf8ToPath("audio" + request.m_extension);
    request.m_options.progressCallback = [&](const auto& item) {
        progress(item.progress, item.message);
    };
    for ( const auto& value : request.m_inputs ) {
        const auto input = canonicalPath(value);
        // 去重比较实际来源，使用第一次出现的输入名定义制品命名。
        // 重复项不再次编码，仍消耗原请求中的一格进度。
        if ( seen.insert(input).second ) {
            // 编码路径与映射表使用同一个规范输入；原参数仅保留用户侧命名信息。
            request.m_options.inputPath = input;
            const auto item =
                Audio::AudioSpeedExportService::exportWav(request.m_options);
            if ( item.success ) {
                auto       stem      = Config::pathToUtf8(value.stem());
                const bool imdPrefix = imdInputs.contains(input);
                // 为 IMD
                // 使用新前缀，避免库在同目录时按原音频扩展名优先级读取旧文件。
                // 若前缀仍冲突，后续发布步骤再按编号避让。
                if ( imdPrefix ) stem += " (转码)";
                // 实际文件名还要经过冲突处理，首选路径本身不会提前进入映射表。
                const auto preferred =
                    outputRoot / Config::utf8ToPath(stem + request.m_extension);
                auto output = copyToFreePath(
                    request.m_options.outputPath, preferred, error, imdPrefix);
                // 编码成功只代表暂存完整；目标目录写入错误仍须单独计为该音频失败。
                if ( output.empty() )
                    addError(result, input, "无法写入音频：" + error.message());
                else {
                    ++result.m_audioWritten;
                    // 只有正式复制成功的路径才能进入谱面映射表。
                    // 保留改名后的实际路径，不能在改链时重新以首选名推断。
                    converted.emplace(input, std::move(output));
                }
            } else {
                // 编码器失败保留其原始诊断，避免把解码输入失败误报为目标命名冲突。
                addError(result, input, item.errorMessage);
            }
            // 每项使用相同的暂存名，失败留下的半成品必须先清理再开始下一项。
            std::filesystem::remove(request.m_options.outputPath, error);
        }
        ++index;
    }
    // 新阶段的查重对象是谱面，而不是前一阶段的媒体输入。
    // 映射表此时冻结，供各格式独立改链；一个谱面失败不能使另一份已发布副本失效。
    seen.clear();
    for ( const auto& chart : request.m_charts ) {
        const auto input = canonicalPath(chart);
        // 按规范路径只处理一次同一谱面，报错仍使用请求中的路径。
        // 相对引用未命中属于可保留原态，真正无法复制或改写才记入失败。
        if ( seen.insert(input).second &&
             !copyChart(input, staging / "chart", converted, result) )
            addError(result, chart, "无法写出保持音频关联的谱面副本");
        ++index;
        // 谱面步骤没有细分百分比，在完成后推进一整格。
        // 进度不宣称每项都成功；具体计数与失败原因由结果返回。
        progress(0.0F, "正在同步谱面引用");
    }
    // 所有发布计数在各自文件完成时递增，批次退出只负责汇报并清理暂存树。
    return result;
}
}  // namespace MMM::Logic
