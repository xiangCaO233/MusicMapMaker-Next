#include "logic/ImdPackageExportService.h"

#include "audio/AudioTimelineExportService.h"
#include "config/Utf8Path.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/PackageFileTypes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::Logic
{
namespace
{

/// @brief 负责在导出完成或提前失败时清理工作目录。
/// @note 只在目录创建成功后构造，避免将已存在的冲突目录纳入清理范围。
/// @note 守卫只负责工作文件，不承担最终包的提交或回滚职责。
struct TemporaryDirectoryGuard {
    /// @brief 需要递归清理的临时目录。
    std::filesystem::path path;

    /// @brief 清理临时目录；清理失败不覆盖原始导出结果。
    ~TemporaryDirectoryGuard()
    {
        // 守卫只接管本次成功创建的工作目录，不触及目标谱包或源资源。
        std::error_code filesystemError;
        std::filesystem::remove_all(path, filesystemError);
    }
};

/// @brief 判断 ASCII 字符是否不适合出现在资源包文件名中。
/// @param character 待检查字符。
/// @return 需要替换时返回 true。
/// @note 本函数只判断单字节，不执行 Unicode 规范化或系统保留名检测。
/// @note 下划线虽通常可作文件名字符，但这里为 IMD 字段分隔语义保留。
bool isInvalidFileNameCharacter(unsigned char character) noexcept
{
    // 按字节过滤 ASCII，UTF-8 的高位字节原样保留。
    if ( character < 0x20U ) return true;
    switch ( character ) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '/':
    case '\\':
    case '|':
    case '?':
    case '*':
    case '_':
        return true;  // 下划线属于 IMD 文件名字段分隔符，不能混入字段内部。
    default: return false;
    }
}

/// @brief 将图片扩展名规范为 IMD 解析器能够查找的小写形式。
/// @param extension 原始扩展名。
/// @return 仅转换 ASCII 大写字母后的扩展名。
/// @note 保留前导点，供扩展名白名单直接比较。
/// @note 不检验文件签名，扩展名合法不能证明图片内容可解码。
std::string lowerAsciiExtension(std::string extension)
{
    // 扩展名比较不依赖系统区域设置，也不转换路径或文件内容。
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char character) {
                       return character >= 'A' && character <= 'Z'
                                  ? static_cast<char>(character - 'A' + 'a')
                                  : static_cast<char>(character);
                   });
    return extension;
}

/// @brief 生成不会破坏 IMD 首段命名规则的文件名片段。
/// @param value 用户选择的包名或谱面元数据文本。
/// @param fallback 清理后为空时使用的回退值。
/// @return 不包含下划线和路径非法字符的 UTF-8 文件名片段。
/// @pre fallback 是调用方提供的安全文件名片段。
/// @note 对值副本进行清理，不改变谱面中原始标题和难度名称。
/// @note 该转换不保证可逆，不同标题可能得到相同包内前缀。
/// @note 这里生成包内字段，不负责选择最终输出目录或解决目标文件冲突。
std::string sanitizeImdFileNamePart(std::string      value,
                                    std::string_view fallback)
{
    for ( char& character : value ) {
        // 先统一替换，再合并连续替代符；不删除中间的合法空格和点。
        const auto byte = static_cast<unsigned char>(character);
        if ( isInvalidFileNameCharacter(byte) ) character = '-';
    }
    // 连续非法字符只保留一个替代连字符，避免冗长占位片段。
    value.erase(std::unique(value.begin(),
                            value.end(),
                            [](char lhs, char rhs) {
                                return lhs == '-' && rhs == '-';
                            }),
                value.end());
    // 清理边缘标点，避免隐藏文件名或不稳定的末尾字符。
    while ( !value.empty() && (value.front() == ' ' || value.front() == '.' ||
                               value.front() == '-') ) {
        value.erase(value.begin());
    }
    while ( !value.empty() && (value.back() == ' ' || value.back() == '.' ||
                               value.back() == '-') ) {
        // 逐次检查长度，使全由待清理字符组成的名称也能安全回退。
        value.pop_back();
    }
    // fallback 是内部提供的安全片段，不再递归清理。
    return value.empty() ? std::string(fallback) : value;
}

/// @brief 为本次导出创建唯一临时目录。
/// @param outputPath 目标包路径，用于生成可诊断的目录名。
/// @param directory 接收成功创建的临时目录。
/// @note 失败时 directory 可能仍含最后一次候选路径，不代表该目录归本次所有。
/// @return 创建成功时返回 true。
/// @note 不复用已有工作目录，避免不同导出之间混用中间音频和谱面。
/// @warning 低频导出路径执行文件系统操作，不得用于逐帧临时缓冲管理。
bool createTemporaryDirectory(const std::filesystem::path& outputPath,
                              std::filesystem::path&       directory)
{
    std::error_code filesystemError;
    const auto      temporaryRoot =
        std::filesystem::temp_directory_path(filesystemError);
    if ( filesystemError || temporaryRoot.empty() ) return false;

    // 时间戳用于降低冲突概率，实际排他性以 create_directory 的结果为准。
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string stem =
        sanitizeImdFileNamePart(Config::pathToUtf8(outputPath.stem()), "map");
    for ( std::uint32_t attempt = 0U; attempt < 16U; ++attempt ) {
        // 尝试次数有界；冲突时只换候选名称，不删除占用者的目录。
        directory =
            temporaryRoot / Config::utf8ToPath("mmm-imd-export-" + stem + "-" +
                                               std::to_string(stamp) + "-" +
                                               std::to_string(attempt));
        filesystemError.clear();
        // 每次操作重新收集错误，不能让上一个候选的冲突状态影响新候选。
        if ( std::filesystem::create_directory(directory, filesystemError) ) {
            return true;
        }
        if ( filesystemError && filesystemError != std::errc::file_exists ) {
            // 权限、空间等错误继续改名无益，只有名称占用才进入下一次尝试。
            return false;
        }
    }
    return false;
}

/// @brief 清除导出副本中 RM/IMD 无法表达的玩家物件采样绑定。
/// @param beatMap 仅用于导出的可修改谱面副本。
/// @note 不删除玩家物件或改变其时间和轨道，仅移除目标格式不支持的绑定。
/// @note 自动采样列表由导出主流程单独替换，不在此函数中处理。
void clearNoteSampleBindings(BeatMap& beatMap)
{
    // 玩家物件触发的声音已并入混音，移除绑定防止导入后重复播放。
    /// @brief 对单种玩家物件容器统一清除采样绑定。
    const auto clearBindings = [](auto& notes) {
        for ( auto& note : notes ) {
            note.clearSampleBinding();
        }
    };
    clearBindings(beatMap.m_noteData.notes);
    clearBindings(beatMap.m_noteData.holds);
    clearBindings(beatMap.m_noteData.flicks);
    clearBindings(beatMap.m_noteData.polylines);
}

/// @brief 读取完整二进制文件。
/// @param path 来源路径。
/// @param bytes 接收文件字节。
/// @return 成功读取时返回 true。
/// @note 成功时 bytes 拥有完整文件内容，后续压缩不依赖文件流生命周期。
/// @pre 文件在读取期间保持稳定，本函数不对并发写入建立快照。
/// @warning 整文件读入会分配与文件大小对应的内存，仅限离线导出流程。
bool readFileBytes(const std::filesystem::path& path,
                   std::vector<std::uint8_t>&   bytes)
{
    bytes.clear();
    // 先定位末尾取得大小，一次分配输出；失败时调用方不得使用部分数据。
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return false;
    const auto size = file.tellg();
    // 负位置表示无法取得文件大小，不能转换成无符号分配长度。
    if ( size < 0 ) return false;
    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    // 空文件也是合法归档成员，不要求发起零长度读取。
    if ( !bytes.empty() ) {
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    return file.good();
}

/// @brief 将内存归档写入目标文件。
/// @param path 目标路径。
/// @param data 归档字节指针。
/// @param size 归档字节数。
/// @return 成功完整写入时返回 true。
/// @pre size 非零时 data 指向至少 size 字节的有效归档缓冲。
/// @note 成功表示写入后流状态正常，不包含落盘同步或重新读取校验。
/// @note data 仅被借用；本函数无论成功失败都不释放归档缓冲。
/// @warning 此函数可能创建目标父目录并覆盖同名文件，不是只读检查。
bool writePackageFile(const std::filesystem::path& path, const void* data,
                      std::size_t size)
{
    std::error_code filesystemError;
    if ( !path.parent_path().empty() ) {
        // 相对文件名可直接使用当前目录；仅在存在父路径时创建目录层级。
        std::filesystem::create_directories(path.parent_path(),
                                            filesystemError);
        if ( filesystemError ) return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    // 此处直接截断写目标，不提供临时文件替换或旧包回滚保证。
    if ( !file ) return false;
    if ( size > 0U ) {
        // 输出按二进制写入，不进行换行转换或字符编码处理。
        file.write(static_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
    }
    return file.good();
}

/// @brief 向 zip 归档添加一个根目录文件。
/// @param archive 正在写入的归档。
/// @param archiveName 包内 UTF-8 文件名。
/// @param sourcePath 来源文件路径。
/// @param errorMessage 接收失败原因。
/// @return 成功添加时返回 true。
/// @pre archive 已成功初始化为 writer，archiveName 是包根目录安全文件名。
/// @note 成功时不清空 errorMessage；调用方只在返回 false 时读取错误。
/// @note 成员字节临时读入当前函数，压缩器完成添加后即可释放该副本。
/// @warning 该操作读取完整文件并压缩，耗时随资源大小增长。
bool addArchiveFile(mz_zip_archive& archive, const std::string& archiveName,
                    const std::filesystem::path& sourcePath,
                    std::string&                 errorMessage)
{
    std::vector<std::uint8_t> bytes;
    if ( !readFileBytes(sourcePath, bytes) ) {
        // 错误区分读取与压缩阶段，保留对应文件名便于定位缺失资源。
        errorMessage =
            "无法读取待打包文件：" + Config::pathToUtf8(sourcePath.filename());
        return false;
    }
    const void* data = bytes.empty() ? nullptr : bytes.data();
    // 先用 filesystem 路径读入，再向压缩器传内存，避免压缩器解释本机路径编码。
    if ( !mz_zip_writer_add_mem(&archive,
                                archiveName.c_str(),
                                data,
                                bytes.size(),
                                MZ_BEST_COMPRESSION) ) {
        errorMessage = "无法压缩文件：" + archiveName;
        return false;
    }
    return true;
}

}  // namespace

/// @brief 将谱面副本、离线混音和背景图封装为 IMD/RM ZIP 资源包。
/// @param beatMap 只读源谱面，兼容性修改只作用于临时加载的副本。
/// @param audioEvents 已解析的音频时间线事件，混音服务负责播放时间与偏移。
/// @param chartEndSeconds 传给混音服务的谱面结束时间，单位秒。
/// @param coverPath 本地背景图片路径，当前仅接受 PNG/JPG/JPEG 扩展名。
/// @param outputPath 最终资源包路径，其 stem 优先作为包内资源名前缀。
/// @param progress 可选同步阶段回调，混音阶段也复用此回调。
/// @return 成功标志、包内文件名与失败原因；文件名赋值不代表导出成功。
/// @warning 低频导出路径包含文件读写、音频转码和压缩，不得在渲染热路径调用。
/// @note 临时文件在退出时清理；写目标失败可能留下不完整包，不承诺事务回滚。
/// @pre 调用期间源谱面和 audioEvents 保持稳定，音频资源路径仍然可用。
/// @note progress 在当前执行线程调用，回调不得假设处于 UI 线程。
/// @note 回调提供阶段文本而非完成百分比，返回结果才是成功与否的依据。
/// @note 不修改谱面物件时间；音频时间线与生成谱面的时间零点保持同一约定。
ImdPackageExportResult ImdPackageExportService::exportPackage(
    const BeatMap&                                    beatMap,
    const std::vector<Audio::AudioTimelineLoadEvent>& audioEvents,
    double chartEndSeconds, const std::filesystem::path& coverPath,
    const std::filesystem::path&                 outputPath,
    const std::function<void(std::string_view)>& progress)
{
    ImdPackageExportResult result;
    // 结果默认失败，直到所有成员压缩及目标写入成功后才置成功标志。
    if ( progress ) progress("正在生成 IMD 谱面…");
    if ( outputPath.empty() ) {
        // 在创建临时副本或混音前拒绝无效输出，避免产生无目标的导出工作。
        result.errorMessage = "IMD 资源包输出路径为空";
        return result;
    }
    if ( beatMap.m_baseMapMetadata.track_count <= 0 ) {
        // 后续把轨道数用作首个 BGM 绝对索引，必须先保证玩家轨道数有效。
        result.errorMessage = "IMD 资源包要求谱面轨道数大于零";
        return result;
    }

    std::error_code filesystemError;
    if ( !std::filesystem::is_regular_file(coverPath, filesystemError) ||
         filesystemError ) {
        // 背景是必需成员；读取其属性失败与不存在都在昂贵转码前拒绝。
        result.errorMessage = "找不到谱面背景图片";
        return result;
    }
    // 只验证文件类型入口，不重编码图片；包内沿用图片字节和规范后的扩展名。
    const std::string coverExtension =
        lowerAsciiExtension(Config::pathToUtf8(coverPath.extension()));
    static constexpr std::array<std::string_view, 3> IMD_COVER_EXTENSIONS{
        // 不接受视频或任意可解码图片格式，确保目标格式能够按扩展名找到背景。
        ".png",
        ".jpg",
        ".jpeg"
    };
    if ( !packageExtensionInList(IMD_COVER_EXTENSIONS, coverExtension) ) {
        result.errorMessage = "IMD 资源包背景只支持 PNG、JPG 或 JPEG";
        return result;
    }

    std::string rawPrefix = Config::pathToUtf8(outputPath.stem());
    // 优先使用用户选定的包名，只有空 stem 才依次回退到谱面标题。
    if ( rawPrefix.empty() ) {
        const auto& meta = beatMap.m_baseMapMetadata;
        rawPrefix        = !meta.title_unicode.empty()
                               ? meta.title_unicode
                               : (!meta.title.empty() ? meta.title : meta.name);
    }
    const std::string prefix =
        sanitizeImdFileNamePart(std::move(rawPrefix), "map");
    // 难度字段独立清理，避免其中下划线改变包内谱面名称的分段解释。
    const std::string version =
        sanitizeImdFileNamePart(beatMap.m_baseMapMetadata.version, "default");
    // 首段前缀必须与音频和图片一致，解析器据此关联包内资源。
    result.beatmapFileName =
        prefix + "_" + std::to_string(beatMap.m_baseMapMetadata.track_count) +
        "k_" + version + ".imd";
    result.audioFileName = prefix + ".mp3";
    result.coverFileName = prefix + coverExtension;
    // 这里只决定包内命名，不重命名源背景或项目中的原始音频。

    std::filesystem::path temporaryDirectory;
    if ( !createTemporaryDirectory(outputPath, temporaryDirectory) ) {
        // 尚未构造清理守卫，失败候选可能属于其他进程，不能递归移除。
        result.errorMessage = "无法创建 IMD 资源包临时目录";
        return result;
    }
    const TemporaryDirectoryGuard cleanup{ temporaryDirectory };
    // 守卫在导出副本之后析构，保证仍使用中间资源的局部对象先结束生命周期。
    // 从此处开始的所有提前返回都由守卫清理工作目录。

    const auto sourceBeatmapPath = temporaryDirectory / "source.mmm";
    // 通过本机格式往返创建独立导出副本，后续移除不兼容字段不影响编辑会话。
    if ( !beatMap.saveToFile(sourceBeatmapPath) ) {
        result.errorMessage = "无法创建 IMD 导出副本";
        return result;
    }
    BeatMap exportBeatMap = BeatMap::loadFromFile(sourceBeatmapPath);
    // 导出副本来自刚写出的中间文件，不复用编辑器的实体或操作历史。
    clearNoteSampleBindings(exportBeatMap);
    exportBeatMap.m_audioSamples.clear();
    // 所有自动采样合并为一个 MP3；副本只保留从时间零开始的统一播放入口。
    exportBeatMap.m_baseMapMetadata.bgm_track_count =
        // 至少保留一个 BGM 轨道容纳合并音频，不缩减原有正轨道数。
        std::max(exportBeatMap.m_baseMapMetadata.bgm_track_count, 1);
    exportBeatMap.m_audioSamples.push_back(AudioSampleEvent{
        // 原采样音量和偏移已经交给混音处理，此入口不能重复叠加这些参数。
        // BGM 首轨的绝对索引紧随玩家轨道，资源 ID 必须匹配包内 MP3 文件名。
        .m_timestamp = 0.0,
        .m_offsetMs  = 0,
        .m_track     = static_cast<std::uint32_t>(
            exportBeatMap.m_baseMapMetadata.track_count),
        .m_audioResourceId = result.audioFileName,
        .m_volume          = 1.0F,
    });
    // 修改完成后统一同步派生状态，再交给 IMD 序列化器。
    exportBeatMap.sync();

    const auto imdPath =
        temporaryDirectory / Config::utf8ToPath(result.beatmapFileName);
    if ( !exportBeatMap.saveToFile(imdPath) ) {
        // 谱面转换失败时无需启动音频混合，避免无效的转码开销。
        result.errorMessage = "无法生成兼容的 IMD 谱面";
        return result;
    }

    const auto audioPath =
        // 混音输出位于本次独占工作目录，不覆盖项目内同名 MP3。
        temporaryDirectory / Config::utf8ToPath(result.audioFileName);
    // 原事件列表不替换为副本中的单一采样，确保所有声音仍参与离线混合。
    const auto audioResult =
        Audio::AudioTimelineExportService::exportMixedAudio(
            Audio::AudioTimelineExportOptions{
                .events          = audioEvents,
                .chartEndSeconds = chartEndSeconds,
                .outputPath      = audioPath,
                .progress        = progress,
            });
    if ( !audioResult.success ) {
        // 转码失败立即终止，不用缺失或部分音频继续生成看似成功的包。
        result.errorMessage = "无法拼装 MP3 音频：" + audioResult.errorMessage;
        return result;
    }

    if ( progress ) progress("正在压缩 RM 资源包…");
    mz_zip_archive archive{};
    // 归档先在内存完成，成员压缩失败不会提前打开最终输出文件。
    if ( !mz_zip_writer_init_heap(&archive, 0, 0) ) {
        // 初始化未成功时没有可用 writer，直接交由目录守卫收尾。
        result.errorMessage = "无法初始化 IMD 资源包压缩器";
        return result;
    }

    // 短路保留第一个失败原因，三个成员均放在包根目录，不保留临时路径。
    bool success =
        addArchiveFile(
            archive, result.beatmapFileName, imdPath, result.errorMessage) &&
        addArchiveFile(
            archive, result.audioFileName, audioPath, result.errorMessage) &&
        addArchiveFile(
            archive, result.coverFileName, coverPath, result.errorMessage);
    void* archiveBuffer = nullptr;
    // 堆归档缓冲由压缩库产生，不能用普通容器释放或在 writer 内部状态中寻找。
    std::size_t archiveSize = 0U;
    // 仅当全部成员加入成功时生成最终归档目录和输出缓冲。
    if ( success && !mz_zip_writer_finalize_heap_archive(
                        &archive, &archiveBuffer, &archiveSize) ) {
        result.errorMessage = "无法完成 IMD 资源包压缩";
        // 成员加入成功仍不足以形成完整 ZIP，必须完成最终目录写入。
        success = false;
    }
    // 压缩器内部状态与最终堆缓冲分别释放，关闭 writer 后缓冲仍用于写文件。
    mz_zip_writer_end(&archive);

    if ( success &&
         !writePackageFile(outputPath, archiveBuffer, archiveSize) ) {
        // 只有完整内存归档才进入写目标阶段，不把初始化失败当成空包写入。
        result.errorMessage = "无法写入 IMD 资源包文件";
        success             = false;
    }
    // 不论最终写入成功与否，都归还压缩库分配的缓冲区。
    if ( archiveBuffer ) mz_free(archiveBuffer);

    result.success = success;
    // result 不持有临时路径或压缩缓冲，返回后可独立用于界面反馈。
    return result;
}

}  // namespace MMM::Logic
