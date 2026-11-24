#include "audio/AudioTimelineExportService.h"

#include "audio/AudioTimelineMixerNode.h"
#include "audio/AudioTimelineResourceProcessor.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/dec/IDecoderInstance.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/out/io/FFmpegFileReceiver.hpp>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::Audio
{
namespace
{

/**
 * @file AudioTimelineExportService.cpp
 * @brief 将自动采样混成单个可发布的音频文件。
 *
 * 事件与 PCM 使用不同的身份。路径只决定原始解码片段；路径加 DSP
 * 参数决定处理片段；事件仍有自己的增益、轨道和时间区间。
 * 这样同一个文件多次触发、发生重叠，或同时启用不同速度时，输出不会
 * 因资源复用而合并事件。剪裁和混音仍由时间线的通用调度器处理。
 *
 * 内存大小随 DSP 单配置预算变化，磁盘开销随解码和处理后 PCM 的
 * 总帧数变化。缓存键表、片段表和时间线调度仍随事件数增长。
 * 容器只影响原始解码与最终编码，暂存没有容器元数据，也从不写回源文件。
 *
 * 文件写入分为两步：完整准备 PCM 并编码，再在同一文件系统发布。
 * 创建或读取暂存失败时，已存在的导出仍可用；新的文件也不会以成功路径出现。
 * 工作区只由一次同步调用拥有，并发的导出之间不共享文件游标。
 * 调用方仍须避免将不同任务同时导出到同一个目标文件名。
 */
/// @brief 离线时间线混音和编码拉取容量，不用于音频设备回调。
constexpr std::size_t AUDIO_TIMELINE_EXPORT_CHUNK_FRAMES = 65536U;
/// @brief 解码块与完整缓存的旧实现一致，样本顺序和尾块长度便于复核。
constexpr std::size_t PCM_IO_FRAMES = 2048U;
/// @brief 单次导出的原始及处理后 PCM 共用磁盘额度。
/// @note 仅统计工作区的解码 PCM；最终压缩文件仍受目标文件系统约束。
constexpr std::uint64_t PCM_DISK_BUDGET = 4ULL * 1024U * 1024U * 1024U;
/// @brief 单一处理配置的完整输入及输出声道数组共用内存额度。
/// @note 解码器、DSP 分析器及混音的固定工作缓冲不计入声道数组配额。
constexpr std::size_t PCM_DSP_BUDGET = 256U * 1024U * 1024U;

/// @brief 管理本次导出的独立目录；发布前的暂存文件均随失败回收。
class ExportWorkspace
{
public:
    /// @brief 在输出所在文件系统排他创建暂存目录，允许最后用一次 rename 发布。
    /// @param outputParent 已验证的输出父目录，支持当前目录与 Unicode 路径。
    /// @note 路径文字不包含用户提供的文件名，避免合法长文件名挤占临时目录长度。
    /// @warning 可能阻塞文件系统，调用方应在准备解码前创建工作区。
    explicit ExportWorkspace(const std::filesystem::path& outputParent)
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        // 时间戳只减少冲突；是否拥有路径由 create_directory 的结果决定。
        for ( unsigned attempt = 0; attempt < 16U; ++attempt ) {
            const auto candidate =
                outputParent / (".mmm-audio-export-" + std::to_string(stamp) +
                                "-" + std::to_string(attempt));
            std::error_code error;
            // 创建成功之前整个对象保持空态，析构时不会误删同名目录。
            if ( std::filesystem::create_directory(candidate, error) ) {
                m_path = candidate;
                return;
            }
            // 权限及空间错误无法通过更换候选名称恢复。
            if ( error && error != std::errc::file_exists ) return;
        }
    }

    /// @brief 清理本次拥有的目录；失败时同样不删除用户已有的输出。
    ~ExportWorkspace()
    {
        if ( m_path.empty() ) return;
        std::error_code error;
        // 编码器及声道文件先析构，Windows 上目录清理不依赖删除打开的文件。
        std::filesystem::remove_all(m_path, error);
    }

    ExportWorkspace(const ExportWorkspace&)            = delete;
    ExportWorkspace& operator=(const ExportWorkspace&) = delete;

    /// @brief 借用暂存目录，创建失败时为空。
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    /// @brief 仅在排他创建成功后记录，以免清理其它导出占用的目录。
    std::filesystem::path m_path;
};

/// @brief 暂存文件的连续片段；所有字节偏移均在导出范围内。
/// @note 偏移只能用于当前工作区，同一路径在另一次导出会生成自己的片段。
struct PcmRegion {
    /// @brief 本片段在每个声道文件中的字节起点。
    std::uint64_t offset{};
    /// @brief 解码或 DSP 实际提交的每声道帧数。
    std::size_t frames{};
};

/// @brief 串行导出的 PCM 暂存文件，每个输出声道只打开一个文件。
/// @warning 导出线程独占实例，读写同步定位和访问文件。
///
/// 原始、处理与混音阶段共用一组游标。每次追加前明确定位到尾部，每次
/// 读取前定位到请求片段，文件之间没有分配或写入顺序的并发约定。
class ExportPcmStore
{
public:
    /// @brief 创建固定数量的平面文件；失败原因由 error() 报告。
    /// @param root 本次导出独占的空工作目录。
    /// @param channels 引擎混音格式的非零声道数。
    ExportPcmStore(const std::filesystem::path& root, std::size_t channels)
    {
        // Windows 的 long 可能只有 32 位，存储定位使用标准流的大文件偏移类型。
        static_assert(std::numeric_limits<std::streamoff>::max() >=
                      PCM_DISK_BUDGET);
        m_planes.reserve(channels);
        // 文件数量仅取决于内部音频格式。按声道存储允许将 PCM 直接读入混音缓冲。
        for ( std::size_t channel = 0; channel < channels; ++channel ) {
            auto& plane =
                m_planes.emplace_back(root / ("pcm-" + std::to_string(channel)),
                                      std::ios::in | std::ios::out |
                                          std::ios::binary | std::ios::trunc);
            if ( !plane ) {
                m_error = "无法创建离线音频 PCM 暂存文件";
                return;
            }
        }
    }

    /// @brief 在所有平面的同一字节位置开始追加新片段。
    /// @note 直到片段全部提交之前，不得调用 read 或开启另一片段。
    [[nodiscard]] PcmRegion startRegion()
    {
        // DSP 可能刚从任意片段读取，显式切回追加位置才能保留旧片段。
        for ( auto& plane : m_planes )
            plane.seekp(static_cast<std::streamoff>(m_planeSize));
        return { m_planeSize, 0U };
    }

    /// @brief 追加各平面中的有效帧；单声道来源复制到全部目标平面。
    /// @param region 由 startRegion 返回、尚未完成的片段。
    /// @param planes 借用的源 PCM 声道；调用结束前不会保留这些指针。
    /// @param sourceChannels 解码器或 DSP 实际填充的平面数。
    /// @param frames 这些平面已准备好的每声道帧数。
    /// @return 预算不足或任一声道短写时返回 false；调用方随后放弃整次导出。
    /// @pre planes 指向至少 sourceChannels 个容量均不小于 frames 的声道。
    /// @pre sourceChannels 等于一或目标声道数。
    [[nodiscard]] bool append(PcmRegion& region, const float* const* planes,
                              std::size_t sourceChannels, std::size_t frames)
    {
        const auto planeBudget = PCM_DISK_BUDGET / m_planes.size();
        // 在乘法前判断所有平面的剩余额度，计入每一份 DSP 变体。
        if ( frames > (planeBudget - m_planeSize) / sizeof(float) ) {
            m_error = "离线音频 PCM 暂存超过 4 GiB";
            return false;
        }
        // 输入来自固定解码块或受限 DSP 数组，单次写入可用 streamsize 表达。
        const auto bytes = static_cast<std::streamsize>(frames * sizeof(float));
        for ( std::size_t channel = 0; channel < m_planes.size(); ++channel ) {
            const auto source = sourceChannels == 1U ? 0U : channel;
            if ( !m_planes[channel].write(
                     reinterpret_cast<const char*>(planes[source]), bytes) ) {
                m_error = "写入离线音频 PCM 暂存失败";
                return false;
            }
        }
        // 只有所有平面都提交后，才更新连续范围和有效帧数。
        m_planeSize += static_cast<std::uint64_t>(bytes);
        region.frames += frames;
        return true;
    }

    /// @brief 随机访问已完成的片段；调用方保证范围和缓冲容量有效。
    /// @param region 已提交的源片段。
    /// @param planes 按引擎格式提供的独立目标平面。
    /// @param start 相对片段开头的起始帧。
    /// @param frames 本次必须完整读出的帧数。
    /// @return 任一声道短读后锁存失败；之后的混音读取不再依赖文件状态。
    [[nodiscard]] bool read(PcmRegion region, float* const* planes,
                            std::size_t start, std::size_t frames)
    {
        if ( m_error ) return false;
        const auto offset =
            static_cast<std::streamoff>(region.offset + start * sizeof(float));
        const auto bytes = static_cast<std::streamsize>(frames * sizeof(float));
        // 同一个片段可以重叠播放或逆序访问，不能沿用上次读取时的游标。
        for ( std::size_t channel = 0; channel < m_planes.size(); ++channel ) {
            auto& plane = m_planes[channel];
            if ( !plane.seekg(offset).read(
                     reinterpret_cast<char*>(planes[channel]), bytes) ) {
                m_error = "读取离线音频 PCM 暂存失败";
                return false;
            }
        }
        return true;
    }

    /// @brief 在启动编码前提交残余写缓冲，及早报告空间不足等 IO 失败。
    /// @note 刷新只要求后续读取可见；暂存只服务当前进程，不用于崩溃恢复。
    [[nodiscard]] bool finishWriting()
    {
        for ( auto& plane : m_planes ) {
            if ( !plane.flush() ) {
                m_error = "写入离线音频 PCM 暂存失败";
                return false;
            }
        }
        return true;
    }

    /// @brief 返回本次导出首次遇到的暂存错误；无错时为空指针。
    /// @note 返回静态消息，导出结果可在工作区销毁后使用它构造诊断。
    [[nodiscard]] const char* error() const { return m_error; }

private:
    /// @brief 相同位置对应同一时间帧，混音前后共用同一组文件句柄。
    std::vector<std::fstream> m_planes;
    /// @brief 单个平面的已提交字节数，所有平面始终等长。
    std::uint64_t m_planeSize{};
    /// @brief 暂存错误通过这里传播到导出结果，不能按缺页静音发布。
    const char* m_error{};
};

/// @brief 同一份暂存片段可由任意数量的自动采样事件引用。
class StoredTimelinePcm final : public OfflineTimelinePcm
{
public:
    /// @brief 借用导出的暂存文件；整个 MixerNode 必须先于文件销毁。
    /// @param store 由同步导出函数管理的平面文件。
    /// @param region 已提交且不会再修改的 PCM 片段。
    StoredTimelinePcm(ExportPcmStore& store, PcmRegion region)
        : m_store(store), m_region(region)
    {
    }

    /// @brief 同步读取片段与目标缓冲共有的帧数。
    /// @param buffer 与暂存声道数一致的混音缓冲。
    /// @param start 相对片段而非整条时间线的起点。
    /// @param frames 调度要求的区间长度；不能越过片段末尾。
    /// @return 全部可读帧数；暂存失败由服务在编码完成后读取。
    /// @warning 只通过离线 MixerNode 路径进入，不运行在设备回调中。
    std::size_t read(ice::AudioBuffer& buffer, std::size_t start,
                     std::size_t frames) const noexcept override
    {
        if ( start >= m_region.frames ||
             buffer.num_channels() !=
                 ice::ICEConfig::internal_format.channels ||
             !buffer.raw_ptrs() )
            return 0U;
        // 跨片段末尾时只填充有效样本，剩余部分由混音节点维持为零。
        const auto count =
            std::min({ frames, m_region.frames - start, buffer.num_frames() });
        return m_store.read(m_region, buffer.raw_ptrs(), start, count) ? count
                                                                       : 0U;
    }

private:
    /// @brief 暂存文件由同步导出调用拥有，生命周期长于全部片段。
    ExportPcmStore& m_store;
    /// @brief 收录配置处理完成后的确定帧数，时间线使用它计算结尾。
    const PcmRegion m_region;
};

/// @brief 顺序解码实际有效帧，复用经过 ICE 解码器转换的目标采样率。
/// @param store 当前原始 PCM 索引所对应的平面文件。
/// @param path 按项目音频接口约定传递的 UTF-8 路径。
/// @param format 引擎内部声道布局与采样率。
/// @return 空片段表示资源打不开、输出格式异常或写入失败。
/// @note 后端零帧结束正常读取，与 ICE 旧缓存一样无法从零帧单独判断解码错误。
PcmRegion decodeToStore(ExportPcmStore& store, std::string_view path,
                        const ice::AudioDataFormat& format)
{
    const ice::FFmpegDecoderFactory factory;
    // 使用与 ICE 完整缓存相同的工厂和起始定位，保留编解码器已有的预卷语义。
    auto decoder = factory.create_instance(path, format);
    if ( !decoder || !decoder->seek(0U) ) return {};
    const auto& decodedFormat = decoder->get_source_format();
    // 保留完整缓存原有的单声道扩展规则，不在导出层做重采样或矩阵混音。
    if ( decodedFormat.samplerate != format.samplerate ||
         (decodedFormat.channels != format.channels &&
          decodedFormat.channels != 1U) )
        return {};

    // 按后端实报布局分配固定块，单声道不会在解码时访问第二个指针。
    ice::AudioBuffer buffer(decodedFormat, PCM_IO_FRAMES);
    if ( buffer.num_frames() != PCM_IO_FRAMES || !buffer.raw_ptrs() ) return {};
    auto region = store.startRegion();
    // 短块也可能是有效数据；只有解码器返回零才终止这条流。
    while ( const auto frames =
                decoder->read(buffer.raw_ptrs(), PCM_IO_FRAMES) ) {
        // FFmpeg 的时长只用于估计，末尾长度由实际解码帧决定。
        if ( frames > PCM_IO_FRAMES ||
             !store.append(
                 region, buffer.raw_ptrs(), decodedFormat.channels, frames) )
            return {};
    }
    return region;
}

/// @brief 装入并处理当前配置，落盘后立即释放整段 DSP 输入与输出。
/// @param store 同时保存原始和处理后平面；原片段在处理过程中不变。
/// @param source 尚未应用当前配置的原始 PCM。
/// @param channels 引擎混音格式的声道数，与暂存一致。
/// @param config 本资源的持久化 EQ、变速和变调设置。
/// @return 空片段表示超预算、处理失败或暂存失败。
/// @note 内存预算与磁盘预算独立；处理完的 PCM
/// 在此同步释放，下一配置可以复用额度。
PcmRegion processStoredResource(ExportPcmStore& store, PcmRegion source,
                                std::size_t             channels,
                                const AudioTrackConfig& config)
{
    // 输入自身都无法容纳时，在构建任何完整声道数组之前返回失败。
    if ( source.frames > PCM_DSP_BUDGET / sizeof(float) / channels ) return {};
    std::vector<std::vector<float>> pcm(channels);
    std::vector<float*>             input(channels);
    std::vector<const float*>       output(channels);

    // 解码或其它配置的数据仍在磁盘，这里只装入本次 EQ/变速所需的平面。
    for ( std::size_t channel = 0U; channel < channels; ++channel ) {
        pcm[channel].resize(source.frames);
        input[channel] = pcm[channel].data();
    }
    // 整条输入就绪后才启动资源处理，失败时没有变体会进入混音调度。
    if ( !store.read(source, input.data(), 0U, source.frames) ||
         !processExportTimelineResource(pcm, config, PCM_DSP_BUDGET) )
        return {};

    // 变速可能替换全部平面，必须在处理完成后重新取得每个声道的位置。
    for ( std::size_t channel = 0U; channel < channels; ++channel )
        output[channel] = pcm[channel].data();
    auto result = store.startRegion();
    if ( !store.append(result, output.data(), channels, pcm.front().size()) )
        return {};
    return result;
}

/// @brief 将秒数安全转换为统一时间线帧。
/// @param seconds 秒数，允许为负。
/// @return 限制在 AudioTimelineFrame 可表达范围内的最近帧。
///
/// 使用 long double 完成乘法，推迟大秒数在转换前丢失精度的时点。llround 采用
/// 最近帧而非截断，使正负小数秒在时间线原点两侧保持对称舍入。
AudioTimelineFrame secondsToTimelineFrame(double seconds) noexcept
{
    // 非有限时间没有可用排序语义，安全回退到时间线原点。
    if ( !std::isfinite(seconds) ) return 0;

    const long double frames =
        static_cast<long double>(seconds) *
        static_cast<long double>(ice::ICEConfig::internal_format.samplerate);
    constexpr auto MIN_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::min());
    constexpr auto MAX_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::max());
    // 在转换回整数前饱和，避免 llround 的输入越出返回类型范围。
    if ( frames <= MIN_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::min();
    }
    if ( frames >= MAX_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::max();
    }
    return static_cast<AudioTimelineFrame>(std::llround(frames));
}

/// @brief 规范化项目音频资源音量。
/// @param volume 持久化线性音量。
/// @return 有限的零到一音量。
///
/// 资源音量来自项目配置，公开语义为百分比；异常值选择最近合法边界或零。
float sanitizedResourceVolume(float volume) noexcept
{
    // 项目资源音量是百分比语义，不允许事件文件把它放大到一以上。
    return std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 0.0F;
}

/// @brief 规范化单个事件音量。
/// @param volume 事件线性音量。
/// @return 有限的非负音量。
///
/// 事件格式可能允许大于一的强调倍率，因此这里只限制下界，不截去合法放大。
float sanitizedEventVolume(float volume) noexcept
{
    // 事件层允许正向放大，但负值和非有限值都按静音处理。
    return std::isfinite(volume) ? std::max(volume, 0.0F) : 0.0F;
}

}  // namespace

/// @brief 把全部自动采样事件离线混合并编码为一个音频文件。
/// @param options 事件、谱面结束时间、输出路径与阶段进度回调。
/// @return 编码成功状态、诊断文本和实际写出帧数。
///
/// 原始 PCM 按路径去重，处理结果按路径及规范前的 DSP 参数去重。
/// 两者只在暂存文件保留长数据，重复事件仍各自维护位置、音量及轨道。
/// 空事件列表按谱面长度导出静音，零长度时间线输出最小的一帧。
/// @warning 用户触发的同步导出路径，允许文件系统、解码、DSP 和编码。
AudioTimelineExportResult AudioTimelineExportService::exportMixedAudio(
    const AudioTimelineExportOptions& options)
{
    // 默认失败且帧数为零，只有编码器确实写出数据后才提交 success。
    AudioTimelineExportResult result;
    if ( options.outputPath.empty() ) {
        result.errorMessage = "音频输出路径为空";
        return result;
    }

    // 整条图统一使用内部格式，避免事件之间发生不同采样率或声道布局混合。
    const ice::AudioDataFormat format = ice::ICEConfig::internal_format;
    if ( format.channels == 0U || format.samplerate == 0U ) {
        result.errorMessage = "音频引擎内部格式无效";
        return result;
    }

    // 使用 error_code 路径表达失败，符合项目禁用异常的约束。
    std::error_code filesystemError;
    if ( !options.outputPath.parent_path().empty() ) {
        std::filesystem::create_directories(options.outputPath.parent_path(),
                                            filesystemError);
        if ( filesystemError ) {
            result.errorMessage = "无法创建音频输出目录";
            return result;
        }
    }

    // 进度回调只在阶段边界调用，不从音频拉取或编码内部热循环调用。
    if ( options.progress ) options.progress("正在解码音频资源…");
    // 相对文件名没有显式父目录，暂存仍必须与目标文件位于同一个文件系统。
    const ExportWorkspace workspace(options.outputPath.parent_path().empty()
                                        ? std::filesystem::path(".")
                                        : options.outputPath.parent_path());
    if ( workspace.path().empty() ) {
        result.errorMessage = "无法创建离线音频导出的暂存目录";
        return result;
    }
    ExportPcmStore store(workspace.path(), format.channels);
    if ( store.error() ) {
        result.errorMessage = store.error();
        return result;
    }
    // 工作区、文件句柄、片段与编码器按依赖顺序构造，随后逆序释放。
    std::unordered_map<std::string, PcmRegion> rawByPath;
    // 同一路径只解码一次，索引中只记录 PCM 的偏移和实际帧数。
    rawByPath.reserve(options.events.size());
    // 第一遍只处理文件身份；事件位置、音量和 DSP 配置留到第二遍。
    for ( const auto& event : options.events ) {
        // 空路径保留 resourceKey 进入诊断，便于定位未解析的谱面资源引用。
        if ( event.filePath.empty() ) {
            result.errorMessage = "存在未解析的音频资源：" + event.resourceKey;
            return result;
        }
        // 同一路径的多个事件共享同一暂存片段，但仍在下一阶段生成独立片段。
        if ( rawByPath.contains(event.filePath) ) continue;

        const auto path = Config::utf8ToPath(event.filePath);
        filesystemError.clear();
        // 目录或特殊文件都不能交给解码器，统一按资源缺失处理。
        if ( !std::filesystem::is_regular_file(path, filesystemError) ||
             filesystemError ) {
            result.errorMessage = "找不到音频资源：" + event.filePath;
            return result;
        }
        const auto region = decodeToStore(store, event.filePath, format);
        if ( region.frames == 0U ) {
            result.errorMessage = store.error()
                                      ? store.error()
                                      : "无法解码音频资源：" + event.filePath;
            return result;
        }
        // 原缓存不受后续资源级变速影响，可继续服务中性配置与其它变体。
        rawByPath.emplace(event.filePath, region);
    }

    if ( options.progress ) options.progress("正在处理音频时间线…");
    std::unordered_map<std::string,
                       std::shared_ptr<const PreparedTimelineAudio>>
        preparedByConfig;
    // 路径相同但处理配置不同必须生成不同缓存，因此使用处理键而非仅用路径。
    // 第二遍按处理配置共享不可变 PCM，再为每个事件保留独立时间线身份。
    preparedByConfig.reserve(options.events.size());
    // 事件逐一装入调度；配置相同仍允许重复触发或出现在不同 BGM 轨。
    std::vector<PreparedTimelineClip> clips;
    clips.reserve(options.events.size());
    for ( const auto& event : options.events ) {
        // 事件起点异常会破坏排序与结束帧计算，不能静默回退到零。
        if ( !std::isfinite(event.effectiveStartSeconds) ) {
            result.errorMessage = "音频事件起播时间无效：" + event.resourceKey;
            return result;
        }

        const auto source = rawByPath.find(event.filePath);
        if ( source == rawByPath.end() ) {
            result.errorMessage = "音频资源未完成载入：" + event.filePath;
            return result;
        }

        // 音量留给事件混合，只有真正改变 PCM 的配置会写入新片段。
        const auto processingKey = makeAudioResourceProcessingCacheKey(
            event.filePath, event.resourceConfig);
        auto preparedIterator = preparedByConfig.find(processingKey);
        if ( preparedIterator == preparedByConfig.end() ) {
            // 默认直接引用原始片段，只有 DSP 生效时才读回声道数组。
            auto region = source->second;
            if ( audioTimelineResourceNeedsProcessing(event.resourceConfig) ) {
                region = processStoredResource(
                    store, region, format.channels, event.resourceConfig);
            }
            if ( region.frames == 0U ) {
                result.errorMessage =
                    store.error()
                        ? store.error()
                        : "无法处理音频资源（单配置 PCM 上限 256 MiB）：" +
                              event.filePath;
                return result;
            }
            auto pcm      = std::make_shared<StoredTimelinePcm>(store, region);
            auto prepared = std::make_shared<const PreparedTimelineAudio>(
                std::move(pcm), region.frames);
            preparedIterator =
                preparedByConfig.emplace(processingKey, std::move(prepared))
                    .first;
        }

        // 资源静音在混音音量层实现，不妨碍共享同一份已处理 PCM。
        const float resourceVolume =
            event.resourceConfig.muted
                ? 0.0F
                : sanitizedResourceVolume(event.resourceConfig.volume);
        // 起点允许为负；MixerNode 会在零点输出时裁去相应源前缀。
        clips.push_back(PreparedTimelineClip{
            .eventId    = event.eventId,
            .sourceKey  = event.resourceKey,
            .startFrame = secondsToTimelineFrame(event.effectiveStartSeconds),
            .bgmTrackIndex = event.bgmTrackIndex,
            .volume = resourceVolume * sanitizedEventVolume(event.eventVolume),
            .audio  = preparedIterator->second,
        });
    }

    // 提前暴露缓冲写入错误，避免编码器在混音阶段才读到不完整的片段。
    if ( !store.finishWriting() ) {
        result.errorMessage = store.error();
        return result;
    }

    // 无效或负谱面结束时间按零处理，实际片段尾部仍可扩展复合结束帧。
    const double chartEndSeconds = std::isfinite(options.chartEndSeconds)
                                       ? std::max(options.chartEndSeconds, 0.0)
                                       : 0.0;
    // 节点构造会过滤空资源并把结束位置扩展到最晚片段尾部。
    auto timeline = std::make_shared<AudioTimelineMixerNode>(
        std::move(clips),
        secondsToTimelineFrame(chartEndSeconds),
        AUDIO_TIMELINE_EXPORT_CHUNK_FRAMES,
        nullptr,
        true);
    // 离线也要显式应用播放命令，否则首次拉取只会返回静音。
    timeline->play();

    // 编码器至少拉取一帧，空时间线也能生成格式合法的最小输出。
    const auto timelineEndFrame = timeline->timelineEndFrame();
    const auto positiveEndFrame =
        timelineEndFrame > 0 ? static_cast<std::uint64_t>(timelineEndFrame)
                             : std::uint64_t{ 1U };
    const auto targetFrames = static_cast<std::size_t>(std::min(
        positiveEndFrame,
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())));

    // Receiver 按目标帧数停止拉取，不依赖节点结束后的额外空 block。
    // 这个长度只限制最终时间线，并不改变同一资源被多个片段共同引用的区间。
    // 扩展名必须与目标一致，供 FFmpeg 选择容器；完整编码后再发布。
    auto encodedPath = workspace.path() / "encoded";
    encodedPath += options.outputPath.extension();
    ice::FFmpegFileReceiver receiver(encodedPath, format);
    receiver.set_source(timeline);
    receiver.set_target_frames(targetFrames);
    // 与 MixerNode scratch 使用同一块大小，避免 Receiver 请求超过预备容量。
    receiver.set_block_frames(AUDIO_TIMELINE_EXPORT_CHUNK_FRAMES);
    if ( options.progress ) options.progress("正在混音并转码音频…");
    // start 同步完成，返回时已关闭编码器输出句柄，可以在 Windows 上重命名。
    const bool encoded = receiver.start();
    // 混音接口只返回样本，源暂存错误优先从独立的状态读取。
    if ( store.error() ) {
        result.errorMessage = store.error();
        return result;
    }
    if ( !encoded ) {
        result.errorMessage = receiver.error_message().empty()
                                  ? "无法编码拼装后的音频"
                                  : receiver.error_message();
        return result;
    }
    if ( receiver.frames_written() == 0U ) {
        result.errorMessage = "音频编码器未写出任何数据";
        return result;
    }

    // 暂存读失败时 MixerNode 只能返回短读，绝不将带补零的结果替换目标文件。
    std::filesystem::rename(encodedPath, options.outputPath, filesystemError);
    // 目标被占用或缺少权限时，保留先前的目标并由工作区守卫回收本次产物。
    if ( filesystemError ) {
        result.errorMessage =
            "无法发布音频输出文件：" + filesystemError.message();
        return result;
    }
    // 发布成功后才提交帧数，调用方不会拿失败产物计算时长或更新项目引用。
    result.outputFrames = receiver.frames_written();
    result.success      = true;

    // 成功日志使用 UTF-8 路径，保持跨平台输出可读。
    XINFO("AudioTimelineExportService: wrote {} frames to {}",
          result.outputFrames,
          Config::pathToUtf8(options.outputPath));
    return result;
}

}  // namespace MMM::Audio
