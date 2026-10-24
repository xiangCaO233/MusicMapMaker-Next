#include "logic/BmsPackageExportService.h"

#include "audio/AudioOriginAlignmentService.h"
#include "common/LogicCommands.h"
#include "config/Utf8Path.h"
#include "logic/ProjectResourceService.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/PackageFileTypes.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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
    double phase = std::fmod(first->m_timestamp, beatLength);
    // 事件模式只能后移，不能裁掉音乐中任何已经存在的内容。
    // 向后补到下一整拍，保证原首拍落在从零点开始的同一网格上。
    const bool shiftEvents = mode == BmsPackageAlignmentMode::ShiftEvents;
    if ( shiftEvents && phase > 1.0e-6 ) phase -= beatLength;
    double earliest = std::numeric_limits<double>::infinity();
    // 主音轨由音频内容承担位移；其他事件都必须保留在合法非负时间范围。
    // 若裁头会使早期物件变成负数，则改为补足整数拍静音，网格相位不变。
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
    for ( auto& annotation : map.m_annotations ) shift(annotation.m_timestamp);
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
    BmsPackageAlignmentMode                            alignmentMode)
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
        if ( entries.contains(target) ) {
            result.errorMessage =
                "多张谱面转换后同名：" + Config::pathToUtf8(target);
            return result;
        }
        // 引用解析必须使用来源路径，而不是临时文件目录或输出谱面的新后缀。
        // 文件名相同的不同目录音效也依赖完整项目身份区分。
        const auto* mainResource =
            ProjectResourceService::findDefaultBeatmapAudioResource(
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
        for ( auto& note : map.m_noteData.notes )
            if ( note.m_sampleBinding &&
                 !remap(note.m_sampleBinding->m_audioResourceId, false) )
                return result;
        for ( auto& hold : map.m_noteData.holds )
            if ( hold.m_sampleBinding &&
                 !remap(hold.m_sampleBinding->m_audioResourceId, false) )
                return result;
        for ( auto& sample : map.m_audioSamples )
            if ( !remap(sample.m_audioResourceId, &sample == mainSample) )
                return result;
        // 包目标是标准 BMS/iBMSC，允许叠加稀疏行；这不是额外的文件协议指令。
        // 开关只存在于本次导出副本，原项目与普通另存为策略都不受影响。
        map.m_metadata.map_properties[MapMetadataType::BMS]["compact_rows"] =
            "1";
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
        if ( !entries.emplace(relative, source).second ) {
            result.errorMessage = "包内资源重名";
            return result;
        }
    }
    // 所有来源都已确定，才开始创建内存 ZIP，避免转换失败时遗留半包。
    // 按排序后的路径写入，重复选择不会导致条目出现两次。
    // 普通媒体保留字节，新的主音乐与 BMS 读取工作目录中的准备结果。
    ArchiveWriter zip;
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
    void*  buffer = nullptr;
    size_t size   = 0;
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
    output.close();
    mz_free(buffer);
    result.success = !output.fail();
    if ( !result.success ) result.errorMessage = "无法写入 BMS ZIP";
    return result;
}
}  // namespace MMM::Logic
