#include "audio/StemAudition.h"

#include "audio/AudioTimelineMixerNode.h"
#include "audio/StemPlaybackStorage.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/thread/ThreadPool.hpp>


#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace MMM::Audio
{
/// @brief 复制当前非实时状态；首次准备期间返回原版的默认状态。
std::shared_ptr<const StemPlaybackMix> StemPlaybackSource::mix() const
{
    static const auto     empty = std::make_shared<const StemPlaybackMix>();
    const std::lock_guard lock(m_mixMutex);
    return m_current ? m_current : empty;
}

/// @brief 发布由用户调整或新任务准备的完整调音台状态。
bool StemPlaybackSource::publish(StemPlaybackMix mix)
{
    if ( !mix.catalog ) return false;
    return publishMix(std::move(mix), false);
}

/// @brief 以声部目录身份校验编辑仍适用于当前代次。
bool StemPlaybackSource::publishSettings(StemPlaybackMix mix)
{
    if ( !mix.catalog ) return false;
    return publishMix(std::move(mix), false, true);
}

/// @brief 文件任务竞争同一音轨时立即返回忙，避免阻塞线程池。
std::unique_lock<std::mutex> StemPlaybackSource::tryLockCache()
{
    return std::unique_lock(m_cacheMutex, std::try_to_lock);
}

/// @brief 删除成功后取消实时替代，保留空快照阻止在途恢复重新发布。
std::error_code StemPlaybackSource::deleteCachedPlayback(
    const std::string& filePath, std::string_view modelKey)
{
    const auto lock = tryLockCache();
    if ( !lock )
        return std::make_error_code(std::errc::device_or_resource_busy);
    const auto removed = removeStemPlaybackCache(filePath, modelKey);
    if ( !removed ) return removed.error();
    const auto current = mix();
    // 阻止正在解码已删模型的恢复任务重新发布，同时保留其他活动模型。
    if ( (current->catalog && current->cacheModelKey == modelKey) ||
         (!current->catalog && *removed) )
        static_cast<void>(publishMix({}, false));
    return {};
}

/// @brief 载入目标模型自身的推子，启用实时分轨并保存重启选择。
std::error_code StemPlaybackSource::activateCachedPlayback(
    const std::string& filePath, std::string_view modelKey,
    ice::ThreadPool& decodePool)
{
    const auto lock = tryLockCache();
    if ( !lock )
        return std::make_error_code(std::errc::device_or_resource_busy);
    // 之前离开的模型可能还有待落盘的推子，先保存再重新解码其缓存。
    saveQueuedSettings();
    auto loaded = loadStoredStemPlayback(filePath, decodePool, modelKey);
    if ( !loaded )
        return std::make_error_code(std::errc::no_such_file_or_directory);
    loaded->useStems = true;
    if ( !selectStoredStemPlayback(filePath, *loaded) ||
         !publish(std::move(*loaded)) )
        return std::make_error_code(std::errc::io_error);
    return {};
}

/// @brief 串行发布控制快照，恢复作业只填充仍为空的来源。
bool StemPlaybackSource::publishMix(StemPlaybackMix mix, bool onlyIfEmpty,
                                    bool settingsOnly)
{
    if ( mix.catalog ) {
        const auto count = mix.catalog->tracks().size();
        if ( count == 0 || mix.gains.size() != count ||
             mix.enabled.size() != count || (mix.solo && *mix.solo >= count) )
            return false;
    }
    for ( auto& gain : mix.gains )
        gain = std::isfinite(gain) ? std::clamp(gain, 0.0F, 1.0F) : 0.0F;

    auto next = std::make_shared<const StemPlaybackMix>(std::move(mix));
    std::shared_ptr<const StemPlaybackMix>              previous;
    std::vector<std::shared_ptr<const StemPlaybackMix>> reclaimed;
    {
        const std::lock_guard lock(m_mixMutex);
        if ( onlyIfEmpty && m_current ) return false;
        // 文件线程可能刚删除或替换了快照，界面不能把手中的旧目录装回。
        if ( settingsOnly &&
             (!m_current || m_current->catalog != next->catalog) )
            return false;
        // 先断开旧指针，再检查读者。顺序一致保证仍可持有旧指针的读者
        // 必然在本次检查之前进入读区。
        m_active.store(next->useStems ? next.get() : nullptr);
        previous = std::exchange(m_current, std::move(next));
        // 保存操作可能在另一模型已发布后排队，先记下编辑所属的物理文件。
        if ( settingsOnly && !m_current->settingsPath.empty() )
            m_pendingSettings.insert_or_assign(m_current->settingsPath,
                                               m_current);
        if ( m_readers.load() == 0U ) {
            reclaimed.swap(m_retired);
        } else if ( previous ) {
            m_retired.push_back(std::move(previous));
        }
    }
    // 退役目录可能拥有整首歌的 PCM，不能在界面读取的短锁内析构。
    return true;
}

/// @brief 同源准备任务只读取一次本地结果，且不会将旧文件覆盖新任务结果。
void StemPlaybackSource::restore(const std::string& filePath,
                                 ice::ThreadPool&   decodePool)
{
    std::call_once(m_restored, [&] {
        if ( mix()->catalog ) return;
        if ( auto restored = loadStoredStemPlayback(filePath, decodePool) )
            static_cast<void>(publishMix(std::move(*restored), true));
    });
}

/// @brief 每个来源只安排一份后台写入任务，任务持有来源直到保存完成。
void StemPlaybackSource::queueSettingsSave()
{
    auto* files  = Runtime::AppThreadPool::instance().getFileThreadPool();
    auto  source = weak_from_this().lock();
    if ( !files || !source ) return;
    {
        const std::lock_guard lock(m_mixMutex);
        if ( m_current && !m_current->settingsPath.empty() )
            m_pendingSettings.insert_or_assign(m_current->settingsPath,
                                               m_current);
        if ( m_saveQueued || m_pendingSettings.empty() ) return;
        m_saveQueued = true;
    }
    auto job = files->enqueue(
        [source = std::move(source)] { source->saveQueuedSettings(); });
    if ( !job.valid() ) {
        const std::lock_guard lock(m_mixMutex);
        m_saveQueued = false;
    }
}

/// @brief 磁盘只接收有序快照；保存期间的变化合并到最新一次状态。
void StemPlaybackSource::saveQueuedSettings()
{
    const std::lock_guard writer(m_settingsWriteMutex);
    for ( ;; ) {
        decltype(m_pendingSettings) pending;
        {
            const std::lock_guard lock(m_mixMutex);
            if ( m_pendingSettings.empty() ) {
                m_saveQueued = false;
                return;
            }
            pending.swap(m_pendingSettings);
        }
        // 不同缓存各用自己的配置文件，新编辑只合并同一个文件的更新。
        for ( const auto& entry : pending )
            if ( !storeStemPlaybackSettings(*entry.second) )
                XWARN("Could not save stem playback settings");
    }
}

/// @brief 只对实际启用分轨的来源进入读区，避免原版主轨走原子读改写。
/// @warning 音频热路径。首次查询仅为空值测试，进入读区后重新获取指针。
StemPlaybackSource::Read::Read(const StemPlaybackSource* source) noexcept
{
    if ( !source || !source->m_active.load(std::memory_order_relaxed) ) return;
    m_source = source;
    m_source->m_readers.fetch_add(1U);
    m_mix = m_source->m_active.load();
}

/// @brief 离开读区，最后一个读者也仅修改计数，不析构数据。
/// @warning 音频热路径；释放所有权由 UI 在后续发布时执行。
StemPlaybackSource::Read::~Read()
{
    if ( m_source ) m_source->m_readers.fetch_sub(1U);
}

/// @brief 读取已借用的完整组合，生命周期不超过读区。
const StemPlaybackMix* StemPlaybackSource::Read::get() const noexcept
{
    return m_mix;
}

/// @brief 接管经过完整验证的目录。
/// @details 输出长度取原始资源时间轴，任一声部的实际长度可更短。
/// 轨道所有者随目录保留，发布到播放节点后由各调度状态独立持有。
StemAudition::StemAudition(AudioTimelineFrame             sourceEndFrame,
                           std::vector<StemAuditionTrack> tracks)
    : m_sourceEndFrame(sourceEndFrame), m_tracks(std::move(tracks))
{
}

/// @brief 验证每个声部都可按共享源帧定位并在回调中读取。
/// @details 只接收已驻留的 PCM。流式源在实时读时可能缺页，而从相同
/// 位置离线读却能补页，造成编辑器试听和区间摘录的结果不同。
/// @return 校验成功时转移所有音轨，否则返回首个错误。
std::expected<StemAudition, StemAuditionError> StemAudition::create(
    AudioTimelineFrame sourceEndFrame, std::vector<StemAuditionTrack> tracks)
{
    if ( sourceEndFrame <= 0 ) {
        return std::unexpected(StemAuditionError::InvalidInterval);
    }

    // 建组时验证坐标和格式，后续的区间取样直接复用播放节点的调度。
    // 唯一性只针对当前目录。同一源文件的不同分离版本可拥有不同 ID。
    std::unordered_set<std::string> ids;
    for ( const auto& track : tracks ) {
        // 有效起点的检查必须先于长度相减，确保差值为正且可表示。
        // channel(0) 的连续视图用于拒绝仅有文件或流式缓存的资源。
        if ( track.id.empty() || !track.audio || track.startFrame < 0 ||
             track.startFrame >= sourceEndFrame ||
             track.audio->numFrames() == 0 ||
             track.audio->numChannels() !=
                 ice::ICEConfig::internal_format.channels ||
             track.audio->channel(0).size() != track.audio->numFrames() ||
             std::cmp_greater(track.audio->numFrames(),
                              sourceEndFrame - track.startFrame) ) {
            return std::unexpected(StemAuditionError::InvalidTrack);
        }
        if ( !ids.insert(track.id).second ) {
            return std::unexpected(StemAuditionError::DuplicateTrack);
        }
    }
    // 目录是不可变快照，后续切换只发布选择，不移动或覆盖源 PCM。
    return StemAudition(sourceEndFrame, std::move(tracks));
}

/// @brief 打开已对齐的多轨文件，统一由引擎转换成内部 PCM。
/// @details 导入推迟至文件池。解码任务使用独立池，避免等待同一池的任务。
/// 局部 AudioPool 只负责准备和去重，返回的各轨道继续拥有 PCM。
/// @warning 低频后台入口。路径由调用方经文件选择器或授权来源取得。
std::expected<StemAudition, StemAuditionError> StemAudition::loadAlignedFiles(
    std::span<const std::string> paths, ice::ThreadPool& decodePool)
{
    ice::AudioPool                 pool;
    std::vector<StemAuditionTrack> tracks;
    tracks.reserve(paths.size());
    AudioTimelineFrame end = 0;
    for ( const auto& path : paths ) {
        // 固定缓存策略，保证 UI 正在播放时摘录同一区间仍读取相同样本。
        auto track =
            pool.get_or_load(decodePool, path, ice::CachingStrategy::CACHY)
                .lock();
        // fromTrack 取得固定帧数与声道视图，解码完成前不会把轨道暴露给设备。
        auto audio = PreparedTimelineAudio::fromTrack(std::move(track));
        if ( !audio ||
             !std::in_range<AudioTimelineFrame>(audio->numFrames()) ) {
            return std::unexpected(StemAuditionError::DecodeFailed);
        }
        end =
            std::max(end, static_cast<AudioTimelineFrame>(audio->numFrames()));
        // 文件组按源帧零点对齐。短文件结束后由公共调度补静音。
        tracks.push_back(
            { .id = path, .startFrame = 0, .audio = std::move(audio) });
    }
    // 复用内存输入的边界和 ID 校验，不为文件输入引入放宽后的规则。
    return create(end, std::move(tracks));
}

/// @brief 以借用视图公开目录。
/// @details 保留登记顺序，调用方可用 ID 绑定按钮和未来的区间请求。
/// 返回视图只在目录生命周期内有效。
std::span<const StemAuditionTrack> StemAudition::tracks() const noexcept
{
    return m_tracks;
}

/// @brief 返回可用于 seek、循环与摘录的统一排除端点。
AudioTimelineFrame StemAudition::endFrame() const noexcept
{
    // 该终点独立于当前选择，空混音仍能按原曲范围连续取样。
    return m_sourceEndFrame;
}

/// @brief 把音轨集合解析成播放节点的输入。
/// @details 这里不接受重复轨道，避免重复请求导致额外增益。
/// 播放节点按时间与事件编号排序，所以用目录顺序确定同拍片段的编号。
/// 选空时返回空调度，由调用者保留原曲终点。
std::expected<std::vector<PreparedTimelineClip>, StemAuditionError>
StemAudition::makeClips(std::span<const std::string> ids,
                        std::span<const float>       gains) const
{
    // 先验证完整请求，再按注册顺序建图。输入 ID 排列不改变混音结果。
    std::unordered_set<std::string_view> selected(ids.begin(), ids.end());
    if ( selected.size() != ids.size() ) {
        return std::unexpected(StemAuditionError::DuplicateTrack);
    }

    std::vector<PreparedTimelineClip> clips;
    clips.reserve(ids.size());
    // 推子按目录索引，静音和独听时仍保留各轨原设定。
    if ( !gains.empty() && gains.size() != m_tracks.size() ) {
        return std::unexpected(StemAuditionError::InvalidTrack);
    }
    for ( std::size_t index = 0; index < m_tracks.size(); ++index ) {
        const auto& track = m_tracks[index];
        const float gain  = gains.empty() ? 1.0F : gains[index];
        // 静音选轨与推子增益彼此独立，未选中的轨也必须满足输入契约。
        if ( !std::isfinite(gain) || gain < 0.0F || gain > 1.0F ) {
            return std::unexpected(StemAuditionError::InvalidTrack);
        }
        if ( selected.contains(track.id) ) {
            clips.push_back({
                .eventId    = clips.size() + 1,
                .sourceKey  = track.id,
                .startFrame = track.startFrame,
                .volume     = gain,
                .audio      = track.audio,
            });
        }
    }
    // 数量不同意味着至少一个标识没有匹配。整批拒绝，避免试听部分混音。
    if ( clips.size() != ids.size() ) {
        return std::unexpected(StemAuditionError::UnknownTrack);
    }

    // 空选择与普通选择共用相同的源时间轴及边界处理。
    return clips;
}

/// @brief 创建独立的试听时间线，随后可由设备或测试夹具驱动。
/// @details 目录的所有轨道都已完成资源准备，节点只引用被选择的 PCM。
/// maximumBlockFrames 是暂存块大小，长输出由 mixer 自行分段。
std::expected<std::shared_ptr<AudioTimelineMixerNode>, StemAuditionError>
StemAudition::makePlayback(std::span<const std::string> ids,
                           std::size_t maximumBlockFrames) const
{
    if ( maximumBlockFrames == 0 ) {
        return std::unexpected(StemAuditionError::InvalidInterval);
    }
    // 解析可以分配，发生在控制线程。所有错误均在发布调度前返回。
    auto clips = makeClips(ids);
    if ( !clips ) {
        return std::unexpected(clips.error());
    }
    // 独立节点以原曲长度为终点，即使选择为空也会推进播放头。
    return std::make_shared<AudioTimelineMixerNode>(
        std::move(*clips), m_sourceEndFrame, maximumBlockFrames);
}

/// @brief 发布同一目录的新选择。
/// @details 旧调度继续存活至音频块边界，由节点在控制线程延迟释放。
/// 切换不下发新的 Seek 或 Play，避免覆盖作者正在使用的传输状态。
/// @return 混音器发布代次，供上层跟踪实际生效位置。
std::expected<std::uint64_t, StemAuditionError> StemAudition::select(
    AudioTimelineMixerNode& playback, std::span<const std::string> ids,
    std::size_t maximumBlockFrames, std::span<const float> gains) const
{
    if ( maximumBlockFrames == 0 ) {
        return std::unexpected(StemAuditionError::InvalidInterval);
    }
    // 解析可以分配，发生在控制线程。所有错误均在发布调度前返回。
    auto clips = makeClips(ids, gains);
    if ( !clips ) {
        return std::unexpected(clips.error());
    }
    // 复用既有 block 边界发布，实时线程从上一播放位置继续。
    // 调度替换继承上一块的传输状态，推子更新不归零或回放已消费帧。
    // 已失效调度按播放器的既有退役协议回收。
    return playback.replaceSchedule(
        std::move(*clips), m_sourceEndFrame, maximumBlockFrames);
}

/// @brief 对固定源帧区间执行与试听相同的 PCM 累加。
/// @details 使用后台私有节点，选择和摘录范围与编辑器设备状态无关。
/// 输入全部通过验证后才触及输出，调用者可在失败时保留现有内容。
/// 一个请求只生成所需的区间，轨道未覆盖的位置自然保持零值。
std::expected<void, StemAuditionError> StemAudition::sample(
    std::span<const std::string> ids, AudioTimelineFrame firstFrame,
    ice::AudioBuffer& output) const
{
    // 只处理活动帧长度，调用方可能复用拥有更大底层容量的缓冲。
    const auto frames = output.num_frames();
    // 输出格式需要完整匹配，帧数一致并不能修复采样率或声道布局差异。
    // 先比较剩余长度，避免将无符号帧数加到有符号起点造成溢出。
    if ( firstFrame < 0 || firstFrame >= m_sourceEndFrame || frames == 0 ||
         std::cmp_greater(frames, m_sourceEndFrame - firstFrame) ||
         output.afmt != ice::ICEConfig::internal_format ) {
        return std::unexpected(StemAuditionError::InvalidInterval);
    }

    // 输出可跨多个处理块；限制额外暂存空间，避免为长区间重复分配整段 PCM。
    auto mixer = makePlayback(ids, std::min<std::size_t>(frames, 4096U));
    // 无效的选择保持调用方原先的 PCM，避免误用部分成果。
    if ( !mixer ) {
        return std::unexpected(mixer.error());
    }
    // 私有节点不改变设备播放头或编辑器的试听选择。
    // 回调顺序与设备一致，先接纳 seek 和 play，再消费该区间。
    // 取样未设置循环，因此范围总是原曲中的单个连续片段。
    // 已准备的轨道在读取时不会发生文件访问或解码。
    // mixer 自行清空整段输出，轨道之间空白无需调用方预填。
    // 仅本线程持有该节点，返回后可立即释放其中的暂存状态。
    // 不把经过 DSP 或采样率转换的时间换算掺入本接口。
    // 谱面拍点和秒到源帧的转换由调用方结合对应时间线完成。
    // 保持返回 PCM 为原始增益，交由媒体编码边界决定输出格式。
    // 此处不改变目录或缓存，另一个线程同时取样可以独立完成。
    (*mixer)->seek(firstFrame);
    (*mixer)->play();
    (*mixer)->process(output);
    return {};
}
}  // namespace MMM::Audio
