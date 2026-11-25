#include "logic/BmsPackageExportService.h"
#include "common/platform/DocumentStorage.h"

#include "audio/AudioOriginAlignmentService.h"
#include "common/LogicCommands.h"
#include "config/Utf8Path.h"
#include "logic/ProjectResourceService.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/MyO2Bms.h"
#include "mmm/project/PackageFileTypes.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <miniz.h>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

/// @file BmsPackageExportService.cpp
/// @brief 标准 BMS 谱包的时序、资源引用和 ZIP 提交事务。
///
/// 来源目录始终只读：谱面加载为值模型，重新编码只写独占工作目录。
/// 首红线定义原点相位，而不是把每颗音符重新吸附到固定分拍数。
/// 音符整体前移时裁掉主音乐头部，整体后移时补入真实静音帧。
/// 主音乐的独立触发偏移计入音频处理，避免重复叠加 SOUND 的 offset。
/// 多张难度拥有独立音乐副本，不要求共享源文件具有相同的原点相位。
/// Key 采样依赖单独登记，因此音乐同时作为 Key 文件时仍保留原始字节。
/// 包内路径保持项目相对目录，谱面引用使用相对其自身目录的通用 UTF-8 路径。
/// 首拍相位变化不会改变 STOP 持续时长或长条长度。
/// ZIP 仅在所有条目准备完成后提交，转换失败不会提前截断旧包。
/// 该服务不建立编辑会话、不更新项目元数据，也不持有跨事务资源指针。
namespace MMM::Logic
{
namespace
{
/// @brief 清理本次成功创建的临时目录，不接管源文件或最终输出。
struct WorkDirectory {
    /// @brief 导出事务独占的目录。
    std::filesystem::path path;
    /// @brief 提前返回和正常完成均回收中间谱面与音频。
    ~WorkDirectory()
    {
        // path 只来自 create_directory 成功的本轮目录，不接管冲突目录。
        // 析构不能用清理错误覆盖最初的谱面或编码失败原因。
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

/// @brief 成对关闭 ZIP writer，失败时同样释放压缩器状态。
struct ArchiveWriter {
    /// @brief 已初始化的内存 ZIP writer。
    mz_zip_archive value{};
    /// @brief 是否取得 writer 所有权。
    bool initialized{ false };
    /// @brief 初始化失败时不调用结束接口。
    ~ArchiveWriter()
    {
        // finalize 后 writer 仍需结束，但返回的归档缓冲由最终提交显式释放。
        // 提前失败尚未取得归档缓冲，只需释放 writer 自己拥有的压缩状态。
        if ( initialized ) mz_zip_writer_end(&value);
    }
};

/// @brief 检查规范化后的归档路径是否仍位于项目相对命名空间。
/// @param path 已词法规范化的项目相对路径。
/// @return 能安全作为归档名称时为真。
/// @pre 调用者已经执行 lexically_normal，检查必须发生在打开源文件前。
/// @note 不检查音频内部引用；引用随后通过项目资源身份解析。
/// @note 允许子目录，不能只取 basename，否则同名采样会发生碰撞。
bool safeRelative(const std::filesystem::path& path)
{
    // 包内资源不能采用绝对路径、驱动器路径或向父目录逃逸的名称。
    if ( path.empty() || path.is_absolute() || path.has_root_name() )
        return false;
    for ( const auto& part : path )
        if ( part == ".." ) return false;
    return path != ".";
}

/// @brief 原始资源与生成文件均使用同一二进制读取入口。
/// @param path 实际文件路径。
/// @param bytes 接收完整内容，允许零字节资源。
/// @return 打开及读取均成功时为真。
/// @note 路径由选择校验或独占工作目录提供，不扫描其他系统目录。
/// @note 零字节内容不等于打开失败，是否可解码由对应资源消费者决定。
/// @warning 完整读取文件，只允许在本服务的低频打包事务中使用。
bool readFile(const std::filesystem::path& path,
              std::vector<unsigned char>&  bytes)
{
    // 同一读取器处理已压缩音包和散装媒体，换行或文件中零字节不应有特殊含义。
    // 输入容器由调用方复用；先打开新来源，失败时不能把上一个条目的内容登记为新来源。
    std::ifstream input(path, std::ios::binary);
    if ( !input ) return false;
    bytes.assign(std::istreambuf_iterator<char>(input), {});
    return !input.bad();
}

/// @brief 在首拍网格上统一平移时间轴，不单独吸附物件。
/// @param map 仅用于本次导出的独立谱面。
/// @param mainSample 唯一主音轨事件，没有完整主音乐时为空。
/// @param audioPhase 接收主音乐需要裁掉的毫秒数，负值表示补静音。
/// @param mode 通过音频内容或事件位置承担整体相位补偿。
/// @param error 接收不能建立有限、非负时间轴的原因。
/// @return 完成全部前置检查和变换时为真。
/// @pre mainSample 借用 map 的样本容器，变换期间不得增删该容器。
/// @note 没有整曲主音乐的纯采样谱面仍可统一移动所有触发事件。
/// @note phase 与 audioPhase 不一定相同，后者还要减去原 Main 实际触发时间。
/// @note 提前音符会选择等价的负相位，避免为了对齐而删除物件。
/// @note 首 BPM 延长到原点与平移音符不是两次时间补偿，初始速度保持相同。
/// @warning 遍历完整模型并修改持久化副本，不用于实时音频补偿。
bool alignTimeline(BeatMap& map, AudioSampleEvent* mainSample,
                   double& audioPhase, BmsPackageAlignmentMode mode,
                   std::string& error)
{
    // 不能依赖来源容器已经按时间排序，首红线必须从所有 BPM 中选择。
    Timing* first = nullptr;
    for ( auto& timing : map.m_timings ) {
        if ( timing.m_timingEffect != TimingEffect::BPM ) continue;
        if ( !first || timing.m_timestamp < first->m_timestamp )
            first = &timing;
    }
    // 无有效红线时不能从参考 BPM 猜测原始首拍，否则会改变用户节奏原点。
    // BPM 本身与时间都需有限，避免 fmod、ceil 将无效输入传播给全部物件。
    if ( !first || !std::isfinite(first->m_timestamp) ||
         !std::isfinite(first->m_timingEffectParameter) ||
         first->m_timingEffectParameter <= 0 ) {
        error = "BMS 打包需要有效的首个 BPM 红线";
        return false;
    }
    const double beatLength = 60000.0 / first->m_timingEffectParameter;
    if ( !std::isfinite(beatLength) || beatLength <= 0 ) return false;
    // 只移动一拍内的剩余量，长前导段不应导致整段音乐被无谓裁掉。
    // fmod 保留负时间的符号，所以负首红线会自然转成补静音计划。
    // 相差整拍的候选是同一个对齐网格，所以后续可以按需要保留合法前导，不必改变
    // BPM。
    double phase = std::fmod(first->m_timestamp, beatLength);
    // 事件模式只能后移，不能裁掉音乐中任何已经存在的内容。
    // 向后补到下一整拍，保证原首拍落在从零点开始的同一网格上。
    const bool shiftEvents = mode == BmsPackageAlignmentMode::ShiftEvents;
    if ( shiftEvents && phase > 1.0e-6 ) phase -= beatLength;
    double earliest = std::numeric_limits<double>::infinity();
    // 主音轨由音频内容承担位移；其他事件都必须保留在合法非负时间范围。
    // 若裁头会使早期物件变成负数，则改为补足整数拍静音，网格相位不变。
    // 最小值按实际触发或物件起点计算。长条尾部随有效的长度迁移，
    // 不能用独立尾部吸附补偿前导相位。
    const auto inspect = [&](double time) {
        if ( !std::isfinite(time) ) return false;
        earliest = std::min(earliest, time);
        return true;
    };
    for ( const auto& note : map.m_noteData.notes )
        if ( !inspect(note.m_timestamp) ) return false;
    for ( const auto& hold : map.m_noteData.holds )
        if ( !inspect(hold.m_timestamp) ) return false;
    for ( const auto& timing : map.m_timings )
        if ( &timing != first && !inspect(timing.m_timestamp) ) return false;
    for ( const auto& sample : map.m_audioSamples )
        if ( (shiftEvents || &sample != mainSample) &&
             !inspect(sample.effectiveTimestamp()) )
            return false;
    // 重写音频模式不检查 Main 起点；事件模式连 Main 也必须保留在非负时间。
    // 不包含首 BPM，因为其前导恒速段可延长到零点，不需要保留原声明位置。
    // 每多退一拍只增加共同前导，不改变任何音符相对于原拍线的相位。
    if ( earliest < phase - 1.0e-6 )
        phase -= std::ceil((phase - earliest) / beatLength) * beatLength;
    if ( std::abs(phase) <= 1.0e-6 ) phase = 0;
    // 实际触发时间由锚点和 offset 相加，不能仅使用其中一个字段。
    // 零相位也可能需要处理有独立 offset 的主音乐。
    // 事件模式保留音乐字节和原 offset，所有 BGM 触发与物件共同平移。
    // 重写音频模式才将 Main 的局部偏移烘焙进音乐副本。
    audioPhase = !shiftEvents && mainSample
                     ? phase - mainSample->effectiveTimestamp()
                     : 0;
    // 时间差、长条时长及采样局部偏移保持不变，仅改变统一原点。
    // 首 BPM 向前延长到零点不改变前导段速度，避免写入分数拍的初始红线。
    const auto shift = [phase](double& time) {
        time -= phase;
        if ( std::abs(time) <= 1.0e-6 ) time = 0;
    };
    for ( auto& timing : map.m_timings ) {
        if ( &timing == first )
            timing.m_timestamp = 0;
        else
            shift(timing.m_timestamp);
    }
    for ( auto& note : map.m_noteData.notes ) shift(note.m_timestamp);
    // 只移动长条头部，其时长保持原值，尾部因此随整条一起平移。
    for ( auto& hold : map.m_noteData.holds ) shift(hold.m_timestamp);
    // 批注仍保有与目标物件的相对位置，后续格式决定是否能序列化它。
    for ( auto& annotation : map.m_annotations ) shift(annotation.m_timestamp);
    if ( Bms::usesMyO2Format(map.m_metadata) && phase != 0 ) {
        // 格式专用 FF/HE/SW/ED 与原生视频起点共用同一位移，保留亚毫秒相位。
        const auto old = Bms::bmsProperty(map.m_metadata, Bms::MYO2_CUE_SHIFT);
        const auto value = Internal::parseFloatingPrefix(old);
        if ( !old.empty() &&
             (value.error != std::errc{} || value.parsedLength != old.size() ||
              !std::isfinite(value.value)) )
            return false;
        const double shiftMs = old.empty() ? -phase : value.value - phase;
        map.m_metadata.map_properties[MapMetadataType::BMS]
                                     [std::string(Bms::MYO2_CUE_SHIFT)] =
            fmt::format("{}", shiftMs);
    }
    for ( auto& sample : map.m_audioSamples ) {
        if ( !shiftEvents && &sample == mainSample ) {
            // 音频文件承担全部位移，触发锚点和 offset 不得再重复应用一次。
            sample.m_timestamp = 0;
            sample.m_offsetMs  = 0;
        } else
            shift(sample.m_timestamp);
    }
    // 不需要 sync：保存器直接读取持久化坐标，导出模型不进入编辑缓存。
    return true;
}
}  // namespace

/// @brief 为单文件导出复用纯事件对齐，接口不暴露音频重写策略。
/// @param map 仅用于本次另存为的独立快照。
/// @param error 非法时间或缺失首红线的具体诊断。
/// @return 共同时间变换完成时为真。
/// @note Main 观察指针为空仍会遍历所有 BGM，事件模式不需特殊归零对象。
/// @note 不执行文件系统操作，音频文件保持字节不变由此接口边界保证。
bool BmsPackageExportService::shiftEventsToGrid(BeatMap&     map,
                                                std::string& error)
{
    // 单文件接口固定为事件模式，禁止调用者通过选项误入裁头或补头路径。
    double unusedAudioPhase = 0;
    return alignTimeline(map,
                         nullptr,
                         unusedAudioPhase,
                         BmsPackageAlignmentMode::ShiftEvents,
                         error);
}

/// @brief 校验 KEY 音 ZIP 的压缩方式、解压预算及音源引用。
/// @details 目录名使用与站点一致的 UTF-8、斜杠和小写匹配。
/// 重名会在浏览器中覆盖，因此规范化后冲突视为格式错误。
/// 仅扫描中央目录，音源保持原始压缩数据及文件名。
bool BmsPackageExportService::validateMyO2KeySoundBank(
    const BeatMap& map, const std::filesystem::path& path, std::string& error)
{
    constexpr std::uint64_t budget = 256U * 1024U * 1024U;
    error = "无法使用 MyO2 KEY 音包：" + Config::pathToUtf8(path);
    std::error_code filesystemError;
    if ( !packageExtensionEquals(Config::pathToUtf8(path.extension()),
                                 ".zip") ||
         std::filesystem::file_size(path, filesystemError) > budget ||
         filesystemError )
        return false;
    std::vector<unsigned char> contents;
    if ( !readFile(path, contents) ) return false;

    mz_zip_archive zip{};
    if ( !mz_zip_reader_init_mem(&zip, contents.data(), contents.size(), 0) )
        return false;
    // 所有退出路径都只结束本次只读 ZIP 句柄，不触碰源文件。
    struct ReaderEnd {
        mz_zip_archive& m_zip;  ///< 借用本函数内已初始化的 ZIP 句柄。
        /// @brief 释放 miniz 读取状态。
        ~ReaderEnd() { mz_zip_reader_end(&m_zip); }
    } reader{ zip };
    // 这里模拟下载端的路径键而不重命名 ZIP
    // 内容，两个原名映射到同一键时拒绝音包。 非 ASCII
    // 字节原样保留，不能随运行电脑的区域设置改变匹配规则。
    const auto normalize = [](std::string name) {
        for ( auto& c : name ) c = c == '\\' ? '/' : toPackageLowerAscii(c);
        return name;
    };
    std::set<std::string> names;
    std::uint64_t         expanded = 0;
    for ( mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i ) {
        // 这一步只取中央目录信息，内容仍由归档原字节保存。算法编号限制到存储和
        // DEFLATE，
        // 因为目标端必须能直接读取；无法取得记录时也不能把条目当作可忽略的空目录。
        // 预算用剩余量比较，以防损坏元数据的未压缩长度在累加时绕回。
        mz_zip_archive_file_stat stat{};
        if ( !mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_encrypted ||
             (stat.m_method != 0 && stat.m_method != 8) ||
             stat.m_uncomp_size > budget - expanded )
            return false;
        expanded += stat.m_uncomp_size;
        // 目录名不满足样本引用，因此只把文件加入查找集合；上方预算仍涵盖所登记的长度。
        if ( stat.m_is_directory ) continue;
        if ( !names.insert(normalize(stat.m_filename)).second ) return false;
    }
    // 空采样绑定表示静音，不要求音包有空名条目；非空名字失败时保留它供 UI
    // 定位。
    const auto contains = [&](const std::string& name) {
        if ( name.empty() || names.contains(normalize(name)) ) return true;
        error = "MyO2 KEY 音包缺少音源：" + name;
        return false;
    };
    for ( const auto& note : map.m_noteData.notes )
        if ( note.m_sampleBinding &&
             !contains(note.m_sampleBinding->m_audioResourceId) )
            return false;
    for ( const auto& hold : map.m_noteData.holds )
        if ( hold.m_sampleBinding &&
             !contains(hold.m_sampleBinding->m_audioResourceId) )
            return false;
    // 整曲模式的 BGM 通过外部音乐文件提供，自动采样中同路径的主曲不用再次在 KEY
    // 查找。 STANDARD 则连自动音源也取自音包，所以没有这个豁免。
    const auto mainPath =
        map.m_baseMapMetadata.song_file_hint.lexically_normal();
    const bool wholeMusic =
        Bms::bmsProperty(map.m_metadata, Bms::MYO2_AUDIO_MODE) != "STANDARD";
    for ( const auto& sample : map.m_audioSamples )
        if ( (!wholeMusic ||
              Config::utf8ToPath(sample.m_audioResourceId).lexically_normal() !=
                  mainPath) &&
             !contains(sample.m_audioResourceId) )
            return false;
    error.clear();
    return true;
}

/// @brief 将选中谱面、音频与媒体准备到独立事务，再提交完整 ZIP。
/// @param project 仅用于资源定位，整个调用不修改项目数组。
/// @param selectedPaths 用户明确选择的项目相对文件列表。
/// @param outputPath 最终包路径，不允许覆盖项目源资源。
/// @param overrides 作用于转换副本的基础元数据。
/// @param alignmentMode 已由界面互斥选择的对齐策略。
/// @return 任一阶段失败均包含诊断，原始文件保持不变。
/// @note 选中列表是最终包内容的授权边界，不隐式补入未选音频。
/// @note 覆盖元数据只作用于本轮模型，不写回项目中的源谱面。
/// @note 准备阶段保留完整目录关系，输出时才构造相对于 BMS 的资源引用。
/// @note 原始音频只有在完全被主音乐副本替代且无 Key 依赖时才省略。
/// @note 同名目标谱面或生成文件与用户资源冲突时整体拒绝，不覆盖任一来源。
/// @note 最终目标由上层覆盖确认授权，本函数不触发额外的 UI 交互。
/// @warning 低频用户导出路径，包含文件读取、音频编码与压缩。
BmsPackageExportResult BmsPackageExportService::exportPackage(
    const Project& project, const std::vector<std::string>& selectedPaths,
    const std::filesystem::path&                       outputPath,
    const std::vector<PackageBeatmapMetadataOverride>& overrides,
    BmsPackageAlignmentMode alignmentMode, bool myO2Grid)
{
    BmsPackageExportResult result;
    // 空列表不能只产生 ZIP 尾记录，否则 UI 会误报一个没有谱面的成功包。
    // 参数错误在取得任何临时文件所有权前返回。
    if ( selectedPaths.empty() ) {
        result.errorMessage = "没有选择 BMS 谱包内容";
        return result;
    }
    std::error_code error;
    const auto      tempRoot = std::filesystem::temp_directory_path(error);
    if ( error ) {
        result.errorMessage = "无法创建 BMS 临时目录";
        return result;
    }
    // 时间戳只是区分目录候选。是否拥有目录取决于实际创建结果，冲突时不进入清理流程。
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto workPath =
        tempRoot / ("mmm_bms_package_" + std::to_string(stamp));
    if ( !std::filesystem::create_directory(workPath, error) || error ) {
        result.errorMessage = "无法创建 BMS 临时目录";
        return result;
    }
    // 只有目录确实创建成功之后才转交清理守卫，避免删到并发导出的目录。
    // 后续任意 return 都会清理已经生成的音频、BMS 和中间子目录。
    WorkDirectory work{ workPath };
    // selected 保持原始选择身份；entries 只保存最终确实引用的资源与生成文件。
    // 两个表分离，允许一份原音乐被多张谱面转换为不同偏移的独立副本。
    std::map<std::filesystem::path, std::filesystem::path> selected, entries;
    // 同一源文件可同时出现在两个集合，例如一张难度裁切主曲，另一张仍按原始 Key
    // 音引用。 只有从未被直接引用的替代源才能在最终归档中省略。
    std::set<std::filesystem::path> neededAudio, replacedAudio;
    for ( const auto& value : selectedPaths ) {
        const auto relative = Config::utf8ToPath(value).lexically_normal();
        const auto source =
            (project.m_projectRoot / relative).lexically_normal();
        if ( !safeRelative(relative) ||
             !std::filesystem::is_regular_file(source, error) || error ||
             source == outputPath.lexically_normal() ) {
            result.errorMessage = "BMS 谱包包含无效路径：" + value;
            return result;
        }
        // 相同选择去重，但保留完整路径身份，不合并不同目录的同名文件。
        // 检查输出路径不能也是输入，避免用户误把资源包覆盖成原始音频文件。
        selected.emplace(relative, source);
    }
    // 每张谱面的准备相互独立，只有资源引用集合在包级汇总。
    // 源谱面、源音频和最终路径均未打开为写入模式。
    std::size_t charts = 0;
    for ( const auto& [relative, source] : selected ) {
        const auto extension = Config::pathToUtf8(relative.extension());
        if ( !isKnownPackageResourceExtension(PackageResourceType::Beatmap,
                                              extension) )
            continue;
        // 只在白名单谱面来源上调用模型加载器，媒体不会被误识别为 JSON。
        // 加载成功身份由 map_path 判断，不能用 Note 数量排除合法的空谱面。
        auto map = BeatMap::loadFromFile(source);
        if ( map.m_baseMapMetadata.map_path.empty() ) {
            result.errorMessage =
                "无法读取待打包谱面：" + Config::pathToUtf8(relative);
            return result;
        }
        // 元数据按来源相对路径匹配，修改后缀后再匹配会丢掉覆盖身份。
        // BPM、物件与采样不属于基础元数据覆盖，仍来自加载出的原始内容。
        for ( const auto& item : overrides )
            if ( Config::utf8ToPath(item.relativePath).lexically_normal() ==
                 relative )
                map.m_baseMapMetadata = item.baseMeta;
        auto target = relative;
        // 目标统一为 .bms，不根据键数把七轨强制改成 .bme 或长条改成 .bml。
        // 子目录与文件主体保留，关联媒体引用不会因为扁平化而产生重名。
        target.replace_extension(".bms");
        // 按输出而非输入身份检查。例如同目录的同名 .mc 和 .mmm 会转换到同一个
        // .bms。 此时必须报告冲突，否则第二张谱面或它的资源关系会被覆盖。
        if ( entries.contains(target) ) {
            result.errorMessage =
                "多张谱面转换后同名：" + Config::pathToUtf8(target);
            return result;
        }
        // 引用解析必须使用来源路径，而不是临时文件目录或输出谱面的新后缀。
        // 文件名相同的不同目录音效也依赖完整项目身份区分。
        const auto archivedSounds =
            Bms::bmsProperty(map.m_metadata, Bms::MYO2_KEYSOUND);
        // KEY 音包里的名字属于内层归档。STANDARD 模式没有需要裁切的外部主曲，
        // 即使该名字在项目资源索引中有同名候选也不使用它。
        const bool archiveOnly =
            myO2Grid && !archivedSounds.empty() &&
            Bms::bmsProperty(map.m_metadata, Bms::MYO2_AUDIO_MODE) ==
                "STANDARD";
        // STANDARD 的 01 也属于音包命名空间；同名松散文件不应触发音频裁剪。
        const auto* mainResource =
            archiveOnly
                ? nullptr
                : ProjectResourceService::findDefaultBeatmapAudioResource(
                      project, map, source);
        // 指针只在此张谱面容器稳定期间借用，不保存到资源包或下一张谱面。
        // 主音乐必须以解析后的资源身份比较，文件名相同不能证明是同一音频。
        AudioSampleEvent* mainSample = nullptr;
        for ( auto& sample : map.m_audioSamples ) {
            const auto* resource =
                ProjectResourceService::findAudioResourceForReference(
                    project, source, sample.m_audioResourceId);
            if ( !resource || resource != mainResource ||
                 resource->m_type != AudioTrackType::Main )
                continue;
            // 同一 Main 被多次触发不能套用单次整曲裁头，避免改变后续重播片段。
            // 这项检查只针对解析到的默认 Main，其他自动效果采样不受影响。
            if ( mainSample &&
                 alignmentMode == BmsPackageAlignmentMode::RewriteAudio ) {
                result.errorMessage = "BMS 首拍对齐需要唯一的主音乐触发事件";
                return result;
            }
            mainSample = &sample;
        }
        // 歌曲提示可代表旧谱面隐式主音轨，转换时显式生成唯一的 01 事件。
        // 无歌曲提示的纯 Key 谱面不能因为项目恰好有 Main 就被擅自加音乐。
        if ( !mainSample && mainResource &&
             mainResource->m_type == AudioTrackType::Main &&
             !map.m_baseMapMetadata.song_file_hint.empty() ) {
            auto& sample             = map.m_audioSamples.emplace_back();
            sample.m_audioResourceId = mainResource->m_id;
            mainSample               = &sample;
        }
        // 选中的输出模式覆盖来源协议，且必须在首拍对齐前设置。
        // 这是导出副本；标记只影响本次序列化及对齐，原谱面的 MyO2
        // 设置仍由项目保存入口管理。
        auto& format = map.m_metadata.map_properties[MapMetadataType::BMS];
        format["myo2_grid"]    = myO2Grid ? "1" : "0";
        format["myo2_format"]  = myO2Grid ? "1" : "0";
        format["compact_rows"] = "1";
        const auto chartFolder = target.parent_path().empty()
                                     ? std::filesystem::path(".")
                                     : target.parent_path();
        const auto bankValue =
            myO2Grid ? Bms::bmsProperty(map.m_metadata, Bms::MYO2_KEYSOUND)
                     : std::string_view{};
        const bool hasBank = !bankValue.empty();
        // 校验和重写须用同一来源文件。绝对名字先换成项目相对身份，既有相对名字以谱面定位。
        // 项目选择范围不因原谱面记录了一个可访问的路径而扩大。
        if ( hasBank ) {
            // KEY 音包路径按来源谱面定位；ZIP 内部 WAV
            // 名字不能作为松散音频改写。
            auto bank = (relative.parent_path() /
                         Config::utf8ToPath(std::string(bankValue)))
                            .lexically_normal();
            if ( bank.is_absolute() )
                bank = std::filesystem::relative(
                    bank, project.m_projectRoot, error);
            if ( error || !safeRelative(bank) || !selected.contains(bank) ) {
                result.errorMessage =
                    "未选择 MyO2 KEY 音包：" + std::string(bankValue);
                return result;
            }
            if ( !validateMyO2KeySoundBank(
                     map, selected.at(bank), result.errorMessage) )
                return result;
            // 只有外层 KEY ZIP
            // 引用变成新谱面相对路径；压缩包内采样名字保留原文。
            format[std::string(Bms::MYO2_KEYSOUND)] =
                Config::pathToUtf8Generic(bank.lexically_relative(chartFolder));
        }
        if ( myO2Grid && map.m_baseMapMetadata.cover_type == CoverType::VIDEO &&
             !map.m_baseMapMetadata.main_cover_path.empty() ) {
            // 站点按歌曲编号加载
            // MP4；打包保留原文件名，部署时由歌曲库配置编号。
            const auto cover = map.m_baseMapMetadata.main_cover_path;
            auto video = (relative.parent_path() / cover).lexically_normal();
            // 兼容旧工程直接写项目相对媒体路径；优先按谱面解释可以保留同目录同名资源。
            if ( !selected.contains(video) ) video = cover.lexically_normal();
            if ( !selected.contains(video) ||
                 !packageExtensionEquals(Config::pathToUtf8(video.extension()),
                                         ".mp4") ) {
                result.errorMessage = "MyO2 视频 BGA 需要选择对应的 MP4 文件";
                return result;
            }
        }
        // 唯一隐式 Main 已在上方补齐，此后不再增删样本容器。
        // 时间变换先于重映射，以便使用源 ID 和独立 offset 确定真实触发点。
        double audioPhase = 0;
        if ( !alignTimeline(map,
                            mainSample,
                            audioPhase,
                            alignmentMode,
                            result.errorMessage) ) {
            if ( result.errorMessage.empty() )
                result.errorMessage = "BMS 首拍对齐时间无效";
            return result;
        }
        // 净位移为零时不编码，原音乐字节会在普通资源分支原样入包。
        // 非零位移生成以难度命名的副本，避免不同首拍的谱面争用一份音乐。
        std::filesystem::path generatedAudio;
        if ( mainSample && std::abs(audioPhase) > 1.0e-6 ) {
            const auto original =
                Config::utf8ToPath(mainResource->m_path).lexically_normal();
            // 主曲即使可以从项目解析，也必须出现在本轮明确选中的文件列表中。
            // 否则生成的 BMS 会依赖一个用户并未授权放入谱包的音乐文件。
            if ( !selected.contains(original) ) {
                result.errorMessage =
                    "没有选择主音乐资源：" + mainResource->m_path;
                return result;
            }
            // 只重新编码需要时移的主音频；Key 采样保持原始字节和触发关系。
            // 使用 WAV 避免再引入有损编码器前导延迟，目录保持与谱面相邻。
            generatedAudio =
                target.parent_path() /
                (Config::pathToUtf8(target.stem()) + "__bms_main.wav");
            // 生成名称不能遮蔽原先选中的真实资源，即使两者字节可能相同。
            // 另一张谱面准备的条目也不能被后来的难度覆盖。
            if ( selected.contains(generatedAudio) ||
                 entries.contains(generatedAudio) ) {
                result.errorMessage = "BMS 对齐音频文件名冲突：" +
                                      Config::pathToUtf8(generatedAudio);
                return result;
            }
            // 将包内逻辑名称投影到本轮工作目录。作业完成后以该名称入包，
            // 渲染器或播放器无需知道宿主机临时目录的位置。
            const auto generatedPath = work.path / generatedAudio;
            std::filesystem::create_directories(generatedPath.parent_path(),
                                                error);
            if ( error ) {
                result.errorMessage = "无法创建音频输出目录";
                return result;
            }
            // 原始资源来自选中表，目标只位于工作目录，编码器不能覆盖源音乐。
            // 编码失败直接结束本次包，不能继续提交一份只有 BMS 的不完整归档。
            const auto encoded =
                Audio::AudioOriginAlignmentService::alignToOrigin(
                    { .inputPath         = selected.at(original),
                      .outputPath        = generatedPath,
                      .phaseMilliseconds = audioPhase });
            if ( !encoded.success ) {
                result.errorMessage = encoded.errorMessage;
                return result;
            }
            // entries 借用生成文件路径，文件由工作目录守卫存活到压缩完成。
            // replacedAudio 只表示存在替代版本，不表示原音乐已不被 Key 使用。
            entries.emplace(generatedAudio, generatedPath);
            replacedAudio.insert(original);
        }
        // 将所有稳定资源 ID 改成相对于包内谱面的位置，不能泄漏项目内部 ID。
        // 只有主音乐触发指向生成副本；同一原文件绑定的 Key 音仍引用原始资源。
        // 每个字段独立改写，不能全局替换一个 ID 后把 Key 绑定也指向裁过的音乐。
        // 对于未知 ID 或未选择的依赖明确失败，不生成可打开但静音的包。
        // neededAudio 记录真正仍要原文件的引用，供最终去重阶段使用。
        const auto remap = [&](std::string& reference, bool generatedMain) {
            // 空绑定表示静音判定，不能为了寻找文件而把它删除或当成缺失依赖。
            // 非空绑定才承担真实采样定位和入包检查。
            if ( reference.empty() ) return true;
            // 跳过通用 ID
            // 解析，因为生成文件不在项目索引里，只存在于本轮的归档条目集合中。
            if ( generatedMain && !generatedAudio.empty() ) {
                reference =
                    Config::pathToUtf8Generic(generatedAudio.lexically_relative(
                        target.parent_path().empty()
                            ? std::filesystem::path(".")
                            : target.parent_path()));
                return true;
            }
            const auto* resource =
                ProjectResourceService::findAudioResourceForReference(
                    project, source, reference);
            if ( !resource ) {
                result.errorMessage = "无法解析音频资源：" + reference;
                return false;
            }
            const auto path =
                Config::utf8ToPath(resource->m_path).lexically_normal();
            if ( !selected.contains(path) ) {
                result.errorMessage = "没有选择音频资源：" + resource->m_path;
                return false;
            }
            // 将引用定位到目标 BMS 所在目录，父目录引用在包内是合法的。
            // 原文件本身仍以项目相对路径入包，不复制成难度目录下的同名资源。
            neededAudio.insert(path);
            reference = Config::pathToUtf8Generic(path.lexically_relative(
                target.parent_path().empty() ? std::filesystem::path(".")
                                             : target.parent_path()));
            return true;
        };
        // ZIP
        // 音源名已由独立的内层验证覆盖，按项目路径再次解释会把它错误绑定到散装文件。
        if ( !hasBank ) {
            for ( auto& note : map.m_noteData.notes )
                if ( note.m_sampleBinding &&
                     !remap(note.m_sampleBinding->m_audioResourceId, false) )
                    return result;
            for ( auto& hold : map.m_noteData.holds )
                if ( hold.m_sampleBinding &&
                     !remap(hold.m_sampleBinding->m_audioResourceId, false) )
                    return result;
        }
        for ( auto& sample : map.m_audioSamples ) {
            // 比较实际事件身份，不能单看文件名。只有被选为主曲的这次触发改用移相副本。
            const bool isMain = &sample == mainSample;
            if ( (!hasBank || isMain) &&
                 !remap(sample.m_audioResourceId, isMain) )
                return result;
        }
        // MyO2
        // 写出器同时读取歌曲提示和实际触发；两处必须指向同一个最终包内路径。
        if ( myO2Grid && mainSample )
            map.m_baseMapMetadata.song_file_hint =
                Config::utf8ToPath(mainSample->m_audioResourceId);
        // 保存器直到验证全部物件和时间线后才写出，失败不会留下有效归档条目。
        // 同目录结构使外部编辑器解包后也能按相对路径找到采样。
        const auto preparedPath = work.path / target;
        std::filesystem::create_directories(preparedPath.parent_path(), error);
        if ( error || !map.saveToFile(preparedPath) ) {
            result.errorMessage =
                "无法写出 BMS 谱面：" + Config::pathToUtf8(relative);
            return result;
        }
        // 此时仅登记准备结果，真正的 ZIP 写入在全部难度成功后统一执行。
        // charts 按实际写出的谱面计数，只有媒体的选择不构成 BMS 谱包。
        entries.emplace(target, preparedPath);
        if ( myO2Grid ) {
            const auto silence = Bms::myO2SilencePath(preparedPath);
            if ( std::filesystem::is_regular_file(silence, error) ) {
                // STANDARD 松散音源的静音 WAV
                // 和谱面同目录，同目录多个难度共用。
                const auto relativeSilence = Bms::myO2SilencePath(target);
                if ( selected.contains(relativeSilence) ) {
                    result.errorMessage = "MyO2 静音音源名称与包内文件冲突";
                    return result;
                }
                // 复用前一张谱面已记录的同目录静音文件，归档中同一名字只保留一个条目。
                entries.try_emplace(relativeSilence, silence);
            }
            error.clear();
        }
        ++charts;
    }
    // 谱面计数在成功保存后增加；只有失败候选或单纯媒体不能通过这一门槛。
    // 该检查仍先于 ZIP writer 初始化，不留下任何目标归档内容。
    if ( charts == 0 ) {
        result.errorMessage = "没有选择可转换的 BMS 谱面";
        return result;
    }
    for ( const auto& [relative, source] : selected ) {
        const auto extension = Config::pathToUtf8(relative.extension());
        if ( isKnownPackageResourceExtension(PackageResourceType::Beatmap,
                                             extension) )
            continue;
        // 需要原始音频的 Key 绑定和零位移 Main
        // 会登记依赖，其他旧主音频不重复入包。
        // 媒体等非音频选择原样保留，避免把无声图片误判成可以删除的资源。
        if ( isKnownPackageResourceExtension(PackageResourceType::Audio,
                                             extension) &&
             replacedAudio.contains(relative) &&
             !neededAudio.contains(relative) )
            continue;
        // 若普通资源与准备结果发生名称冲突则整个包失败，不能让后加入的媒体遮蔽谱面引用。
        if ( !entries.emplace(relative, source).second ) {
            result.errorMessage = "包内资源重名";
            return result;
        }
    }
    // 所有来源都已确定，才开始创建内存 ZIP，避免转换失败时遗留半包。
    // 按排序后的路径写入，重复选择不会导致条目出现两次。
    // 普通媒体保留字节，新的主音乐与 BMS 读取工作目录中的准备结果。
    ArchiveWriter zip;
    // writer 保存压缩结果，工作副本仍由目录守卫管理，初始化失败不会留下目标
    // ZIP。
    zip.initialized = mz_zip_writer_init_heap(&zip.value, 0, 0) != 0;
    if ( !zip.initialized ) {
        result.errorMessage = "无法初始化 BMS ZIP";
        return result;
    }
    // 复用一个条目读取缓冲，避免把所有未压缩音频同时常驻在额外容器中。
    // ZIP 自己保存压缩输出，源文件的下一次读取可以安全覆盖该缓冲。
    std::vector<unsigned char> bytes;
    for ( const auto& [relative, path] : entries ) {
        // ZIP 协议不接受宿主反斜杠，UTF-8 名称与包内引用使用同一分隔规则。
        // 路径早已检查为安全相对形式，此处不再以 basename 丢弃目录。
        const auto name = Config::pathToUtf8Generic(relative);
        if ( !readFile(path, bytes) ||
             !mz_zip_writer_add_mem(&zip.value,
                                    name.c_str(),
                                    bytes.data(),
                                    bytes.size(),
                                    MZ_DEFAULT_COMPRESSION) ) {
            result.errorMessage = "无法压缩资源：" + name;
            return result;
        }
    }
    // finalize 转交归档缓冲所有权，成功后需要单独调用 mz_free。
    // writer 的内部状态仍由上方守卫结束，二者生命周期不能混为一谈。
    void* buffer = nullptr;
    // miniz
    // 返回的归档大小用于下一次整块写入，不可用最后一个输入条目的长度代替。
    size_t size = 0;
    if ( !mz_zip_writer_finalize_heap_archive(&zip.value, &buffer, &size) ) {
        result.errorMessage = "无法完成 BMS ZIP";
        return result;
    }
    // 全部转换和压缩完成后才打开目标；此前任何失败都不会截断旧包。
    // bytes 是最后一个条目的原始数据，最终提交必须使用 finalize 的归档缓冲。
    // 缓冲写出后立即释放，不把 miniz 分配交给普通 C++ 容器管理。
    // 以文件系统 path 打开目标，避免平台窄字符 API 破坏非 ASCII 路径。
    // 显式关闭后检查流状态，不能把缓冲区尚未刷新当作保存成功。
    std::ofstream output(outputPath, std::ios::binary);
    output.write(static_cast<const char*>(buffer),
                 static_cast<std::streamsize>(size));
    // 检查放在 close 之后，磁盘耗尽等延迟报告的错误不能作为可发布文档交给平台。
    output.close();
    mz_free(buffer);
    result.success = !output.fail();
    if ( !result.success ) result.errorMessage = "无法写入 BMS ZIP";
    // 工作副本完整关闭后才发布授权文档，provider 失败不能报告导出成功。
    // 普通磁盘路径沿用原结果，平台 I/O 仅发生在用户明确导出的低频事务。
    if ( result.success )
        result.success =
            Common::publishDocument(outputPath, result.errorMessage);
    return result;
}
}  // namespace MMM::Logic
