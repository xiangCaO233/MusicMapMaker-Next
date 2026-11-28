#pragma once

#include "audio/AudioTimelineTransport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ice
{
class AudioBuffer;
class ThreadPool;
}  // namespace ice

namespace MMM::Audio
{
class AudioTimelineMixerNode;
class PreparedTimelineAudio;
struct PreparedTimelineClip;

/// @brief 已对齐到内部采样率的独立试听音轨。
struct StemAuditionTrack {
    /// @brief 调用方提供的稳定 ID，文件名和显示名可独立变化。
    std::string id;
    /// @brief 音轨首帧在共享源时间轴上的位置。
    AudioTimelineFrame startFrame{};
    /// @brief 保留源电平的 PCM，采用与音频图一致的声道和采样率。
    std::shared_ptr<const PreparedTimelineAudio> audio;
};

/// @brief 音轨组、选择或区间不满足请求契约。
enum class StemAuditionError {
    DecodeFailed,     ///< 指定文件未解码出可用的完整 PCM。
    InvalidTrack,     ///< 空 ID、缺失 PCM 或当前音轨超出源时间轴。
    DuplicateTrack,   ///< 源目录或请求中有重复 ID，可能导致重复混音。
    UnknownTrack,     ///< 请求的 ID 不在当前目录中。
    InvalidInterval,  ///< 区间、缓冲格式或单次输出帧数无效。
};

/// @brief 为试听节点与离线摘录提供同一份不可变、源帧对齐的音轨目录。
/// @details 原曲或其他参考版可独立建组。调用方负责准备 PCM 和管理设备。
class StemAudition final
{
public:
    /// @brief 建立有明确结束位置的目录，拒绝重复 ID 和越界 PCM。
    /// @param sourceEndFrame 源时间轴排除端点，也是全关时的播放终点。
    /// @param tracks 已转换为内部 PCM 格式的已知音轨。
    [[nodiscard]] static std::expected<StemAudition, StemAuditionError> create(
        AudioTimelineFrame             sourceEndFrame,
        std::vector<StemAuditionTrack> tracks);

    /// @brief 列表中的文件使用共同零点，以最长音轨的排除端点结束。
    /// @param paths 完整 UTF-8 路径，作为本目录内的稳定 ID。
    /// @param decodePool 与当前任务所在池分离的解码池。
    /// @warning 仅供后台文件任务调用，等待完整解码期间可能阻塞。
    [[nodiscard]] static std::expected<StemAudition, StemAuditionError>
    loadAlignedFiles(std::span<const std::string> paths,
                     ice::ThreadPool&             decodePool);

    /// @brief 按注册顺序列出可选音轨，视图随目录失效。
    [[nodiscard]] std::span<const StemAuditionTrack> tracks() const noexcept;

    /// @brief 返回音轨组的排除端点，单位为内部采样帧。
    [[nodiscard]] AudioTimelineFrame endFrame() const noexcept;

    /// @brief 按稳定 ID 构建实时播放节点，空选择生成保持时钟的静音节点。
    /// @warning 在控制线程使用。节点接管独立的 PCM 所有权。
    [[nodiscard]] std::expected<std::shared_ptr<AudioTimelineMixerNode>,
                                StemAuditionError>
    makePlayback(std::span<const std::string> ids,
                 std::size_t                  maximumBlockFrames) const;

    /// @brief 在下一音频块切换选择，保留已有的播放位置与循环。
    /// @param playback 由当前目录通过 makePlayback 创建的节点。
    /// @param ids 新的全部选择，空列表表示静音。
    /// @param maximumBlockFrames 调用方为新调度预备的块容量。
    /// @param gains 与目录同序的线性推子值。空列表使用单位增益。
    /// @return 新调度代次或错误。无效选择不影响当前调度。
    /// @warning 仅允许控制线程调用，并按节点契约回收已退役的调度。
    [[nodiscard]] std::expected<std::uint64_t, StemAuditionError> select(
        AudioTimelineMixerNode& playback, std::span<const std::string> ids,
        std::size_t            maximumBlockFrames,
        std::span<const float> gains = {}) const;

    /// @brief 在原曲帧坐标取样，output 活动帧数决定半开区间的长度。
    /// @details 区间必须完全属于源时间轴，输出采用内部 PCM 格式。
    /// @warning 仅供后台线程使用，可能分配临时混音节点及读取离线 PCM。
    [[nodiscard]] std::expected<void, StemAuditionError> sample(
        std::span<const std::string> ids, AudioTimelineFrame firstFrame,
        ice::AudioBuffer& output) const;

private:
    /// @brief 解析选择并按固定求和顺序生成时间线调度片段。
    [[nodiscard]] std::expected<std::vector<PreparedTimelineClip>,
                                StemAuditionError>
    makeClips(std::span<const std::string> ids,
              std::span<const float>       gains = {}) const;

    /// @brief 创建通过时间轴与 ID 校验的目录。
    StemAudition(AudioTimelineFrame             sourceEndFrame,
                 std::vector<StemAuditionTrack> tracks);

    /// @brief 原曲排除端点，避免静音或短轨改变试听长度。
    AudioTimelineFrame m_sourceEndFrame;
    /// @brief 固定输入顺序确定浮点求和顺序。
    std::vector<StemAuditionTrack> m_tracks;
};

/// @brief 同一主音轨共用的分离结果、播放模式和调音台设置。
struct StemPlaybackMix {
    /// @brief 后台准备完毕的声部目录；为空时只能播放原版。
    std::shared_ptr<const StemAudition> catalog;
    /// @brief 目录顺序对应的线性推子值。
    std::vector<float> gains;
    /// @brief 目录顺序对应的通道启用状态。
    std::vector<bool> enabled;
    /// @brief 独听时只输出指定通道，保留各通道的启用设置。
    std::optional<std::size_t> solo;
    /// @brief 是否用分轨混音替换谱面中同源的主音轨。
    bool useStems{};
    /// @brief 本代分轨的本机设置路径。空路径表示未能持久化的会话结果。
    std::filesystem::path settingsPath;
    /// @brief 当前缓存的模型键；旧版未记录模型的结果使用空键。
    std::string cacheModelKey;
    /// @brief 写入缓存清单的模型显示名。
    std::string cacheModelName;
};

/// @brief 共享主音轨的 UI 状态及传给谱面混音器的不可变快照。
/// @details
/// 后台恢复与界面编辑通过短锁发布；音频回调不取锁或重置播放头。
class StemPlaybackSource final
    : public std::enable_shared_from_this<StemPlaybackSource>
{
public:
    /// @brief 取得供界面或后台保存使用的不可变状态句柄。
    /// @warning 每次绘制只取得一次，不能在音频回调复制所有权或取锁。
    [[nodiscard]] std::shared_ptr<const StemPlaybackMix> mix() const;

    /// @brief 发布完整调音台状态，未通过目录和参数校验时保留当前状态。
    /// @warning 控制或准备线程调用；文件读取和快照析构均在锁外。
    [[nodiscard]] bool publish(StemPlaybackMix mix);

    /// @brief 仅当所属分轨仍在播放源内时更新设置，丢弃已删除代次的编辑。
    /// @warning 仅界面实际更改控件时调用；不等待文件任务。
    [[nodiscard]] bool publishSettings(StemPlaybackMix mix);

    /// @brief 尝试独占同一源的分离及删除事务。
    /// @warning 仅文件线程调用，锁须在同一线程释放。
    [[nodiscard]] std::unique_lock<std::mutex> tryLockCache();

    /// @brief 删除指定模型；仅移除当前模型时恢复原版播放。
    /// @warning 后台文件操作；分离事务进行中时立即返回忙。
    [[nodiscard]] std::error_code deleteCachedPlayback(
        const std::string& filePath, std::string_view modelKey);

    /// @brief 从磁盘激活指定模型，无需再次运行分离。
    /// @warning 后台文件与解码操作，不在持有混音状态锁时读取磁盘。
    [[nodiscard]] std::error_code activateCachedPlayback(
        const std::string& filePath, std::string_view modelKey,
        ice::ThreadPool& decodePool);

    /// @brief 从本机保存的结果恢复一次；已经发布的结果优先。
    /// @warning 仅资源准备或独立文件线程调用，可能等待解码。
    void restore(const std::string& filePath, ice::ThreadPool& decodePool);

    /// @brief 将最新调音台设置提交给后台文件池，拖动期间合并连续更新。
    /// @warning 不做文件 I/O；仅在实际编辑或安装新结果时调用。
    void queueSettingsSave();

    /// @brief 在一次实时混音操作中借用分轨快照。
    /// @warning 音频热路径：原版播放只查询一次指针；分轨播放不分配、不等待。
    class Read final
    {
    public:
        /// @brief 读取当前启用的混音，空来源或原版模式均返回空。
        explicit Read(const StemPlaybackSource* source) noexcept;
        /// @brief 结束借用，由界面线程负责回收退役快照。
        ~Read();
        Read(const Read&)            = delete;
        Read& operator=(const Read&) = delete;
        /// @brief 返回当前借用的分轨配置，原版模式为空。
        [[nodiscard]] const StemPlaybackMix* get() const noexcept;

    private:
        /// @brief 需要退出读区时记录来源；由调用方的调度保活。
        const StemPlaybackSource* m_source{};
        /// @brief 本次读取的只读调音台状态。
        const StemPlaybackMix* m_mix{};
    };

private:
    /// @brief 内部发布入口，后台恢复不得覆盖用户已完成的新结果。
    [[nodiscard]] bool publishMix(StemPlaybackMix mix, bool onlyIfEmpty,
                                  bool settingsOnly = false);
    /// @brief 写出最新快照，保持同一来源的设置写入有序。
    /// @warning 仅文件工作线程执行，落盘期间不持有混音状态锁。
    void saveQueuedSettings();
    /// @brief 短时保护非实时快照及保存任务状态。
    mutable std::mutex m_mixMutex;
    /// @brief 仅文件线程访问，覆盖从推理开始到内存发布的完整事务。
    std::mutex m_cacheMutex;
    /// @brief 串行化设置落盘与切换前的保存任务，只在后台获取。
    std::mutex m_settingsWriteMutex;
    /// @brief 归并同一来源的并发加载，不重复解码整组分离文件。
    std::once_flag m_restored;
    /// @brief 已经有文件任务负责处理本源后续更新。
    bool m_saveQueued{};
    /// @brief 每代混音文件保留最后一份待保存快照，模型切换不会覆盖另一代。
    std::map<std::filesystem::path, std::shared_ptr<const StemPlaybackMix>>
        m_pendingSettings;
    /// @brief 最新发布状态，可为空；普通线程通过短锁复制持有权。
    std::shared_ptr<const StemPlaybackMix> m_current;
    /// @brief 保存可能仍被回调借用的旧快照，析构移到发布锁外。
    std::vector<std::shared_ptr<const StemPlaybackMix>> m_retired;
    /// @brief 当前实时配置；空值表示原版。
    /// @warning UI
    /// 写、音频线程读。与读者计数保持顺序一致，避免旧配置被提前释放。
    std::atomic<const StemPlaybackMix*> m_active{};
    /// @brief 音频线程正在进行的分轨借用数。
    /// @warning UI 读取以回收快照。进入必须早于指针读取，形成跨原子的回收保证。
    mutable std::atomic<unsigned> m_readers{};
};
}  // namespace MMM::Audio
