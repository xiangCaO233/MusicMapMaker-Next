#include "logic/audio/AudioTimelineDescriptor.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

/// @brief 谱面在 charts 子目录，旧路径需以谱面父目录为基准。
constexpr std::string_view BEATMAP_PATH = "charts/descriptor.mmm";

/// @brief 构造覆盖 Main、Effect 和高级配置的测试项目。
/// @return 使用绝对临时根目录的项目。
/// @note 资源集合仅描述身份和配置，不读取音频元信息。
/// @note 返回项目的根路径用于规范化，不要求该目录已存在。
/// @warning 路径查询属于测试准备，不能复用于逻辑每帧资源查找。
MMM::Project makeProject()
{
    // 只生成路径，不创建媒体文件。
    // 描述符解析资源声明，解码由后续音频加载层负责。
    std::error_code filesystemError;
    auto root = std::filesystem::temp_directory_path(filesystemError);
    if ( filesystemError ) {
        // 临时目录不可用时改用当前目录。
        // 清除旧错误，让本次路径查询独立报告状态。
        filesystemError.clear();
        root = std::filesystem::current_path(filesystemError);
    }

    // 每次返回独立项目，字段变化不会污染其他用例。
    // 固定叶子路径避免把随机目录名带入指纹比较。
    MMM::Project project;
    project.m_projectRoot = root / "mmm-audio-timeline-descriptor-test";

    // 非默认速度、音高和 EQ 用于识别字段丢失。
    // 增益数组和 Q 数组使用不同数据，避免误交换仍通过比较。
    // EQ 预设和手动频带同时保存，两者都属于持久化配置。
    MMM::AudioTrackConfig mainConfig;
    mainConfig.volume        = 0.8F;
    mainConfig.playbackSpeed = 1.15F;
    mainConfig.playbackPitch = -1.5F;
    mainConfig.muted         = false;
    mainConfig.eqEnabled     = true;
    mainConfig.eqPreset      = 2;
    mainConfig.eqBandGains   = { 1.0F, -2.0F };
    mainConfig.eqBandQs      = { 0.7F, 1.2F };

    // Effect 与 Main 使用不同参数，能识别错误复用配置。
    // EQ 关闭时频带仍保留，关闭效果不等于删除保存值。
    MMM::AudioTrackConfig effectConfig;
    effectConfig.volume        = 0.45F;
    effectConfig.playbackSpeed = 0.9F;
    effectConfig.playbackPitch = 2.0F;
    effectConfig.eqBandGains   = { 3.0F };
    effectConfig.eqBandQs      = { 0.85F };

    // 稳定 ID 与文件名刻意不同。
    // 两种资源类型均需支持 ID 和旧路径引用。
    // 项目资源声明使用相对根目录的路径，不相对谱面目录。
    project.m_audioResources = {
        MMM::AudioResource{
            .m_id     = "main-id",
            .m_path   = "audio/main.ogg",
            .m_type   = MMM::AudioTrackType::Main,
            .m_config = mainConfig,
        },
        MMM::AudioResource{
            .m_id     = "effect-id",
            .m_path   = "audio/effect.wav",
            .m_type   = MMM::AudioTrackType::Effect,
            .m_config = effectConfig,
        },
    };
    return project;
}

/// @brief 构造单个自动采样测试对象。
/// @param timestamp 锚点时间，单位毫秒。
/// @param offsetMs 有符号偏移，单位毫秒。
/// @param track 纯视觉统一轨道索引。
/// @param audioReference 项目资源 ID 或旧路径。
/// @param volume 物件音量。
/// @return 自动采样对象。
/// @note volume 不在夹具内归一化，被测转换应接收原始实例值。
/// @note 偏移允许使有效时间小于零，不代表非法采样。
/// @note audioReference 按值保存，调用方临时字符串不会成为悬空引用。
MMM::AudioSampleEvent makeSample(double timestamp, std::int64_t offsetMs,
                                 std::uint32_t      track,
                                 const std::string& audioReference,
                                 float              volume)
{
    // 锚点与偏移分别保存，实际起播时间交给生产代码合成。
    // track 保留统一画布索引，不在夹具里预先转成 BGM 局部索引。
    // 事件音量与资源配置音量独立，后续检查不能混为同一字段。
    MMM::AudioSampleEvent sample;
    sample.m_timestamp       = timestamp;
    sample.m_offsetMs        = offsetMs;
    sample.m_track           = track;
    sample.m_audioResourceId = audioReference;
    sample.m_volume          = volume;
    return sample;
}

/// @brief 构造包含重复采样、正负 offset 和缺失引用的测试谱面。
/// @param reverseOrder 是否反转输入采样顺序。
/// @return 测试谱面。
/// @note 返回四条自动采样和一条带绑定的普通 Note。
/// @note 两个主音轨引用采用不同语法，但应解析到同一个规范键。
/// @note 超范围视觉轨号仍参与描述符；此处不测试编辑器轨道合法性。
MMM::BeatMap makeBeatMap(bool reverseOrder)
{
    MMM::BeatMap beatMap;
    // 四条玩家轨决定统一轨号到 BGM 局部索引的偏移。
    // BGM 可见数量不应裁掉播放事件。
    // 歌曲提示是元数据，不应隐式加入播放时间线。
    beatMap.m_baseMapMetadata.track_count     = 4;
    beatMap.m_baseMapMetadata.bgm_track_count = 96;
    beatMap.m_baseMapMetadata.song_file_hint  = "ignored-song-hint.ogg";

    // 两条 Main 具有相同起播时间与音量，但属于不同播放实例。
    // ID 与旧路径最终应归一到同一资源键。
    // 缺失引用从负时间开始，用于区分锚点排序和有效时间排序。
    // Effect 从 2.5 秒减去 0.5 秒，负偏移不能被丢弃。
    std::vector<MMM::AudioSampleEvent> samples{
        makeSample(1000.0, 250, 4, "main-id", 0.6F),
        makeSample(2500.0, -500, 8, "audio/effect.wav", 0.75F),
        makeSample(100.0, -600, 1000, "missing.wav", 0.4F),
        makeSample(1000.0, 250, 27, "audio/main.ogg", 0.6F),
    };
    // 反序只改变遍历顺序，事件值保持相同。
    // 规范化后的顺序与身份不能依赖插入次序。
    if ( reverseOrder ) std::reverse(samples.begin(), samples.end());
    for ( auto& sample : samples ) {
        beatMap.m_audioSamples.push_back(std::move(sample));
    }

    // Note 绑定属于按键音效，不属于自动采样。
    // 使用非空绑定才能识别错误混入另一条播放通路。
    MMM::Note ignoredNote;
    ignoredNote.setSampleBinding(MMM::AudioSampleBinding{ "effect-id", 0.2F });
    beatMap.m_noteData.notes.push_back(std::move(ignoredNote));
    return beatMap;
}

/// @brief 比较两个完整音轨配置。
/// @param lhs 左侧配置。
/// @param rhs 右侧配置。
/// @return 所有持久化字段均相同时返回 true。
/// @note 对照双方必须处于同一次夹具语义中，不比较近似 DSP 结果。
/// @note 静音和 EQ 禁用时仍比较保存值，配置身份不只包含当前可听输出。
/// @note 不比较资源 ID 或文件路径，它们由事件层比较器负责。
bool sameConfig(const MMM::AudioTrackConfig& lhs,
                const MMM::AudioTrackConfig& rhs)
{
    // 夹具只经过值复制，浮点字段应完全一致。
    // 容差会掩盖不应发生的配置重写，因此使用精确比较。
    // 频带数组同时检查内容、顺序和长度。
    return lhs.volume == rhs.volume && lhs.playbackSpeed == rhs.playbackSpeed &&
           lhs.playbackPitch == rhs.playbackPitch && lhs.muted == rhs.muted &&
           lhs.eqEnabled == rhs.eqEnabled && lhs.eqPreset == rhs.eqPreset &&
           lhs.eqBandGains == rhs.eqBandGains && lhs.eqBandQs == rhs.eqBandQs;
}

/// @brief 比较两个 AudioManager 加载事件的完整字段。
/// @param lhs 左侧事件。
/// @param rhs 右侧事件。
/// @return 事件 ID 和全部听觉字段均相同时返回 true。
/// @note 不要求文件存在，路径以规范化后保存的字符串比较。
/// @note 本测试不比较解码 PCM 或设备播放时钟。
/// @note 比较范围用于捕获描述符构建的信息丢失，不代替声音验收。
bool sameLoadEvent(const MMM::Audio::AudioTimelineLoadEvent& lhs,
                   const MMM::Audio::AudioTimelineLoadEvent& rhs)
{
    // 检查身份稳定性，不能只确认播放内容相同。
    // 规范资源键和绝对路径分开检查，避免同名文件混淆。
    // BGM 索引决定路由，必须与实际起播时刻一起保留。
    return lhs.eventId == rhs.eventId && lhs.resourceKey == rhs.resourceKey &&
           lhs.filePath == rhs.filePath &&
           lhs.effectiveStartSeconds == rhs.effectiveStartSeconds &&
           lhs.bgmTrackIndex == rhs.bgmTrackIndex &&
           lhs.eventVolume == rhs.eventVolume &&
           sameConfig(lhs.resourceConfig, rhs.resourceConfig);
}

/// @brief 比较两个规范事件序列。
/// @param lhs 左侧事件序列。
/// @param rhs 右侧事件序列。
/// @return 顺序和全部事件字段均相同时返回 true。
/// @pre 两组数据由描述符构建入口返回，测试不预先修改规范顺序。
/// @note 空序列之间可相等；需要非空的场景必须另外检查数量。
/// @note 比较器不修改事件，不引入新的播放身份。
bool sameLoadEvents(const std::vector<MMM::Audio::AudioTimelineLoadEvent>& lhs,
                    const std::vector<MMM::Audio::AudioTimelineLoadEvent>& rhs)
{
    // 先比较长度，再按规范顺序逐项比较。
    // 不在断言中重新排序，否则会掩盖生产排序错误。
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), sameLoadEvent);
}

/// @brief 按资源键查找首个加载事件。
/// @param descriptor 待查询描述符。
/// @param resourceKey 项目资源键。
/// @return 找到时返回事件地址，否则返回空。
/// @pre descriptor 在返回指针被使用期间保持存活且不修改事件容器。
/// @note 查询使用规范键，不替调用方解析旧路径。
/// @note 找不到时返回空指针，调用方先检查再读取资源配置。
const MMM::Audio::AudioTimelineLoadEvent* findEvent(
    const MMM::Logic::AudioTimelineDescriptor& descriptor,
    const std::string&                         resourceKey)
{
    // 同资源可有多个实例，此助手只查首项供配置断言使用。
    // 实例数量与唯一 ID 由其他断言验证。
    // 返回内部观察指针，描述符修改或销毁后不得继续使用。
    const auto iterator =
        std::find_if(descriptor.m_events.begin(),
                     descriptor.m_events.end(),
                     [&](const MMM::Audio::AudioTimelineLoadEvent& event) {
                         return event.resourceKey == resourceKey;
                     });
    return iterator == descriptor.m_events.end() ? nullptr : &*iterator;
}

/// @brief 验证规范排序、资源解析、重复事件和正负 offset。
/// @return 描述符保留全部自动采样听觉语义时返回 true。
/// @note 时间容差仅用于毫秒换算后的秒值，身份和轨号按精确值检查。
/// @note 正常资源路径是声明式夹具，不对磁盘文件可读性作结论。
/// @note 缺失诊断关联事件，而不是把整个描述符判作构建失败。
bool testCanonicalDescriptor()
{
    const auto project    = makeProject();
    const auto beatMap    = makeBeatMap(false);
    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        12.5);

    // 缺失资源仍保留事件，以便诊断指向具体实例。
    // 一个未解析引用只产生一条诊断。
    // 同时检查两种指纹的格式和谱面结束时间。
    if ( descriptor.m_events.size() != 4U ||
         descriptor.m_diagnostics.size() != 1U ||
         descriptor.m_fingerprint.size() != 32U ||
         descriptor.m_mainAudioSyncFingerprint.size() != 32U ||
         descriptor.m_chartEndSeconds != 12.5 ) {
        XERROR("Audio timeline descriptor did not retain all sample events");
        return false;
    }

    // 前置数量检查保证下面四个下标有效。
    // 统一轨号减四，得到 996、0、23 和 4。
    // 负起播事件应排在最前，同时间实例保持确定顺序。
    // 保留负时间，播放器从谱面中段起播时仍需要这些信息。
    if ( descriptor.m_events[0].resourceKey != "missing.wav" ||
         descriptor.m_events[0].filePath != "" ||
         std::abs(descriptor.m_events[0].effectiveStartSeconds + 0.5) >
             1.0e-9 ||
         descriptor.m_events[0].bgmTrackIndex != 996U ||
         descriptor.m_events[1].resourceKey != "main-id" ||
         descriptor.m_events[2].resourceKey != "main-id" ||
         descriptor.m_events[1].bgmTrackIndex != 0U ||
         descriptor.m_events[2].bgmTrackIndex != 23U ||
         std::abs(descriptor.m_events[1].effectiveStartSeconds - 1.25) >
             1.0e-9 ||
         std::abs(descriptor.m_events[2].effectiveStartSeconds - 1.25) >
             1.0e-9 ||
         descriptor.m_events[3].resourceKey != "effect-id" ||
         descriptor.m_events[3].bgmTrackIndex != 4U ||
         std::abs(descriptor.m_events[3].effectiveStartSeconds - 2.0) >
             1.0e-9 ) {
        XERROR("Audio timeline descriptor was not canonically time-sorted");
        return false;
    }

    // 相同资源的两个实例不能共享事件身份。
    // ID 必须非零，零不能冒充可引用的播放事件。
    if ( descriptor.m_events[1].eventId == descriptor.m_events[2].eventId ||
         descriptor.m_events[1].eventId == 0U ||
         descriptor.m_events[2].eventId == 0U ) {
        XERROR("Duplicate audio tuples did not receive unique stable IDs");
        return false;
    }

    // 两种资源类型都必须解析绝对路径及完整配置。
    // 这里只检查路径形态，不要求真实媒体存在。
    // 先检查观察指针有效，再解引用配置，失败断言不能自身崩溃。
    const auto* mainEvent   = findEvent(descriptor, "main-id");
    const auto* effectEvent = findEvent(descriptor, "effect-id");
    if ( !mainEvent || !effectEvent ||
         !MMM::Config::utf8ToPath(mainEvent->filePath).is_absolute() ||
         !MMM::Config::utf8ToPath(effectEvent->filePath).is_absolute() ||
         !sameConfig(mainEvent->resourceConfig,
                     project.m_audioResources[0].m_config) ||
         !sameConfig(effectEvent->resourceConfig,
                     project.m_audioResources[1].m_config) ) {
        XERROR("Main or Effect resources were not fully resolved");
        return false;
    }

    // 诊断关联规范事件 ID，而不是排序前的容器下标。
    // 原始引用用于提示用户，空 filePath 无法说明缺失的是谁。
    // 文案只要求非空，不将具体措辞作为测试契约。
    const auto& diagnostic = descriptor.m_diagnostics.front();
    if ( diagnostic.m_code !=
             MMM::Logic::AudioTimelineDescriptorDiagnosticCode::
                 UnresolvedAudioResource ||
         diagnostic.m_eventId != descriptor.m_events.front().eventId ||
         diagnostic.m_audioReference != "missing.wav" ||
         diagnostic.m_message.empty() ) {
        XERROR("Missing audio reference diagnostic was incomplete");
        return false;
    }
    return true;
}

/// @brief 验证输入排列不影响规范顺序、事件 ID 和指纹。
/// @return 反序输入得到完全相同描述符时返回 true。
/// @note 保持同时间重复事件，检查排序后的身份分配也可重现。
/// @note 不要求不同谱面路径共享事件 ID；本场景路径保持不变。
/// @note 相同指纹与逐事件相同必须同时成立。
bool testOrderIndependentIdentity()
{
    const auto project  = makeProject();
    const auto forward  = makeBeatMap(false);
    const auto reversed = makeBeatMap(true);
    // 两个构建使用同一项目、谱面路径和结束时间。
    // 唯一变化来自输入次序，排除无关字段干扰。
    const auto forwardDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        forward,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        8.0);
    // 第二次调用接收反序容器而非复用第一次结果。
    // 若生产入口遗漏规范化，逐事件比较能观察到差异。
    // 所有路径参数相同，测试不对跨项目指纹作要求。
    const auto reversedDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        reversed,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        8.0);

    // 两种指纹与完整事件序列分别比较。
    // 哈希相同不能代替逐字段检查稳定 ID 和路由。
    if ( forwardDescriptor.m_fingerprint != reversedDescriptor.m_fingerprint ||
         forwardDescriptor.m_mainAudioSyncFingerprint !=
             reversedDescriptor.m_mainAudioSyncFingerprint ||
         !sameLoadEvents(forwardDescriptor.m_events,
                         reversedDescriptor.m_events) ) {
        XERROR("Audio descriptor identity depended on source event order");
        return false;
    }
    return true;
}

/// @brief 验证画布同步仅由 Main 资源序列及各自有效起播位置决定。
/// @return 布局和非 Main 差异被忽略，Main 资源或位置变化被识别时返回 true。
/// @note Main 同步键允许忽略音量和布局，完整指纹仍应记录它们的播放影响。
/// @note 一毫秒变化用于检查起播位置敏感性，不借助真实时钟推进。
/// @note Main 类型由项目资源声明确定，不以文件扩展名区分。
bool testMainAudioSyncFingerprintUsesResourcesAndPositions()
{
    auto project = makeProject();
    // 加入第二种主音轨，检查同步身份是否包含资源序列。
    // 只用一个 Main 无法区分序列变化与简单的资源存在性。
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id   = "main-alt-id",
        .m_path = "audio/main-alt.ogg",
        .m_type = MMM::AudioTrackType::Main,
    });

    /// @brief 构造跨 Key 数但 Main 序列和起播位置可保持一致的谱面。
    const auto makeSyncMap = [](int           trackCount,
                                std::uint32_t firstMainTrack,
                                std::uint32_t secondMainTrack,
                                float         firstMainVolume,
                                double        effectTimestamp) {
        // 第一段主音轨从零点前开始，第二段从 4.25 秒开始。
        // Effect 的时间单独可变，用于确认它不影响 Main 同步。
        // 夹具不对参数轨号做迁移，Key 数变化会改变相对 BGM 位置。
        MMM::BeatMap beatMap;
        beatMap.m_baseMapMetadata.track_count = trackCount;
        beatMap.m_audioSamples.push_back(
            makeSample(0.0, -577, firstMainTrack, "main-id", firstMainVolume));
        beatMap.m_audioSamples.push_back(
            makeSample(4000.0, 250, secondMainTrack, "main-alt-id", 0.7F));
        beatMap.m_audioSamples.push_back(makeSample(
            effectTimestamp, 0, secondMainTrack + 3U, "effect-id", 0.4F));
        return beatMap;
    };

    // 改变 Key 数、主轨音量、Effect 时间及结束时间。
    // 完整播放语义改变，但 Main 资源和有效起播位置一致。
    // 两种指纹必须表达各自用途，不能简单共用同一哈希。
    const auto sixKeyMap        = makeSyncMap(6, 10U, 11U, 0.5F, 1000.0);
    const auto fiveKeyMap       = makeSyncMap(5, 10U, 11U, 0.9F, 2500.0);
    const auto sixKeyDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        sixKeyMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        30.0);
    const auto fiveKeyDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        fiveKeyMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        45.0);

    // 完整指纹决定重建播放时间线，同步指纹决定画布联动。
    // 非空检查排除两个默认空字符串造成伪兼容。
    if ( sixKeyDescriptor.m_fingerprint == fiveKeyDescriptor.m_fingerprint ||
         sixKeyDescriptor.m_mainAudioSyncFingerprint.empty() ||
         sixKeyDescriptor.m_mainAudioSyncFingerprint !=
             fiveKeyDescriptor.m_mainAudioSyncFingerprint ) {
        XERROR("Compatible Main timelines did not share a canvas sync key");
        return false;
    }

    // 只让第二段 Main 偏移增加一毫秒。
    // 资源序列不变，位置变化仍必须破坏同步兼容性。
    auto movedMainMap = makeSyncMap(5, 10U, 11U, 0.9F, 2500.0);
    movedMainMap.m_audioSamples[1].m_offsetMs += 1;
    const auto movedMainDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        movedMainMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        45.0);
    if ( movedMainDescriptor.m_mainAudioSyncFingerprint ==
         fiveKeyDescriptor.m_mainAudioSyncFingerprint ) {
        XERROR("A moved Main resource retained the old canvas sync key");
        return false;
    }

    // 起播时刻保持不变，只替换第二段资源。
    // 相同数量和时间点不能掩盖资源序列变化。
    auto changedSequenceMap = makeSyncMap(5, 10U, 11U, 0.9F, 2500.0);
    changedSequenceMap.m_audioSamples[1].m_audioResourceId = "main-id";
    const auto changedSequenceDescriptor =
        MMM::Logic::buildAudioTimelineDescriptor(
            changedSequenceMap,
            project,
            MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
            45.0);
    if ( changedSequenceDescriptor.m_mainAudioSyncFingerprint ==
         fiveKeyDescriptor.m_mainAudioSyncFingerprint ) {
        XERROR("A changed Main resource sequence retained the old sync key");
        return false;
    }
    return true;
}

/// @brief 验证玩家轨道数量变化后保持相对 BGM 轨道时指纹不变。
/// @return 仅绝对画布索引迁移及非自动采样字段变化时指纹保持不变。
/// @note 迁移保持相对路由，不能推广为任意 BGM 轨道移动都不影响身份。
/// @note 普通 Note 的绑定只影响按键音，和自动采样播放生命周期不同。
/// @note 基准与变体使用相同结束时间，排除长度差异。
bool testNonAudioFieldsAreExcluded()
{
    const auto project  = makeProject();
    const auto baseline = makeBeatMap(false);
    auto       changed  = makeBeatMap(false);
    // 玩家轨增加五条，采样统一轨号也整体增加五。
    // 相对 BGM 路由不变，不应触发音频重建。
    // 歌曲提示和 BGM 可见数量均不属于自动采样听觉语义。
    changed.m_baseMapMetadata.track_count     = 9;
    changed.m_baseMapMetadata.bgm_track_count = 2048;
    changed.m_baseMapMetadata.song_file_hint  = "another-hint.wav";
    for ( auto& sample : changed.m_audioSamples ) {
        sample.m_track += 5U;
    }
    // 按键绑定变化不能进入自动播放指纹。
    // 混合场景包含真实绑定，避免只测空绑定而遗漏错误扫描。
    changed.m_noteData.notes.front().setSampleBinding(
        MMM::AudioSampleBinding{ "main-id", 0.95F });

    const auto baselineDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        baseline,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        9.0);
    const auto changedDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        changed,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        9.0);
    // 事件序列和指纹都必须保持不变。
    // 只忽略指纹里的绝对轨号、却错误修改加载路由也应失败。
    if ( baselineDescriptor.m_fingerprint != changedDescriptor.m_fingerprint ||
         !sameLoadEvents(baselineDescriptor.m_events,
                         changedDescriptor.m_events) ) {
        XERROR("Absolute canvas track migration leaked into audio fingerprint");
        return false;
    }
    return true;
}

/// @brief 验证全部资源配置、物件音量和谱面结束时间参与指纹。
/// @return 任一听觉语义变化均产生不同指纹。
/// @note 逐字段扰动用于验证失效条件，不以哈希字面值作为黄金数据。
/// @note 资源级处理与实例级控制分别覆盖，避免相同响度掩盖语义差异。
/// @note 失败日志指出首个未参与身份的维度。
bool testFingerprintSensitivity()
{
    // 基准构建后不再修改。
    // 每个变体重新创建夹具，确保差异来自单个测试维度。
    const auto baselineProject = makeProject();
    const auto baselineMap     = makeBeatMap(false);
    // 只构建一次基准描述符供全部变体比较。
    // 基准资源与谱面保持存活，后续变体不共享可写资源配置。
    // 结果保留为值对象，不依赖生产缓存的地址身份。
    const auto baseline = MMM::Logic::buildAudioTimelineDescriptor(
        baselineMap,
        baselineProject,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.0);

    /// @brief 验证单个资源配置修改能够改变指纹。
    const auto configChangesFingerprint = [&](auto&&      mutate,
                                              const char* fieldName) {
        // 仅修改一个资源配置字段，其他输入和基准相同。
        // 字段名用于错误日志，不参与被测指纹。
        auto project = makeProject();
        mutate(project.m_audioResources.front().m_config);
        const auto beatMap = makeBeatMap(false);
        const auto changed = MMM::Logic::buildAudioTimelineDescriptor(
            beatMap,
            project,
            MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
            10.0);
        if ( changed.m_fingerprint == baseline.m_fingerprint ) {
            XERROR("Audio config field '{}' was absent from fingerprint",
                   fieldName);
            return false;
        }
        return true;
    };

    // 分别扰动所有持久化资源参数。
    // EQ 开关和预设分别检查，不能只哈希最终启用状态。
    // 追加频带元素还能检查数组长度是否参与身份。
    // 短路失败保留首个字段名，便于定位缺失的语义维度。
    if ( !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) { config.volume += 0.01F; },
             "volume") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) {
                 config.playbackSpeed += 0.01F;
             },
             "playbackSpeed") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) {
                 config.playbackPitch += 0.25F;
             },
             "playbackPitch") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) { config.muted = true; },
             "muted") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) { config.eqEnabled = false; },
             "eqEnabled") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) { ++config.eqPreset; },
             "eqPreset") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) {
                 config.eqBandGains.push_back(4.0F);
             },
             "eqBandGains") ||
         !configChangesFingerprint(
             [](MMM::AudioTrackConfig& config) {
                 config.eqBandQs.push_back(1.5F);
             },
             "eqBandQs") ) {
        return false;
    }

    // 实例音量与项目资源音量分开验证。
    // 改变采样响度不能要求先改写共享资源。
    auto volumeMap = makeBeatMap(false);
    volumeMap.m_audioSamples.front().m_volume += 0.1F;
    const auto volumeChanged = MMM::Logic::buildAudioTimelineDescriptor(
        volumeMap,
        baselineProject,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.0);
    // 谱面尾部静默区决定时间线结束位置。
    // 事件完全相同也不能忽略结束时间增长。
    const auto chartEndChanged = MMM::Logic::buildAudioTimelineDescriptor(
        baselineMap,
        baselineProject,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.5);
    // 保持玩家轨数不变，只移动一个采样的混音路由。
    // 这与整体轨号迁移不同，必须改变完整指纹。
    auto bgmTrackMap = makeBeatMap(false);
    ++bgmTrackMap.m_audioSamples.front().m_track;
    const auto bgmTrackChanged = MMM::Logic::buildAudioTimelineDescriptor(
        bgmTrackMap,
        baselineProject,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.0);
    // 三个独立变体分别对照同一基准。
    // 一种变化被识别不能替另一种未识别的变化通过。
    if ( volumeChanged.m_fingerprint == baseline.m_fingerprint ||
         chartEndChanged.m_fingerprint == baseline.m_fingerprint ||
         bgmTrackChanged.m_fingerprint == baseline.m_fingerprint ) {
        XERROR(
            "Event volume, BGM track routing or chart end was absent from "
            "fingerprint");
        return false;
    }
    return true;
}

/// @brief 验证资源配置失效筛选只匹配自动采样的规范资源 ID。
/// @return Main、Effect 和缺失引用可匹配，Note 绑定与别名路径不会误匹配。
/// @note 调用方必须传规范键，旧路径解析由构建阶段完成。
/// @note 无法解析的非空引用仍需参与失效查询，以便资源恢复后重建。
/// @note 只含 Note 绑定的反例应返回未引用。
bool testDescriptorResourceReferenceLookup()
{
    const auto project    = makeProject();
    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        makeBeatMap(false),
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        10.0);
    // 失效查询只接受规范资源键，不再解析旧别名。
    // 未解析引用仍保留为诊断键，补齐资源时需触发重建。
    // 空字符串不表示匹配全部资源。
    if ( !MMM::Logic::audioTimelineDescriptorReferencesResource(descriptor,
                                                                "main-id") ||
         !MMM::Logic::audioTimelineDescriptorReferencesResource(descriptor,
                                                                "effect-id") ||
         !MMM::Logic::audioTimelineDescriptorReferencesResource(
             descriptor, "missing.wav") ||
         MMM::Logic::audioTimelineDescriptorReferencesResource(
             descriptor, "audio/main.ogg") ||
         MMM::Logic::audioTimelineDescriptorReferencesResource(descriptor,
                                                               "") ) {
        XERROR("Audio timeline resource reference lookup was incorrect");
        return false;
    }

    // 仅有 Note 绑定而没有自动采样。
    // 这个独立反例检查按键资源不会泄漏进自动时间线引用集合。
    MMM::BeatMap noteOnlyMap;
    MMM::Note    note;
    note.setSampleBinding(MMM::AudioSampleBinding{ "effect-id", 1.0F });
    noteOnlyMap.m_noteData.notes.push_back(std::move(note));
    const auto noteOnlyDescriptor = MMM::Logic::buildAudioTimelineDescriptor(
        noteOnlyMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    return !MMM::Logic::audioTimelineDescriptorReferencesResource(
        noteOnlyDescriptor, "effect-id");
}

/// @brief 验证大量重复 v2 ID 事件复用首次出现的资源索引结果。
///
/// 资源和事件数量同时扩大，使测试输入能够暴露逐事件线性扫描退化；
/// 断言不依赖机器耗时，只验证首次同 ID 资源及全部事件解析结果。
/// @return 全部事件均解析到首次同 ID 资源时返回 true。
/// @note 两个同 ID 资源故意同时存在，覆盖历史首项优先规则。
/// @note 每条采样具有不同时间，不能因资源重复而折叠实例。
/// @note 该规模测试不检查真实解码任务数。
bool testBulkStableIdResolutionUsesFirstResource()
{
    // 前置资源把目标推到列表后部，扩大重复扫描输入。
    // 断言不计时，不能把通过结果当成复杂度测量。
    constexpr std::size_t FILLER_RESOURCE_COUNT = 256U;
    constexpr std::size_t EVENT_COUNT           = 512U;

    auto project = makeProject();
    project.m_audioResources.clear();
    // 移除默认资源，排除 ID 或 basename 偶然命中。
    // 预留容量只减少夹具构造成本，不介入生产索引。
    project.m_audioResources.reserve(FILLER_RESOURCE_COUNT + 2U);
    for ( std::size_t index = 0U; index < FILLER_RESOURCE_COUNT; ++index ) {
        project.m_audioResources.push_back(MMM::AudioResource{
            .m_id   = "filler-" + std::to_string(index),
            .m_path = "audio/filler-" + std::to_string(index) + ".wav",
            .m_type = MMM::AudioTrackType::Effect,
        });
    }

    MMM::AudioTrackConfig firstConfig;
    // 首个同 ID 项使用特征参数。
    // 完整配置检查能发现路径来自首项、配置来自后项的拼接错误。
    firstConfig.volume        = 0.37F;
    firstConfig.playbackSpeed = 1.25F;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id     = "repeated-id",
        .m_path   = "audio/repeated-first.wav",
        .m_type   = MMM::AudioTrackType::Effect,
        .m_config = firstConfig,
    });

    // 后项同时改变路径、类型和配置。
    // 索引优化必须保留列表首项优先，不能让最后写入覆盖。
    MMM::AudioTrackConfig duplicateConfig;
    duplicateConfig.volume = 0.91F;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id     = "repeated-id",
        .m_path   = "audio/repeated-second.wav",
        .m_type   = MMM::AudioTrackType::Main,
        .m_config = duplicateConfig,
    });

    // 时间按毫秒递增，使每个实例可独立保留。
    // 同 ID 查询结果可复用，但源事件数量不变。
    // 不借助真实媒体文件数量作为缓存是否有效的判据。
    MMM::BeatMap beatMap;
    for ( std::size_t index = 0U; index < EVENT_COUNT; ++index ) {
        beatMap.m_audioSamples.push_back(
            makeSample(static_cast<double>(index), 0, 4, "repeated-id", 1.0F));
    }

    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    // 先验证总数和无诊断，再检查每条事件所选资源。
    // 否则事件全部丢失也可能让后面的 find_if 返回末尾。
    // 该先后顺序是失败检查本身的边界保护。
    if ( descriptor.m_events.size() != EVENT_COUNT ||
         !descriptor.m_diagnostics.empty() ) {
        XERROR("Bulk stable ID events were not all resolved");
        return false;
    }

    // 叶子文件名避免写死临时根目录。
    // 每个事件仍检查规范键和完整配置，而不只检查首条样本。
    const auto expectedFilename =
        MMM::Config::utf8ToPath("audio/repeated-first.wav").filename();
    const auto invalidEvent = std::find_if(
        descriptor.m_events.begin(),
        descriptor.m_events.end(),
        [&](const MMM::Audio::AudioTimelineLoadEvent& event) {
            return event.resourceKey != "repeated-id" ||
                   MMM::Config::utf8ToPath(event.filePath).filename() !=
                       expectedFilename ||
                   !sameConfig(event.resourceConfig, firstConfig);
        });
    // 前面已经检查数量，空容器不能借助空范围通过。
    // 发现一个错误映射即可失败，不需要解引用查询结果。
    if ( invalidEvent != descriptor.m_events.end() ) {
        XERROR("Repeated stable ID did not retain first-resource semantics");
        return false;
    }
    return true;
}

/// @brief 验证不同匹配方式冲突时仍按项目资源顺序选择首项。
///
/// 同一引用对前项是路径和 basename 匹配，对后项是精确 ID 匹配；
/// 旧实现逐资源扫描时必须选择前项，统一索引也必须保持该语义。
/// @return 描述符解析到项目列表前项时返回 true。
/// @note 这是跨匹配方式的冲突，而不是两个相同 ID 的冲突。
/// @note 前项资源类型为 Effect，后项为 Main，误选也会影响同步归组。
/// @note 断言以规范键、路径与完整配置确认最终选择来源。
bool testCrossModeConflictPreservesResourceOrder()
{
    auto project = makeProject();
    project.m_audioResources.clear();

    MMM::AudioTrackConfig firstConfig;
    firstConfig.volume = 0.23F;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id     = "foo.wav",
        .m_path   = "audio/foo.wav",
        .m_type   = MMM::AudioTrackType::Effect,
        .m_config = firstConfig,
    });

    // 后项 ID 与查询完全相同，但前项先满足旧路径匹配。
    // 不能按匹配方式优先级覆盖原有的项目列表顺序。
    MMM::AudioTrackConfig exactIdConfig;
    exactIdConfig.volume = 0.87F;
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id     = "audio/foo.wav",
        .m_path   = "audio/exact-id.wav",
        .m_type   = MMM::AudioTrackType::Main,
        .m_config = exactIdConfig,
    });

    // 查询字串同时命中前项路径和后项 ID。
    // 只建立一个事件，把失败原因限定为候选优先顺序。
    // 结束时间固定，不用其他听觉差异影响选择验证。
    MMM::BeatMap beatMap;
    beatMap.m_audioSamples.push_back(
        makeSample(0.0, 0, 4, "audio/foo.wav", 1.0F));
    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    if ( descriptor.m_events.size() != 1U ||
         !descriptor.m_diagnostics.empty() ) {
        return false;
    }

    // 数量和诊断先通过，才读取唯一结果。
    // 路径、键和配置必须全部来自同一首选候选。
    const auto& event = descriptor.m_events.front();
    if ( event.resourceKey != "foo.wav" ||
         MMM::Config::utf8ToPath(event.filePath).filename() != "foo.wav" ||
         !sameConfig(event.resourceConfig, firstConfig) ) {
        XERROR("Cross-mode audio reference conflict changed first-match order");
        return false;
    }
    return true;
}

/// @brief 验证大量不同谱面相对旧路径在一次资源索引中完整解析。
///
/// 每个事件引用不同路径且资源使用不同稳定 ID，使输入能够暴露
/// unique-reference×resource 的重复扫描退化。
/// @return 所有不同旧路径均解析为对应稳定资源 ID 时返回 true。
/// @note 路径含父目录跳转，按谱面位置解析后应回到项目资源声明。
/// @note 项目资源 ID 不与路径相同，不能只做字符串原样透传。
/// @note 用键集合检查覆盖率，不依赖输入顺序恢复结果。
bool testBulkDistinctLegacyPathResolution()
{
    // 每条旧引用均不同，重复引用缓存无法掩盖逐事件扫描。
    // 固定规模覆盖批量解析语义，不设机器相关的时间阈值。
    constexpr std::size_t RESOURCE_COUNT = 384U;

    auto project = makeProject();
    project.m_audioResources.clear();
    project.m_audioResources.reserve(RESOURCE_COUNT);

    MMM::BeatMap beatMap;
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        // 谱面位于 charts，../audio 才能定位项目音频目录。
        // 项目保存稳定 ID，旧谱面路径需转换为对应 stable-legacy 键。
        const auto filename = "legacy-" + std::to_string(index) + ".wav";
        project.m_audioResources.push_back(MMM::AudioResource{
            .m_id   = "stable-legacy-" + std::to_string(index),
            .m_path = "audio/" + filename,
            .m_type = MMM::AudioTrackType::Effect,
        });
        beatMap.m_audioSamples.push_back(makeSample(
            static_cast<double>(index), 0, 4, "../audio/" + filename, 1.0F));
    }

    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    // 路径解析不能以跳过失败事件来保持其余结果看似正确。
    // 完整数量与空诊断同时约束批量解析成功。
    // 后面的集合检查再约束每个稳定键是否出现。
    if ( descriptor.m_events.size() != RESOURCE_COUNT ||
         !descriptor.m_diagnostics.empty() ) {
        XERROR("Bulk distinct legacy references were not all resolved");
        return false;
    }

    // 事件总数正确仍可能全部误映射到同一资源。
    // 逐个检查预期键，排除遗漏、重复替代和错误合并。
    std::unordered_set<std::string> resolvedIds;
    resolvedIds.reserve(descriptor.m_events.size());
    for ( const auto& event : descriptor.m_events ) {
        resolvedIds.insert(event.resourceKey);
    }
    for ( std::size_t index = 0U; index < RESOURCE_COUNT; ++index ) {
        if ( !resolvedIds.contains("stable-legacy-" + std::to_string(index)) ) {
            XERROR("Distinct legacy reference missed resource {}", index);
            return false;
        }
    }
    return true;
}

/// @brief 验证重复绝对旧路径在批量解析中共享同一缓存结果。
///
/// 大量事件使用相同绝对引用，确保描述符只需按唯一引用和唯一资源执行
/// 文件系统规范化，同时仍保留每个自动采样事件。
/// @return 全部重复事件均解析到同一稳定资源时返回 true。
/// @note 同一个绝对路径用于多个播放实例，解析缓存只共享资源结果。
/// @note 结果检查全部事件，避免只确认第一个实例后遗漏批量丢失。
/// @note 描述符阶段不需要为该路径生成实体文件。
bool testRepeatedAbsoluteLegacyReferenceResolution()
{
    constexpr std::size_t EVENT_COUNT = 512U;

    auto project = makeProject();
    // 绝对引用不能再次附加谱面目录。
    // 只声明另一个目录下的资源，不创建或解码真实媒体。
    const auto absolutePath =
        project.m_projectRoot / "outside" / "repeated-absolute.wav";
    project.m_audioResources = {
        MMM::AudioResource{
            .m_id   = "stable-absolute",
            .m_path = MMM::Config::pathToUtf8(absolutePath),
            .m_type = MMM::AudioTrackType::Effect,
        },
    };

    MMM::BeatMap beatMap;
    // 全部实例共享引用但时间不同。
    // 缓存路径解析结果不能顺带消除独立播放事件。
    const auto absoluteReference = MMM::Config::pathToUtf8(absolutePath);
    for ( std::size_t index = 0U; index < EVENT_COUNT; ++index ) {
        beatMap.m_audioSamples.push_back(makeSample(
            static_cast<double>(index), 0, 4, absoluteReference, 1.0F));
    }

    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        1.0);
    // 数量检查确保没有按资源键合并独立采样。
    // 逐项规范键检查确保绝对路径确实转换为项目稳定身份。
    // 空诊断检查防止正确事件旁还残留重复报错。
    if ( descriptor.m_events.size() != EVENT_COUNT ||
         !descriptor.m_diagnostics.empty() ||
         std::any_of(descriptor.m_events.begin(),
                     descriptor.m_events.end(),
                     [](const MMM::Audio::AudioTimelineLoadEvent& event) {
                         return event.resourceKey != "stable-absolute";
                     }) ) {
        XERROR("Repeated absolute legacy references were not fully resolved");
        return false;
    }
    return true;
}

/// @brief 验证静音采样草稿不会创建缺失资源诊断或播放事件。
/// @return 描述符完全忽略空资源采样时返回 true。
/// @note 本场景的静音由空引用表达，不等同于资源静音配置或零音量。
/// @note 缺失资源场景必须提供非空引用，不能与草稿共用失败分支。
/// @note 即使没有播放事件，也保留谱面的非音频结束时间。
bool testSilentSampleDraftIsExcludedFromPlayback()
{
    const auto   project = makeProject();
    MMM::BeatMap beatMap;
    beatMap.m_baseMapMetadata.track_count = 4;
    // 空资源键表示尚未绑定的静音草稿。
    // 它不同于非空但解析失败的 missing.wav。
    // 放入真实采样对象，避免空谱面掩盖过滤遗漏。
    beatMap.m_audioSamples.push_back(makeSample(1000.0, 0, 4, {}, 1.0F));

    // 即使项目中存在可用 Main，也不能替空引用自动选歌。
    // 空草稿的位置和音量不应生成默认音频事件。
    // 保留独立结束时间以验证静音时间线仍有范围。
    const auto descriptor = MMM::Logic::buildAudioTimelineDescriptor(
        beatMap,
        project,
        MMM::Config::utf8ToPath(std::string(BEATMAP_PATH)),
        2.0);
    // 静音草稿不产生播放事件或缺失诊断。
    // 没有 Main 时同步指纹为空，防止无音乐画布误入同步组。
    // 谱面结束时间仍保留，静音不等于长度为零。
    return descriptor.m_events.empty() && descriptor.m_diagnostics.empty() &&
           descriptor.m_mainAudioSyncFingerprint.empty() &&
           descriptor.m_chartEndSeconds == 2.0;
}

}  // namespace

/// @brief 运行音频时间线描述符构建测试。
/// @return 全部测试通过时返回 0。
int main()
{
    // 先验证基础规范化，再验证指纹边界和批量冲突。
    // 各场景自行创建夹具，顺序不承担状态传递。
    // 非零退出表示首个失败，后续未执行场景不计为通过。
    return testCanonicalDescriptor() && testOrderIndependentIdentity() &&
                   testNonAudioFieldsAreExcluded() &&
                   testMainAudioSyncFingerprintUsesResourcesAndPositions() &&
                   testFingerprintSensitivity() &&
                   testDescriptorResourceReferenceLookup() &&
                   testBulkStableIdResolutionUsesFirstResource() &&
                   testCrossModeConflictPreservesResourceOrder() &&
                   testBulkDistinctLegacyPathResolution() &&
                   testRepeatedAbsoluteLegacyReferenceResolution() &&
                   testSilentSampleDraftIsExcludedFromPlayback()
               ? 0
               : 1;
}
