#include "AudioManagerTimelinePlayback.h"
#include "BackgroundSpectrumAnalyzer.h"
#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"
#include "audio/AudioTimelineResourceProcessor.h"
#include "audio/AuditionSourceMixerNode.h"
#include "audio/KeySoundControl.h"
#include "audio/StemAudition.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
// 播放只消费音轨参数与类型，不依赖项目资源的路径和身份存储结构。
#include "mmm/project/AudioTrackConfig.h"
#include "mmm/project/AudioTrackType.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>

namespace MMM::Audio
{
/// @brief 后台准备出的音频片段与诊断；控制线程取得后可一次性安装。
struct PreparedAudioTimeline {
    /// @brief 已完成解码和离线处理的片段。
    std::vector<PreparedTimelineClip> m_clips;
    /// @brief 兼容单 BGM 接口所用的首个有效音轨。
    std::shared_ptr<ice::AudioTrack> m_firstTrack;
    /// @brief 仅单事件时间线公开其源路径。
    std::string m_singlePath;
    /// @brief 加载诊断和计数，在安装成功后补齐调度结果。
    AudioTimelineLoadResult m_result;
};

namespace
{
// 本文件管理三类资源状态：复合谱面时间线、旧单 BGM 兼容视图和独立试听轨。
// 复合时间线是主模型；loadBGM 只构造一个零起点事件，旧 getter 则读取加载时
// 保存的首轨兼容字段。试听轨拥有独立 SourceNode，不进入时间线调度。
//
// loadAudioTimeline 按三个阶段组织低频工作：
//
// - 按文件路径去重并批量提交解码；
// - 按会改变 PCM 的资源处理身份准备 DSP 结果；
// - 按事件生成带起点、轨道和最终增益的调度片段。
//
// 文件解码缓存与资源 DSP 缓存是不同层。相同文件只创建一条 AudioTrack；相同
// 路径和 DSP 配置可共享 PreparedTimelineAudio；每个事件仍生成独立 clip。资源
// volume、muted 和事件 volume 只进入运行时片段增益，不扩大 DSP 缓存身份。
//
// 新调度通过 generation 邮箱交给音频回调。替换操作不直接销毁旧调度；回调
// 接管新 generation 后，控制线程才能回收退休快照。加载时允许延后回收，卸载
// 时则有界等待空调度接管，以便随后安全释放可能很大的完整 PCM。
//
// KeySound 区域与轨道控制只是转发到固定大小的 KeySoundControlBank。它们不
// 替换调度、不重新准备资源，音频回调在每个 block 读取一份完整控制快照。
//
// 所有文件系统和解码操作都属于显式加载或卸载入口。状态 getter 与 KeySound
// setter 不访问磁盘；资源清理也只能从项目切换等低频流程触发。
//
// 加载结果的计数分属不同层次：
//
// - requestedSourceCount 是唯一非空文件路径数量；
// - preparedResourceCount 是成功建立的唯一 DSP 结果数量；
// - loadedClipCount 是最终进入调度的事件数量；
// - missingClipCount 是因解码或 DSP 失败跳过的事件数量。
//
// 非有限起点会产生 InvalidStartTime 并回退到零；有限负起点仍然合法，用于表示
// 音频在谱面零点前已经开始。非有限或负的 chartEndSeconds 回退到零，但片段尾端
// 仍可独立延长时间线。缺失单个资源属于可恢复诊断，不使完整加载结果失败。
//
// m_timeline->m_resourceCache 使用弱引用连接 AudioTrack 与
// PreparedTimelineAudio。 它只提供跨时间线和 HitEffect
// 的复用线索，不拥有二者。每次加载和显式缓存 清理都会删除失效项，避免路径 key
// 长期积累或文件重载后复用过期内容。
//
// 旧 BGM getter 只用于仍期待单轨的波形与兼容调用方。复合时间线超过一个事件
// 时 m_timeline->m_singleFilePath 必须为空，调用者应使用 fingerprint
// 判断完整时间线身份，不能把 首个成功资源误认为整个谱面音频。
// 试听同步键则始终按自身文件规范路径生成，不与主时间线 fingerprint 混用。

/// @brief 将音频文件路径转换为稳定的规范化绝对路径键。
/// @param filePath UTF-8 音频文件路径。
/// @return 规范化路径键；路径解析失败时返回词法规范化结果。
/// @warning 低频加载路径：会访问文件系统，只能在加载音轨时调用。
std::string makeAudioPathSyncKey(const std::string& filePath)
{
    if ( filePath.empty() ) {
        return {};
    }

    std::filesystem::path path = Config::utf8ToPath(filePath);
    // weakly_canonical 允许末端尚不存在，同时解析已经存在的父目录和符号链接。
    std::error_code filesystemError;
    auto            canonicalPath =
        std::filesystem::weakly_canonical(path, filesystemError);
    if ( !filesystemError ) {
        // 解析成功时使用文件系统身份；失败时仍保留可比较的词法规范结果。
        path = std::move(canonicalPath);
    }
    return Config::pathToUtf8(path.lexically_normal());
}

/// @brief 将秒数安全转换为统一时间线帧。
/// @param seconds 秒数，允许为负。
/// @return 限制在 AudioTimelineFrame 可表达范围内的最近帧。
AudioTimelineFrame secondsToTimelineFrame(double seconds) noexcept
{
    // 非有限时间没有可排序语义，统一回退到时间线原点。
    if ( !std::isfinite(seconds) ) return 0;

    const long double frames =
        static_cast<long double>(seconds) *
        static_cast<long double>(ice::ICEConfig::internal_format.samplerate);
    constexpr auto MIN_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::min());
    constexpr auto MAX_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::max());
    // 先在浮点域钳制，避免超范围值直接转换为有符号整数。
    if ( frames <= MIN_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::min();
    }
    if ( frames >= MAX_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::max();
    }
    return static_cast<AudioTimelineFrame>(std::llround(frames));
}

/// @brief 追加与指定采样事件关联的加载诊断。
/// @param result 接收诊断的加载结果。
/// @param code 诊断类型。
/// @param event 诊断关联的采样事件。
void appendTimelineDiagnostic(AudioTimelineLoadResult&        result,
                              AudioTimelineLoadDiagnosticCode code,
                              const AudioTimelineLoadEvent&   event)
{
    // 枚举是稳定机器协议，中文文本只面向日志和加载结果展示。
    const char* message = "未知音频时间线加载问题";
    switch ( code ) {
    case AudioTimelineLoadDiagnosticCode::AudioSystemUnavailable:
        message = "音频系统尚未初始化";
        break;
    case AudioTimelineLoadDiagnosticCode::MissingResource:
        message = "音频资源缺失或无法解码";
        break;
    case AudioTimelineLoadDiagnosticCode::InvalidStartTime:
        message = "采样实际起播时间无效，已按 0 秒载入";
        break;
    }
    result.diagnostics.push_back(AudioTimelineLoadDiagnostic{
        // 保留事件身份和原路径，使上层能定位单个降级采样。
        .code        = code,
        .eventId     = event.eventId,
        .resourceKey = event.resourceKey,
        .filePath    = event.filePath,
        .message     = message,
    });
}

/// @brief 将资源配置音量规范化到其声明的单位范围。
/// @param volume 资源配置中的线性音量。
/// @return 有限的 0 到 1 音量。
float sanitizedResourceVolume(float volume) noexcept
{
    // 非有限配置按静音处理，防止 NaN 扩散到整个混音 block。
    return std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 0.0F;
}

/// @brief 将采样物件音量规范化为可混音的非负线性倍率。
/// @param volume 采样物件线性音量。
/// @return 有限非负倍率。
float sanitizedEventVolume(float volume) noexcept
{
    // 事件倍率允许大于一，但负值和非有限值都不能进入混音器。
    return std::isfinite(volume) ? std::max(volume, 0.0F) : 0.0F;
}

/// @brief 等待音频回调接管指定时间线调度，并回收被替换的旧调度。
/// @param node 持有新旧调度的时间线节点。
/// @param generation 需要等待发布的调度代次。
/// @return 在超时前观察到目标代次时返回 true。
/// @warning 项目卸载低频路径：最多阻塞 500 毫秒等待音频 block 边界，禁止
/// 在音频回调、UI 渲染或逻辑热路径中调用。
bool waitForTimelineScheduleRetirement(AudioTimelineMixerNode& node,
                                       std::uint64_t           generation)
{
    using namespace std::chrono_literals;
    constexpr auto TIMEOUT  = 500ms;
    const auto     deadline = std::chrono::steady_clock::now() + TIMEOUT;
    // 只观察公开快照，不读取或修改音频线程当前使用的调度对象。
    while ( std::chrono::steady_clock::now() < deadline ) {
        if ( const auto snapshot = node.clockSnapshot();
             snapshot.valid && snapshot.scheduleGeneration == generation ) {
            static_cast<void>(node.reclaimRetiredSchedules());
            // 目标代次可见后，早于该代次的退休调度已不再被回调读取。
            return true;
        }
        std::this_thread::sleep_for(1ms);
        // 此等待仅在卸载低频路径让出控制线程，不影响回调继续推进。
    }
    static_cast<void>(node.reclaimRetiredSchedules());
    return false;
}

}  // namespace

/// @brief 使用实际源文件身份绑定分轨结果，资源 ID 不跨谱面或项目假定唯一。
std::shared_ptr<StemPlaybackSource> AudioManager::getStemPlaybackSource(
    const std::string& filePath)
{
    return m_timeline->stemPlaybackSource(filePath);
}

/// @brief 后台准备和界面绑定共用索引；规范化和分离均在锁外完成。
std::shared_ptr<StemPlaybackSource>
AudioManager::TimelinePlayback::stemPlaybackSource(const std::string& filePath)
{
    auto key = makeAudioPathSyncKey(filePath);
    if ( key.empty() ) return {};
    std::lock_guard lock(m_stemSourcesMutex);
    auto&           source = m_stemSources[std::move(key)];
    if ( !source ) source = std::make_shared<StemPlaybackSource>();
    return source;
}

/// @brief 查找或建立自动采样与 HitEffect 共用的资源 DSP PCM。
/// @param filePath 用于构造处理缓存身份的音频路径。
/// @param track 已完成解码的源音轨。
/// @param resourceConfig 决定离线 DSP 的资源配置。
/// @param preparedCandidate 调用方已经准备、可直接纳入缓存的候选结果。
/// @return 可供时间线和音效池共享的不可变 PCM；输入无效或处理失败时为空。
/// @warning 低频控制路径：缓存未命中且无候选时会执行完整离线 DSP。
std::shared_ptr<const PreparedTimelineAudio>
AudioManager::TimelinePlayback::getOrPrepareResource(
    const std::string& filePath, const std::shared_ptr<ice::AudioTrack>& track,
    const AudioTrackConfig&                      resourceConfig,
    std::shared_ptr<const PreparedTimelineAudio> preparedCandidate)
{
    // 零帧轨道不能形成有效准备结果，也不应污染缓存。
    if ( !track || track->num_frames() == 0U ) return {};

    const auto processingCacheKey =
        makeAudioResourceProcessingCacheKey(filePath, resourceConfig);
    {
        std::lock_guard lock(m_resourceCacheMutex);
        const auto      existing = m_resourceCache.find(processingCacheKey);
        // 路径和设置相同仍需验证源音轨身份，避免复用替换前的文件内容。
        if ( existing != m_resourceCache.end() &&
             existing->second.sourceTrack.lock() == track ) {
            if ( auto prepared = existing->second.preparedAudio.lock() ) {
                return prepared;
            }
        }
    }

    auto preparedAudio = std::move(preparedCandidate);
    // HitEffect 可能已经准备同一份 PCM，候选存在时不重复执行离线 DSP。
    if ( !preparedAudio ) {
        preparedAudio = prepareAudioTimelineResource(track, resourceConfig);
    }
    if ( preparedAudio ) {
        // 两端都用 weak_ptr，缓存本身不延长源轨或处理结果生命周期。
        std::lock_guard lock(m_resourceCacheMutex);
        // 控制侧与后台可能同时准备同一轨道；优先采用已经发布的 PCM 身份。
        const auto existing = m_resourceCache.find(processingCacheKey);
        if ( existing != m_resourceCache.end() &&
             existing->second.sourceTrack.lock() == track ) {
            if ( auto published = existing->second.preparedAudio.lock() ) {
                return published;
            }
        }
        m_resourceCache.insert_or_assign(processingCacheKey,
                                         CachedTimelineResourceAudio{
                                             .sourceTrack   = track,
                                             .preparedAudio = preparedAudio,
                                         });
    }
    return preparedAudio;
}

/// @brief 在短临界区内锁定 PCM 弱引用，交由调用者在锁外比较。
std::shared_ptr<const PreparedTimelineAudio>
AudioManager::TimelinePlayback::findPreparedResource(
    const std::string& cacheKey) const
{
    const std::lock_guard lock(m_resourceCacheMutex);
    const auto            entry = m_resourceCache.find(cacheKey);
    return entry == m_resourceCache.end() ? nullptr
                                          : entry->second.preparedAudio.lock();
}

/// @brief 删除已无 PCM 或源音轨的条目，避免索引随工程切换增长。
void AudioManager::TimelinePlayback::pruneResourceCache()
{
    std::lock_guard lock(m_resourceCacheMutex);
    std::erase_if(m_resourceCache, [](const auto& cacheEntry) {
        return cacheEntry.second.sourceTrack.expired() ||
               cacheEntry.second.preparedAudio.expired();
    });
}

/// @brief 关闭路径清空 DSP 缓存，各来源的释放不依赖缓存锁。
void AudioManager::TimelinePlayback::clearResourceCache()
{
    const std::lock_guard lock(m_resourceCacheMutex);
    m_resourceCache.clear();
}

/// @brief 在非实时路径准备全部资源并替换复合音频时间线。
/// @param events 自动采样事件。
/// @param chartEndSeconds 非音频谱面内容结束时间。
/// @param fingerprint 完整时间线稳定指纹。
/// @return 图替换结果及逐事件诊断。
/// @warning 低频资源路径：会访问文件系统、等待资源解码并执行资源级离线 DSP。
///
/// 缺失资源和非法事件起点按事件记录诊断，不让其余有效片段加载失败。只有音频
/// 基础设施未初始化时整体失败。空 events 仍能结合 chartEndSeconds 构造独立
/// 谱面时钟，因此不能把零片段视为 unload。
AudioTimelineLoadResult AudioManager::loadAudioTimeline(
    const std::vector<AudioTimelineLoadEvent>& events, double chartEndSeconds,
    const std::string& fingerprint, const std::vector<AudioTimelineStop>& stops)
{
    auto prepared = prepareAudioTimeline(events);
    return installAudioTimeline(*prepared, chartEndSeconds, fingerprint, stops);
}

/// @brief 解码和准备片段但不修改正在使用的 transport。
/// @details 使用 AudioPool 的缓存同步和短期弱缓存锁；作用域覆盖文件解码
/// 与离线变速、变调和 EQ，但不会 seek、更新调度或改写当前 transport。
/// @warning 可在后台运行；引擎退出前须收回任务以保护音频池和工作池生命周期。
std::shared_ptr<PreparedAudioTimeline> AudioManager::prepareAudioTimeline(
    const std::vector<AudioTimelineLoadEvent>& events)
{
    return m_timeline->prepare(
        events, m_audioPool.get(), m_threadPool, m_decodingMode);
}

/// @brief 批量提交解码任务并按事件准备片段。
/// @param pool 管理器的文件缓存，后台准备结束前必须保持有效。
/// @param threadPool 由应用持有的任务池；后端未初始化时为空。
/// @param decodingMode 由控制线程独立发布，只决定下一次文件加载的策略。
/// @warning 时间线准备路径，可能等待解码和处理完整 PCM。
std::shared_ptr<PreparedAudioTimeline> AudioManager::TimelinePlayback::prepare(
    const std::vector<AudioTimelineLoadEvent>& events, ice::AudioPool* pool,
    ice::ThreadPool*                              threadPool,
    const std::atomic<Config::AudioDecodingMode>& decodingMode)
{
    auto  prepared = std::make_shared<PreparedAudioTimeline>();
    auto& result   = prepared->m_result;
    if ( !pool || !threadPool || !m_node ) {
        result.diagnostics.push_back(AudioTimelineLoadDiagnostic{
            .code    = AudioTimelineLoadDiagnosticCode::AudioSystemUnavailable,
            .message = "音频系统尚未初始化",
        });
        return prepared;
    }

    auto& preparedClips = prepared->m_clips;
    // 所有工作容器按事件上界预留，避免准备循环重复扩容。
    preparedClips.reserve(events.size());
    std::unordered_map<std::string, std::shared_ptr<ice::AudioTrack>>
        tracksByPath;
    tracksByPath.reserve(events.size());
    std::unordered_map<std::string,
                       std::shared_ptr<const PreparedTimelineAudio>>
        preparedAudioByProcessingKey;
    preparedAudioByProcessingKey.reserve(events.size());
    // 同源多次触发共用调音台；只为主音轨解析一次源文件身份。
    std::unordered_map<std::string, std::shared_ptr<StemPlaybackSource>>
          stemSourcesByPath;
    auto& firstLoadedTrack = prepared->m_firstTrack;

    // 同一文件可能被多个事件使用；一次收集离线需求，避免逐资源扫描整张事件表。
    std::unordered_set<std::string> offlinePaths;
    for ( const auto& event : events ) {
        if ( event.resourceConfig.eqEnabled ||
             event.resourceConfig.playbackSpeed != 1.0 ||
             event.resourceConfig.playbackPitch != 0.0 )
            // 任一资源需要 DSP 时，该文件必须完整解码，不能选择 Streaming。
            offlinePaths.insert(event.filePath);
    }
    // 先提交全部唯一文件的解码任务，避免逐文件启动后立即等待导致串行化。
    for ( const auto& event : events ) {
        if ( event.filePath.empty() || tracksByPath.contains(event.filePath) ) {
            // 空路径留到事件阶段诊断，重复路径复用首次提交的解码任务。
            continue;
        }
        // 同一文件只要有资源级离线 DSP，就完整缓存以提供稳定 PCM 视图。
        const bool needsOfflinePcm = offlinePaths.contains(event.filePath);
        const auto strategy =
            !needsOfflinePcm && decodingMode.load(std::memory_order_relaxed) ==
                                    Config::AudioDecodingMode::Streaming
                ? ice::CachingStrategy::STREAMING
                : ice::CachingStrategy::CACHY;
        auto track =
            pool->get_or_load(*threadPool, event.filePath, strategy).lock();
        // 失败也记录空项，后续同路径事件不会重复发起相同加载任务。
        tracksByPath.emplace(event.filePath, std::move(track));
    }
    result.requestedSourceCount = tracksByPath.size();

    // 全部解码任务均已提交后，才按事件顺序等待并准备唯一 DSP 结果。
    for ( const auto& event : events ) {
        double startSeconds = event.effectiveStartSeconds;
        // 有限负起点是合法预滚语义，只有 NaN/Inf 才回退到零。
        if ( !std::isfinite(startSeconds) ) {
            appendTimelineDiagnostic(
                result,
                AudioTimelineLoadDiagnosticCode::InvalidStartTime,
                event);
            startSeconds = 0.0;
        }

        std::shared_ptr<ice::AudioTrack> track;
        // 此阶段只消费已提交结果，不再触发新的路径解码。
        if ( !event.filePath.empty() ) {
            const auto existingTrack = tracksByPath.find(event.filePath);
            if ( existingTrack != tracksByPath.end() ) {
                track = existingTrack->second;
            }
        }

        if ( !track ) {
            // 每个缺失事件独立计数，即使多个事件引用同一缺失源。
            ++result.missingClipCount;
            appendTimelineDiagnostic(
                result,
                AudioTimelineLoadDiagnosticCode::MissingResource,
                event);
            continue;
        }

        const auto processingCacheKey = makeAudioResourceProcessingCacheKey(
            event.filePath, event.resourceConfig);
        std::shared_ptr<const PreparedTimelineAudio> preparedAudio;
        const auto                                   existingPreparedAudio =
            preparedAudioByProcessingKey.find(processingCacheKey);
        if ( existingPreparedAudio != preparedAudioByProcessingKey.end() ) {
            // 本次加载内强引用缓存覆盖整个构建阶段。
            preparedAudio = existingPreparedAudio->second;
        } else {
            // 跨加载弱缓存命中或新建 DSP 的细节由统一 helper 负责。
            preparedAudio = getOrPrepareResource(
                event.filePath, track, event.resourceConfig);
            preparedAudioByProcessingKey.emplace(processingCacheKey,
                                                 preparedAudio);
            if ( preparedAudio ) {
                // 统计唯一 DSP 身份，不统计引用它的事件数量。
                ++result.preparedResourceCount;
            }
        }
        if ( !preparedAudio ) {
            // DSP 失败按事件降级，继续构造其余有效片段。
            ++result.missingClipCount;
            appendTimelineDiagnostic(
                result,
                AudioTimelineLoadDiagnosticCode::MissingResource,
                event);
            continue;
        }

        if ( !firstLoadedTrack ) firstLoadedTrack = track;
        // 首个解码轨仅供旧可视化接口兼容，不决定复合时钟。
        const float resourceVolume =
            event.resourceConfig.muted
                ? 0.0F
                : sanitizedResourceVolume(event.resourceConfig.volume);
        std::shared_ptr<StemPlaybackSource> stemSource;
        if ( event.isMainTrack ) {
            auto [source, inserted] =
                stemSourcesByPath.try_emplace(event.filePath);
            if ( inserted ) source->second = stemPlaybackSource(event.filePath);
            stemSource = source->second;
        }
        preparedClips.push_back(PreparedTimelineClip{
            // 负 startFrame 由 MixerNode 在时间线零点裁切。
            .eventId       = event.eventId,
            .sourceKey     = event.resourceKey,
            .startFrame    = secondsToTimelineFrame(startSeconds),
            .bgmTrackIndex = event.bgmTrackIndex,
            .volume = resourceVolume * sanitizedEventVolume(event.eventVolume),
            .audio  = std::move(preparedAudio),
            .stemSource = std::move(stemSource),
        });
    }

    if ( events.size() == 1U ) prepared->m_singlePath = events.front().filePath;
    return prepared;
}

/// @brief 在同一控制侧动作中发布预备片段、调度元数据及主音轨兼容状态。
/// @details 停顿事件和谱面尾端使用提交时数值。准备对象只持有音频片段及诊断，
/// 其容器由此函数接管，不能重复提交给另一个会话使用。
/// @warning 仅控制线程调用；允许构建调度索引，不等待设备回调或执行离线 DSP。
AudioTimelineLoadResult AudioManager::installAudioTimeline(
    PreparedAudioTimeline& prepared, double chartEndSeconds,
    const std::string& fingerprint, const std::vector<AudioTimelineStop>& stops)
{
    return m_timeline->install(
        *this, prepared, chartEndSeconds, fingerprint, stops);
}

/// @brief 发布片段状态，重新同步依赖新时间线身份的混音配置。
/// @param manager 所属控制门面，负责重新协调拉伸器和效果总线。
/// @param prepared 后台准备所得的片段与诊断，提交时移交所有权。
/// @return 诊断、已提交片段数和可供回调接管的调度代次。
AudioTimelineLoadResult AudioManager::TimelinePlayback::install(
    AudioManager& manager, PreparedAudioTimeline& prepared,
    double chartEndSeconds, const std::string& fingerprint,
    const std::vector<AudioTimelineStop>& stops)
{
    auto result = std::move(prepared.m_result);
    // 准备失败时保留现有图，调用方负责决定是否停用旧时间线。
    if ( !m_node || !manager.m_audioPool || !manager.m_threadPool )
        return result;

    const double normalizedChartEnd =
        std::isfinite(chartEndSeconds) ? std::max(chartEndSeconds, 0.0) : 0.0;
    // MixerNode 会取谱面尾端和全部片段尾端的最大值。
    m_baseClips         = std::move(prepared.m_clips);
    m_requestedEndFrame = secondsToTimelineFrame(normalizedChartEnd);
    m_maximumProcessFrames =
        std::max<std::size_t>(ice::ICEConfig::default_buffer_size, 1U);
    // 等待定义在低频载入阶段转换为引擎帧，不在实时回调解析秒数。
    // 使用与歌曲相同的采样率，倍速仅由共享拉伸器处理一次。
    // STOP 位于最后物件时仍须保留一点音乐尾部，让等待可被调度到。
    // 事件资源缺失不影响纯谱面时钟和 STOP 的有效性。
    // 原始时间戳不增加暂停秒数，避免导入与播放各补偿一遍。
    std::vector<TimelineStopEvent> preparedStops;
    for ( const auto& stop : stops ) {
        if ( !std::isfinite(stop.position) || !std::isfinite(stop.duration) ||
             stop.position < 0 || stop.duration <= 0 )
            continue;
        preparedStops.push_back({ secondsToTimelineFrame(stop.position),
                                  secondsToTimelineFrame(stop.duration) });
        // 最后一个 STOP 仍需进入边界处理，保留其后一帧的正常结束位置。
        m_requestedEndFrame = std::max(
            m_requestedEndFrame, secondsToTimelineFrame(stop.position + 0.001));
    }
    // Key 路由与调度一同刷新，避免沿用上个无 STOP 谱面的旁路。
    // 卸载时恢复用户选项，不把此次强制路由写回持久配置。
    m_hasStops = !preparedStops.empty();
    manager.updateSFXSyncSpeedRouting(Config::AppConfig::instance()
                                          .getEditorSettings()
                                          .sfxConfig.hitSfxSyncSpeed);
    result.scheduleGeneration =
        m_node->replaceSchedule(m_baseClips,
                                m_requestedEndFrame,
                                m_maximumProcessFrames,
                                std::move(preparedStops));
    // 发布新代次后清理拉伸历史，避免上一调度残留进入首个 block。
    manager.resetMainTimeStretcher();
    m_loaded = true;
    // 先保存完整兼容状态，再刷新图增益和全局拉伸参数。
    m_fingerprint      = fingerprint;
    m_clipCount        = m_node->clipCount();
    m_missingClipCount = result.missingClipCount;
    // 调度已经就位，长度与这次准备的首轨公开为一个值。
    // 读者无需接触 Mixer 或先读音轨再读取可能已经更新的时长。
    publishAnalysisSource(std::move(prepared.m_firstTrack),
                          manager.getTotalTime());
    m_singleFilePath = std::move(prepared.m_singlePath);
    // 只有严格单事件时间线可暴露旧“单 BGM 路径”语义。
    m_syncKey = fingerprint;

    manager.refreshAudioTimelineVolume();
    // setter 同步现有 stretcher，使换图后延续用户预览参数。
    manager.setPlaybackSpeed(manager.m_speed);
    manager.setPlaybackPitch(manager.getPlaybackPitch());
    manager.setPlaybackQuality(manager.m_playbackQuality);

    result.success         = true;
    result.loadedClipCount = m_clipCount;
    XINFO(
        "Audio timeline loaded: sources={}, prepared={}, clips={}, missing={}, "
        "end={}s, fingerprint={}",
        result.requestedSourceCount,
        result.preparedResourceCount,
        result.loadedClipCount,
        result.missingClipCount,
        manager.getTotalTime(),
        fingerprint);
    return result;
}

/// @brief 停止并卸载当前复合音频时间线。
///
/// 卸载先发布空调度，再短暂解除拉伸器暂停使设备回调能够接管该 generation。
/// 接管完成后恢复暂停并释放弱缓存中无人使用的源轨。等待有上限，超时仅记录
/// 警告，不能让项目切换永久阻塞。
/// @warning 低频项目生命周期路径，可能等待音频回调并释放完整 PCM。
void AudioManager::unloadAudioTimeline(bool waitForRetirement)
{
    m_timeline->unload(*this, waitForRetirement);
}

/// @brief 退役旧调度，保留尚未被设备接管的节点快照。
/// @details 先撤销时间线状态，随后只在调用者要求时等待设备切换代次。
void AudioManager::TimelinePlayback::unload(AudioManager& manager,
                                            bool          waitForRetirement)
{
    if ( !m_node || !m_loaded ) {
        return;
    }

    static_cast<void>(m_node->reclaimRetiredSchedules());
    // 先停止时钟和循环，再清理可能跨项目存活的排定音效。
    m_node->stop();
    m_node->clearLoop();
    manager.clearAllScheduledSoundEffects();
    const std::uint64_t emptyScheduleGeneration = m_node->replaceSchedule(
        {}, 0, std::max<std::size_t>(ice::ICEConfig::default_buffer_size, 1U));
    // 空调度仍有 generation，用于证明回调不再引用旧 clip 快照。
    m_baseClips.clear();
    m_hasStops = false;
    manager.updateSFXSyncSpeedRouting(Config::AppConfig::instance()
                                          .getEditorSettings()
                                          .sfxConfig.hitSfxSyncSpeed);
    // 控制线程副本立即清空；退休调度由回调接管后再释放。
    m_requestedEndFrame    = 0;
    m_maximumProcessFrames = 1U;
    manager.resetMainTimeStretcher();
    // discontinuity 丢弃旧调度已经缓存但尚未输出的拉伸采样。
    m_loaded = false;
    m_fingerprint.clear();
    m_clipCount        = 0U;
    m_missingClipCount = 0U;
    publishAnalysisSource({}, 0.0);
    m_singleFilePath.clear();
    m_syncKey.clear();
    // 交互模式同样先清理可查询状态，随后才决定如何让设备接管新代次。
    // 暂停设备时不等待确认，否则调用线程可能无法在合理时间内继续运行。
    if ( !waitForRetirement ) {
        // 停用立即对调用方生效。设备的下一次有效拉取会接管空调度；
        // 后续调度发布或 AudioManager 关闭时回收仍由节点持有的旧状态。
        if ( manager.m_stretcher ) {
            manager.m_stretcher->set_paused(
                !manager.m_auditionPlaying.load(std::memory_order_relaxed));
        }
        return;
    }
    if ( manager.m_stretcher ) {
        // 停止状态下拉伸器不会拉取上游；短暂恢复空时间线分支，确保音频
        // 回调能在下一 block 接管空调度，期间不会产生可听输出。
        manager.m_stretcher->set_paused(false);
    }
    if ( !waitForTimelineScheduleRetirement(*m_node,
                                            emptyScheduleGeneration) ) {
        XWARN(
            "Timed out waiting for the audio callback to retire the unloaded "
            "timeline schedule.");
    }
    if ( manager.m_stretcher ) {
        // 卸载主谱面不能暂停仍在播放的资源试听。
        manager.m_stretcher->set_paused(
            !manager.m_auditionPlaying.load(std::memory_order_relaxed));
    }
    static_cast<void>(manager.releaseUnusedTrackCache());
    // 最后清理 AudioPool，避免旧项目源轨被缓存自身长期保活。
    XINFO("Audio timeline unloaded.");
}

/// @brief 获取当前完整时间线稳定指纹。
/// @return 未加载时为空字符串。
const std::string& AudioManager::getLoadedAudioTimelineFingerprint() const
{
    return m_timeline->m_fingerprint;
}

/// @brief 获取当前有效采样片段数量。
/// @return 调度表中的片段数量。
std::size_t AudioManager::getLoadedAudioTimelineClipCount() const
{
    return m_timeline->m_clipCount;
}

/// @brief 获取上次加载时缺失的片段数量。
/// @return 缺失或无法解码的事件数量。
std::size_t AudioManager::getMissingAudioTimelineClipCount() const
{
    return m_timeline->m_missingClipCount;
}

/// @brief 判断当前是否已构造时间线时钟。
/// @return 即使零片段时间线也在已构造时返回 true。
bool AudioManager::hasLoadedAudioTimeline() const
{
    return m_timeline->m_loaded;
}

/// @brief 设置整个玩家打击音区的运行时静音覆盖。
/// @param muted true 时覆盖所有玩家轨道输出。
void AudioManager::setPlayerKeySoundAreaMuted(bool muted) noexcept
{
    m_keySoundControls->setPlayerAreaMuted(muted);
}

/// @brief 查询整个玩家打击音区的运行时静音覆盖。
bool AudioManager::isPlayerKeySoundAreaMuted() const noexcept
{
    return m_keySoundControls->isPlayerAreaMuted();
}

/// @brief 设置指定玩家轨道的 Key 音静音状态。
/// @param trackIndex 固定控制库中的玩家轨道索引。
/// @param muted 轨道静音状态。
void AudioManager::setPlayerKeySoundTrackMuted(std::uint32_t trackIndex,
                                               bool          muted) noexcept
{
    m_keySoundControls->setPlayerTrackMuted(trackIndex, muted);
}

/// @brief 查询指定玩家轨道是否已静音。
/// @param trackIndex 固定控制库中的玩家轨道索引。
/// @return 索引有效且轨道静音时返回 true。
bool AudioManager::isPlayerKeySoundTrackMuted(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->isPlayerTrackMuted(trackIndex);
}

/// @brief 设置指定玩家轨道的 Key 音线性增益。
/// @param trackIndex 固定控制库中的玩家轨道索引。
/// @param gain 非负线性增益，由控制库负责规范化。
void AudioManager::setPlayerKeySoundTrackGain(std::uint32_t trackIndex,
                                              float         gain) noexcept
{
    m_keySoundControls->setPlayerTrackGain(trackIndex, gain);
}

/// @brief 查询指定玩家轨道的 Key 音线性增益。
float AudioManager::getPlayerKeySoundTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->getPlayerTrackGain(trackIndex);
}

/// @brief 设置整个草稿轨道区的 Key 音静音状态。
/// @param muted true 时覆盖所有草稿轨道输出。
void AudioManager::setDraftKeySoundAreaMuted(bool muted) noexcept
{
    m_keySoundControls->setDraftAreaMuted(muted);
}

/// @brief 查询整个草稿轨道区是否已静音。
bool AudioManager::isDraftKeySoundAreaMuted() const noexcept
{
    return m_keySoundControls->isDraftAreaMuted();
}

/// @brief 设置指定草稿轨道的 Key 音静音状态。
/// @param trackIndex 固定控制库中的草稿轨道索引。
/// @param muted 轨道静音状态。
void AudioManager::setDraftKeySoundTrackMuted(std::uint32_t trackIndex,
                                              bool          muted) noexcept
{
    m_keySoundControls->setDraftTrackMuted(trackIndex, muted);
}

/// @brief 查询指定草稿轨道是否已静音。
/// @param trackIndex 固定控制库中的草稿轨道索引。
/// @return 索引有效且轨道静音时返回 true。
bool AudioManager::isDraftKeySoundTrackMuted(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->isDraftTrackMuted(trackIndex);
}

/// @brief 设置指定草稿轨道的 Key 音线性增益。
/// @param trackIndex 固定控制库中的草稿轨道索引。
/// @param gain 非负线性增益，由控制库负责规范化。
void AudioManager::setDraftKeySoundTrackGain(std::uint32_t trackIndex,
                                             float         gain) noexcept
{
    m_keySoundControls->setDraftTrackGain(trackIndex, gain);
}

/// @brief 查询指定草稿轨道的 Key 音线性增益。
float AudioManager::getDraftKeySoundTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->getDraftTrackGain(trackIndex);
}

/// @brief 设置整个 BGM 轨道区的 Key 音静音状态。
/// @param muted true 时覆盖所有 BGM 轨道输出。
void AudioManager::setBgmKeySoundAreaMuted(bool muted) noexcept
{
    m_keySoundControls->setBgmAreaMuted(muted);
}

/// @brief 查询整个 BGM 轨道区是否已静音。
bool AudioManager::isBgmKeySoundAreaMuted() const noexcept
{
    return m_keySoundControls->isBgmAreaMuted();
}

/// @brief 设置整个 BGM Key 音区的线性增益。
/// @param gain 非负区域线性增益，由控制库负责规范化。
void AudioManager::setBgmKeySoundAreaGain(float gain) noexcept
{
    m_keySoundControls->setBgmAreaGain(gain);
}

/// @brief 查询整个 BGM Key 音区的线性增益。
float AudioManager::getBgmKeySoundAreaGain() const noexcept
{
    return m_keySoundControls->getBgmAreaGain();
}

/// @brief 设置指定 BGM 轨道的 Key 音静音状态。
/// @param trackIndex 固定控制库中的 BGM 轨道索引。
/// @param muted 轨道静音状态。
void AudioManager::setBgmKeySoundTrackMuted(std::uint32_t trackIndex,
                                            bool          muted) noexcept
{
    m_keySoundControls->setBgmTrackMuted(trackIndex, muted);
}

/// @brief 查询指定 BGM 轨道是否已静音。
/// @param trackIndex 固定控制库中的 BGM 轨道索引。
/// @return 索引有效且轨道静音时返回 true。
bool AudioManager::isBgmKeySoundTrackMuted(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->isBgmTrackMuted(trackIndex);
}

/// @brief 设置指定 BGM 轨道的 Key 音线性增益。
/// @param trackIndex 固定控制库中的 BGM 轨道索引。
/// @param gain 非负线性增益，由控制库负责规范化。
void AudioManager::setBgmKeySoundTrackGain(std::uint32_t trackIndex,
                                           float         gain) noexcept
{
    m_keySoundControls->setBgmTrackGain(trackIndex, gain);
}

/// @brief 查询指定 BGM 轨道的 Key 音线性增益。
float AudioManager::getBgmKeySoundTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->getBgmTrackGain(trackIndex);
}

/// @brief 设置未绑定或绑定打击音效类别的运行时静音覆盖。
/// @param group 绑定或非绑定音效类别。
/// @param muted 类别静音状态。
void AudioManager::setKeySoundEffectGroupMuted(KeySoundEffectGroup group,
                                               bool muted) noexcept
{
    m_keySoundControls->setEffectGroupMuted(group, muted);
}

/// @brief 查询未绑定或绑定打击音效类别的运行时静音覆盖。
bool AudioManager::isKeySoundEffectGroupMuted(
    KeySoundEffectGroup group) const noexcept
{
    return m_keySoundControls->isEffectGroupMuted(group);
}

/// @brief 设置未绑定或绑定打击音效类别的线性增益。
/// @param group 绑定或非绑定音效类别。
/// @param gain 非负类别线性增益，由控制库负责规范化。
void AudioManager::setKeySoundEffectGroupGain(KeySoundEffectGroup group,
                                              float               gain) noexcept
{
    m_keySoundControls->setEffectGroupGain(group, gain);
}

/// @brief 查询未绑定或绑定打击音效类别的线性增益。
float AudioManager::getKeySoundEffectGroupGain(
    KeySoundEffectGroup group) const noexcept
{
    return m_keySoundControls->getEffectGroupGain(group);
}

/// @brief 提交主时间线半开循环范围。
/// @param startSeconds 循环起点。
/// @param endSeconds 排除结束点。
/// @return 参数和时间线均有效时返回 true。
///
/// 循环范围使用谱面帧表达；提交前请求拉伸器 discontinuity，避免历史跨越循环
/// 边界。具体范围钳制和最小宽度由 MixerNode 统一验证。
bool AudioManager::setAudioTimelineLoop(double startSeconds, double endSeconds)
{
    if ( !m_timeline->m_loaded || !m_timeline->m_node ||
         !std::isfinite(startSeconds) || !std::isfinite(endSeconds) ||
         startSeconds >= endSeconds ) {
        return false;
    }
    // 先清理下游历史，再把新范围作为控制命令交给时间线节点。
    resetMainTimeStretcher();
    return m_timeline->m_node->setLoop({ secondsToTimelineFrame(startSeconds),
                                         secondsToTimelineFrame(endSeconds) });
}

/// @brief 关闭主时间线循环并清除拉伸历史。
///
/// 未加载时保持幂等；有效时间线中先清理下游，再撤销上游循环边界。
void AudioManager::clearAudioTimelineLoop()
{
    if ( m_timeline->m_loaded && m_timeline->m_node ) {
        resetMainTimeStretcher();
        m_timeline->m_node->clearLoop();
    }
}

/// @brief 将旧单 BGM 请求包装为零秒单事件时间线。
/// @param filePath 音频文件路径。
/// @param config 完整资源配置；高级 DSP 离线应用且不覆盖全局预览参数。
/// @return 单片段成功载入时返回 true。
///
/// 规范路径同时作为资源 key、时间线 fingerprint 和旧同步键，保持调用方原有
/// “同文件即同 BGM”比较语义。高级资源配置仍走统一离线 DSP。
bool AudioManager::loadBGM(const std::string&      filePath,
                           const AudioTrackConfig& config)
{
    const auto fingerprint = makeAudioPathSyncKey(filePath);
    // 兼容事件固定零 ID 和零起点，不额外构造旧 SourceNode 播放图。
    const auto result = loadAudioTimeline({ AudioTimelineLoadEvent{
                                              .eventId     = 0U,
                                              .resourceKey = fingerprint,
                                              .filePath    = filePath,
                                              .effectiveStartSeconds = 0.0,
                                              .eventVolume           = 1.0F,
                                              .resourceConfig        = config,
                                              .isMainTrack           = true,
                                          } },
                                          0.0,
                                          fingerprint);
    return result.success && result.loadedClipCount == 1U;
}

/// @brief 兼容入口：卸载当前复合时间线。
void AudioManager::unloadBGM()
{
    unloadAudioTimeline();
}

/// @brief 将一份轨道与时间线时长同时提交给 UI。
/// @param track 由调度准备阶段提供，空值代表清除可分析来源。
/// @param duration 调度提交时的复合长度，单位为秒。
void AudioManager::TimelinePlayback::publishAnalysisSource(
    std::shared_ptr<ice::AudioTrack> track, double duration)
{
    // 参数接收旧音轨，离开锁域后才允许释放解码器和 PCM。
    {
        std::lock_guard lock(m_bgmAnalysisMutex);
        m_bgmAnalysisSource.track.swap(track);
        m_bgmAnalysisSource.duration = duration;
        ++m_bgmAnalysisSource.generation;
    }
}

/// @brief 获取单次发布中的轨道、时长与代次。
AudioManager::BgmAnalysisSource AudioManager::getBgmAnalysisSource() const
{
    return m_timeline->analysisSource();
}

/// @brief 读取同一代次的来源与时长。
AudioManager::BgmAnalysisSource
AudioManager::TimelinePlayback::analysisSource() const
{
    std::lock_guard lock(m_bgmAnalysisMutex);
    return m_bgmAnalysisSource;
}

/// @brief 获取当前时间线首个成功加载的音轨，供旧可视化入口兼容。
/// @return 没有有效采样时返回空指针。
std::shared_ptr<ice::AudioTrack> AudioManager::getBGMTrack() const
{
    return getBgmAnalysisSource().track;
}

/// @brief 获取单片段兼容时间线的文件路径。
/// @return 复合时间线或未加载时返回空字符串。
const std::string& AudioManager::getLoadedBGMPath() const
{
    return m_timeline->m_singleFilePath;
}

/// @brief 把独立多轨试听接到变速器后的设备总线。
/// @details 试听使用自身源帧传输，因此谱面 Seek 或 STOP 不影响此节点。
/// 该节点直接送入设备总线，必须先应用当前全局和 BGM 分类控制。
/// 和单轨试听不同，工具节点没有跟随谱面拉伸时钟的节拍器。
/// @warning 仅由试听工具在打开音轨后低频调用。
bool AudioManager::attachStemAudition(
    const std::shared_ptr<AudioTimelineMixerNode>& node)
{
    if ( !m_mainMixer || !node ) {
        return false;
    }
    // 一个工具只占用一个输出来源。重复发布同一节点时不会叠加音量。
    if ( m_stemAudition == node ) {
        return true;
    }
    detachStemAudition(m_stemAudition);
    m_stemAudition = node;
    // 连接前先应用静音，避免新节点用默认音量短暂发声。
    refreshAuditionTrackVolume();
    // 主总线在回调边界更新来源列表。节点无需借用谱面拉伸时钟。
    m_mainMixer->add_source(node);
    return true;
}

/// @brief 在工具关闭或更换文件组时切断试听音频。
/// @warning 仅在控制线程调用，设备回调持有的旧节点由总线延迟回收。
void AudioManager::detachStemAudition(
    const std::shared_ptr<AudioTimelineMixerNode>& node)
{
    if ( !node || m_stemAudition != node ) {
        return;
    }
    // 旧快照可能仍在回调中持有节点，先停止其传输。
    node->stop();
    if ( m_mainMixer ) {
        m_mainMixer->remove_source(node);
    }
    m_stemAudition.reset();
}

/// @brief 将音轨加载到独立试听通道并接入主混音器。
/// @param filePath 音频文件绝对路径。
/// @param config 试听音轨配置。
/// @return 加载并接入混音图成功时返回 true。
/// @warning 低频资源路径：可能触发音频解码缓存加载，禁止在每帧热路径中调用。
///
/// 新文件先完整取得强引用，确认成功后才卸载旧试听，保证失败不会破坏当前试听。
/// 歌曲与专用节拍池先混合，再进入效果音轨的主时间线拉伸前级。
bool AudioManager::loadAuditionTrack(const std::string&      filePath,
                                     const AudioTrackConfig& config,
                                     AudioTrackType          type)
{
    if ( !m_audioPool || !m_threadPool || !m_timingEffectMixer ||
         filePath.empty() ) {
        return false;
    }

    XINFO("Loading audition track: {}", filePath);
    // 试听尊重播放解码偏好；离线分析入口另行强制 CACHY。
    const auto strategy = decodingMode() == Config::AudioDecodingMode::Streaming
                              ? ice::CachingStrategy::STREAMING
                              : ice::CachingStrategy::CACHY;
    auto       trackWeak =
        m_audioPool->get_or_load(*m_threadPool, filePath, strategy);
    auto track = trackWeak.lock();
    if ( !track ) {
        XERROR("Failed to load audition track: {}", filePath);
        return false;
    }

    unloadAuditionTrack();
    // 从此处开始提交新试听状态，旧图已经断开且新 track 确认可用。

    m_auditionTrack       = std::move(track);
    m_auditionPath        = filePath;
    m_auditionSyncKey     = makeAudioPathSyncKey(filePath);
    m_auditionTrackVolume = std::clamp(config.volume, 0.0f, 1.0f);
    m_auditionTrackMuted  = config.muted;
    // 歌曲音量分类独立于节拍器，不能因为共享拉伸器就继承 SFX 总控。
    m_auditionTrackType = type;
    // 歌曲的资源静音只控制 SourceNode，节拍器继续使用自己的池增益。
    // SourceNode 保留资源秒坐标；歌曲与节拍不再拥有第二个拉伸器。
    m_auditionSource = std::make_shared<ice::SourceNode>(m_auditionTrack);
    // 预留主拉伸器最大输入请求，禁止回调期间扩大暂存空间。
    const std::size_t maximumInputFrames =
        std::max<std::size_t>(ice::ICEConfig::default_buffer_size, 1U) * 10U +
        1U;
    m_auditionMixer =
        std::make_shared<AuditionSourceMixerNode>(m_auditionSource,
                                                  m_auditionEffectMixer,
                                                  m_auditionBlockStartFrame,
                                                  maximumInputFrames);
    m_timingEffectMixer->add_source(m_auditionMixer);
    // 加入图后保持 Stopped，调用方显式 playAudition 才开始推进。
    m_auditionStatus = PlaybackStatus::Stopped;

    refreshAuditionTrackVolume();
    setAuditionPlaybackSpeed(config.playbackSpeed);
    XINFO("Audition track loaded successfully.");
    return true;
}

/// @brief 卸载独立试听音轨并断开其混音节点。
///
/// 先停止 SourceNode，再从效果音轨断开歌曲与节拍混合节点，最后释放 track。
/// 资源增益恢复中性值，共用主拉伸器的倍率不因卸载而重置。
void AudioManager::unloadAuditionTrack()
{
    stopAudition();

    if ( m_timingEffectMixer && m_auditionMixer ) {
        // 先断开父图边，再释放本层所有权；主拉伸器继续服务谱面。
        m_timingEffectMixer->remove_source(m_auditionMixer);
    }

    const bool hadAuditionTrack = m_auditionTrack || m_auditionSource ||
                                  m_auditionMixer || !m_auditionPath.empty();
    // 记录清理前状态，使重复卸载保持幂等且不产生误导日志。
    m_auditionMixer.reset();
    m_auditionSource.reset();
    m_auditionTrack.reset();
    m_auditionPath.clear();
    m_auditionSyncKey.clear();
    m_auditionStatus      = PlaybackStatus::Stopped;
    m_auditionTrackVolume = 1.0f;
    m_auditionTrackMuted  = false;

    if ( hadAuditionTrack ) {
        XINFO("Audition track unloaded.");
    }
}

/// @brief 获取独立试听通道当前加载的音频路径。
/// @return 音频文件路径；未加载时返回空字符串。
const std::string& AudioManager::getLoadedAuditionPath() const
{
    return m_auditionPath;
}

/// @brief 获取当前时间线兼容同步键。
/// @return 完整时间线指纹；未加载时为空。
const std::string& AudioManager::getLoadedBGMSyncKey() const
{
    return m_timeline->m_syncKey;
}

/// @brief 获取独立试听文件的规范化绝对路径键。
/// @return 与 Session 主音轨同步键格式一致的路径键；未加载时为空。
const std::string& AudioManager::getLoadedAuditionSyncKey() const
{
    return m_auditionSyncKey;
}

/// @brief 使指定音频文件的解码缓存失效。
/// @param filePath UTF-8 音频文件绝对路径。
/// @warning 低频资源维护入口，会修改 AudioPool 缓存。
void AudioManager::invalidateTrackCache(const std::string& filePath)
{
    if ( !m_audioPool || filePath.empty() ) {
        return;
    }
    // 失效只阻止未来命中；播放图持有的 track 继续存活到消费者释放。
    m_audioPool->invalidate(filePath);
}

/// @brief 释放只剩音频池自身持有的完整解码音轨。
/// @return 本次释放的缓存音轨数量。
/// @warning 低频资源卸载路径：可能析构完整 PCM，禁止在音频回调、UI
/// 渲染或逻辑热路径中调用。
std::size_t AudioManager::releaseUnusedTrackCache()
{
    if ( !m_audioPool ) return 0U;

    m_timeline->pruneResourceCache();
    // 音轨析构可能归还大块 PCM 或清理后端解码器；保持在弱缓存锁外。
    // AudioPool 对自己的索引有独立同步，不让析构阻塞弱值查询。
    const std::size_t releasedCount = m_audioPool->release_unused();
    if ( releasedCount > 0U ) {
        XDEBUG("Released {} unused decoded audio track(s).", releasedCount);
    }
    return releasedCount;
}

/// @brief 加载或复用音频资源池中的轨道，供离线分析工具读取。
/// @param filePath 音频文件绝对路径。
/// @return 加载成功时返回音频轨道；失败时返回空指针。
/// @warning 低频分析路径：可能触发音频解码缓存加载，严禁在每帧
/// UI、渲染或逻辑热路径中调用。
///
/// 分析器需要任意位置稳定读取完整 PCM，因此无条件选择 CACHY，不继承用户为
/// 播放降低内存占用而选择的 Streaming 策略。
std::shared_ptr<ice::AudioTrack> AudioManager::loadTrackForAnalysis(
    const std::string& filePath)
{
    if ( !m_audioPool || !m_threadPool ) {
        return nullptr;
    }

    auto trackWeak = m_audioPool->get_or_load(
        *m_threadPool, filePath, ice::CachingStrategy::CACHY);
    // 锁定弱结果既等待异步加载完成，也把成功音轨交给调用方延长生命周期。
    auto track = trackWeak.lock();
    if ( !track ) {
        XERROR("Failed to load analysis audio track: {}", filePath);
        return nullptr;
    }

    return track;
}

}  // namespace MMM::Audio
