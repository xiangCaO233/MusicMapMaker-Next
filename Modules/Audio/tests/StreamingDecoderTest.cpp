#include "audio/AudioTimelineMixerNode.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/manage/dec/IDecoderFactory.hpp>
#include <ice/manage/dec/IDecoderInstance.hpp>
#include <ice/manage/dec/StreamingDecoder.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
/// @brief 测试源跨越缓存容量，末尾使用不整页长度检查裁剪。
constexpr std::size_t TOTAL = 16384U * 40U + 31U;
/// @brief 非零且按帧可重建的参考 PCM，便于区分缺页静音与实际数据。
/// @param frame 目标格式下的绝对帧，取模使参考值不会随长音频溢出。
/// @param channel 声道序号，每声道偏置不同以检查通道顺序。
/// @return 严格大于零的确定性样本，与静音填充结果可区分。
float sample(std::size_t frame, std::size_t channel)
{
    return 0.125F + static_cast<float>(frame % 251U) * 0.001F +
           static_cast<float>(channel) * 0.1F;
}
/// @brief 在后台读请求处人为挂起 IO，证明回调不等待解码。
/// 门闩仅作用于测试后端，产品代码不依赖测试线程的执行速度。
/// 主线程故意在后台进入 read 后才调用音轨读取，稳定复现 IO 未完成状态。
/// 同一测试只有一个后端消费者，不需要支持多个线程争用二元信号量。
struct ReadGate {
    /// @brief 第二次读取会发布进入状态，主线程只观察该标志。
    std::atomic<bool> m_entered{ false };
    /// @brief 主测试完成非阻塞检查后才允许后台继续。
    std::binary_semaphore m_release{ 0 };
    /// @brief 记录后端收到的最大分块容量，防止退化为全量读取。
    std::atomic<std::size_t> m_maximumRequest{ 0 };
};
/// @brief 无文件依赖的确定性解码器，支持精确回到文件头。
/// 后端自己不缓存 PCM，每次按绝对帧重建样本，因此不会掩盖流式层的位置错误。
/// 只允许 seek(0) 是刻意的接口约束，用于发现未经裁剪的任意 seek 调用。
/// 整段数据超过页缓存容量，使测试能够覆盖真正发生淘汰之后的读取。
class RampDecoder final : public ice::IDecoderInstance
{
public:
    /// @brief 保存目标格式及测试门闩，游标由实例独占。
    /// @param format 测试要求的输出格式，以值保存且不在运行期间改变。
    /// @param gate 跨测试线程和解码线程共享的门闩，构造后不复制所有权。
    RampDecoder(ice::AudioDataFormat format, std::shared_ptr<ReadGate> gate)
        : m_format(format), m_gate(std::move(gate))
    {
    }
    /// @brief 只接受从头重建，确保流式层不假定后端精确任意定位。
    /// @param frame 本测试只接受零，非零请求必须由流式层自己顺序跳过。
    /// @return 返回成功时下一帧一定是参考信号的开头。
    bool seek(std::size_t frame) override
    {
        if ( frame != 0 ) return false;
        m_cursor = 0;
        return true;
    }
    /// @brief 首块立即产生，首次预读等待测试释放门闩。
    /// @param output 引擎提供的声道平面，本测试从不在后端保留其地址。
    /// @param frames 本次容量，最大值用于验证未请求整文件输出缓冲。
    /// @return 实际可写帧数，末尾不补齐，以测试流式层自己的 EOF 裁剪。
    std::size_t read(float** output, std::size_t frames) override
    {
        m_gate->m_maximumRequest.store(
            std::max(frames, m_gate->m_maximumRequest.load()));
        // 第一次调用来自创建流程，不能阻塞，否则测试拿不到解码器句柄。
        // 第二次才是后台预读；发布 entered 后暂停可以可靠制造缓存缺页。
        // 后续定位仍复用该实例，不再次阻塞，以免取消测试需要额外释放协议。
        if ( ++m_reads == 2 ) {
            m_gate->m_entered.store(true, std::memory_order_release);
            m_gate->m_release.acquire();
        }
        // 以剩余真实帧数裁剪，而不是根据引擎请求量产生虚假的延长音频。
        // 测试末尾特意不是整页，使引擎必须区分容量与实际已读取前缀。
        const auto count = std::min(frames, TOTAL - m_cursor);
        for ( std::size_t channel = 0; channel < m_format.channels;
              ++channel ) {
            for ( std::size_t frame = 0; frame < count; ++frame ) {
                output[channel][frame] = sample(m_cursor + frame, channel);
            }
        }
        // 只在写入完成后推进游标，后续连续读与从头重放保持同一参考序列。
        m_cursor += count;
        return count;
    }
    /// @brief 输出格式在整个实例生命周期内不变。
    const ice::AudioDataFormat& get_source_format() const override
    {
        return m_format;
    }
    /// @brief 已知目标帧数，不依赖工作线程进度。
    std::size_t get_source_total_frames() const override { return TOTAL; }

private:
    /// @brief 目标声道布局和采样率。
    ice::AudioDataFormat m_format;
    /// @brief 测试控制的后台读取门闩。
    std::shared_ptr<ReadGate> m_gate;
    /// @brief 顺序目标帧游标。
    std::size_t m_cursor{ 0 };
    /// @brief 区分同步首块和后台首次预读。
    std::size_t m_reads{ 0 };
};
/// @brief 为流式实现提供可控的长音频，不分配整段 PCM。
/// 工厂不保存音频样本，只负责把受控后端接入实际 AudioTrack 创建流程。
/// 这样 PreparedTimelineAudio 的策略选择也由真实音轨驱动，而非替身分支。
/// 测试保留工厂直到轨道销毁，门闩不会在后台仍持有它时提前失效。
class RampFactory final : public ice::IDecoderFactory
{
public:
    /// @brief 所有者在测试退出前保留门闩。
    std::shared_ptr<ReadGate> m_gate{ std::make_shared<ReadGate>() };
    /// @brief 构造确定的元信息，避免测试依赖磁盘编码器。
    /// @param info 必须填入源格式与帧数，其他字段清空以避免未初始化元数据。
    /// @return 总是成功；失败路径由真实媒体和空输入检查分别覆盖。
    bool probe(std::string_view, ice::MediaInfo& info) const override
    {
        info             = {};
        info.format      = ice::ICEConfig::internal_format;
        info.frame_count = TOTAL;
        return true;
    }
    /// @brief 每个实例拥有独立的顺序游标。
    /// @return 唯一拥有的后端，多个音轨不能共用同一个顺序游标。
    std::unique_ptr<ice::IDecoderInstance> create_instance(
        std::string_view, const ice::AudioDataFormat& format) const override
    {
        return std::make_unique<RampDecoder>(format, m_gate);
    }
};

/// @brief 测试线程等待异步条件，超时后返回失败而非无限挂起。
/// 测试中的 yield 不属于产品播放或同步逻辑。
template<
    class
    Predicate>  /// @param predicate
                /// 每次重新读取最新状态，不把先前的静音当作成功证据。
                /// @return 在有界测试期限内观察到成功条件时为真。
                /// 测试可以等待后台完成，但等待绝不发生在正在验证的音频读取函数中。
bool awaitCondition(Predicate predicate)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ( !predicate() ) {
        if ( std::chrono::steady_clock::now() >= deadline ) return false;
        std::this_thread::yield();
    }
    return true;
}
/// @brief 检查所有请求样本，避免只验证首值而漏掉页边界或声道错误。
/// @param buffer 已读取的目标缓冲区，容量可以大于实际有效前缀。
/// @param start 原始请求的绝对帧，用来重建对应位置的参考样本。
/// @param count 本次有效帧数，EOF 后未写入尾部不参与比较。
/// @return 每个声道、每一帧均与确定性信号相同时返回 true。
bool matches(const ice::AudioBuffer& buffer, std::size_t start,
             std::size_t count)
{
    for ( std::size_t channel = 0; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0; frame < count; ++frame ) {
            if ( buffer.raw_ptrs()[channel][frame] !=
                 sample(start + frame, channel) )
                return false;
        }
    }
    return true;
}
/// @brief 覆盖缺页、恢复、跨页、淘汰后倒退、EOF 以及上层分块读取。
/// @return 只有异步恢复和所有样本比较均成功时才接受该实现。
/// 后台门闩与带期限轮询分别验证不等待和最终可读取这两个独立契约。
/// 一次读取返回正帧数不能证明音频已到达，因为缺页会交付等长静音。
/// 因此就绪判断必须同时比较实际样本，而非只检查返回值或长度。
bool testStreaming()
{
    ice::ThreadPool pool(1);
    auto            factory = std::make_shared<RampFactory>();
    auto            track   = ice::AudioTrack::create(
        "ramp", pool, factory, ice::CachingStrategy::STREAMING);
    // 无论前面的验证是否成功，都必须释放门闩后才能析构工作线程。
    if ( !track ) {
        factory->m_gate->m_release.release();
        return false;
    }
    const bool       entered = awaitCondition([&] {
        return factory->m_gate->m_entered.load(std::memory_order_acquire);
    });
    ice::AudioBuffer buffer(ice::ICEConfig::internal_format, 512);
    // 使用非零哨兵验证缺页清零确实执行，而不是偶然保留默认缓冲内容。
    std::fill_n(buffer.raw_ptrs()[0], 512, -1.0F);
    const auto begin = std::chrono::steady_clock::now();
    const auto got   = track->read(buffer, 16384, 512);
    // 缺页返回的正长度表示播放时间继续前进，缓冲内容必须由本次调用清零。
    // 不把调用前的默认初始化当作静音证据，下面检查整个有效声道前缀。
    const bool silent = std::all_of(buffer.raw_ptrs()[0],
                                    buffer.raw_ptrs()[0] + 512,
                                    [](float value) { return value == 0.0F; });
    const bool immediate = std::chrono::steady_clock::now() - begin <
                           std::chrono::milliseconds(100);
    // 释放操作必须先于所有失败返回，保证测试本身不会把产品析构挂在门闩上。
    // 即便读取检查失败，也允许工作线程自然退出并回收状态。
    factory->m_gate->m_release.release();
    if ( !entered || !immediate || !silent || got != 512 ) return false;

    // 时间线适配器必须接受没有 origin 视图的流式音轨。
    // 如果仍要求完整视图，该工厂将直接返回空对象，后续声音无法进入混音图。
    // 同时检查 channel 为空，确保测试没有悄悄切回完整 PCM 缓存路径。
    auto prepared = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !prepared || !prepared->channel(0).empty() ||
         prepared->numFrames() != TOTAL )
        return false;
    // 依次驱动多个远距离页，超过缓存容量后再次回到旧页，校验精确恢复。
    // 起点放在页尾前一百帧，每次读取同时覆盖当前页和下一页。
    // 顺序推进超过缓存页数，使最开始的页面必然需要重新准备。
    // 轮询仍使用正常读取入口，验证缺页请求本身能够驱动后台进展。
    for ( std::size_t page = 1; page < 36; ++page ) {
        const std::size_t start = page * 16384 - 100;
        if ( !awaitCondition([&] {
                 return prepared->read(buffer, start, 512) == 512 &&
                        matches(buffer, start, 512);
             }) )
            return false;
    }
    // 一个位置触发淘汰后的倒退，另一个触发短尾页和超出文件的请求裁剪。
    // 两者都不能读到 scratch 或其他页残留的样本。
    for ( const auto start : { std::size_t(17), TOTAL - 31 } ) {
        const auto wanted = std::min(std::size_t(512), TOTAL - start);
        if ( !awaitCondition([&] {
                 return track->read(buffer, start, 512) == wanted &&
                        matches(buffer, start, wanted);
             }) )
            return false;
    }
    // 文件尾返回零；无效参数和 origin 都不能暴露失效缓存地址。
    std::vector<std::span<const float>> views;
    track->origin(views, 0, 512);
    // 空 origin 容器证明流式实现没有把短期页地址伪装成长期借用。
    // 最大后端请求量验证长音频处理仍然分块，不能按媒体总长度分配输出。
    return views.empty() && track->read(buffer, TOTAL, 512) == 0 &&
           factory->m_gate->m_maximumRequest.load() <= 16384;
}

/// @brief 同一路径切换策略必须创建相应音轨，旧引用仍保持有效。
/// @param path 只读测试资源，可以来自 CTest 默认 WAV 或外部编码格式探针。
/// @return 策略隔离、分块样本及单帧排尾均与完整缓存一致时返回 true。
/// 真实文件用例补充模拟后端无法覆盖的解码延迟、重采样历史和格式差异。
/// 流式缓存容量不需要暴露给上层，用足够长的外部探针驱动真实页淘汰。
bool testRealFile(const std::string& path)
{
    ice::ThreadPool threads(2);
    ice::AudioPool  pool;
    auto            cached =
        pool.get_or_load(threads, path, ice::CachingStrategy::CACHY).lock();
    auto streamed =
        pool.get_or_load(threads, path, ice::CachingStrategy::STREAMING).lock();
    if ( !cached || !streamed || cached == streamed ||
         cached->num_frames() == 0 )
        return false;
    // 长度使用已解码参考值，不能只比较两个策略各自报告的容器估计。
    // 先完成缓存准备，再访问稳定 PCM，从而将等待限制在测试控制侧。
    const auto       total = cached->num_frames();
    ice::AudioBuffer expected(ice::ICEConfig::internal_format, 512);
    ice::AudioBuffer actual(ice::ICEConfig::internal_format, 512);
    // 跨全部页读取后再倒退，实际压缩格式也必须保持与完整缓存一致的样本位置。
    std::vector<std::size_t> positions;
    for ( std::size_t frame = 0; frame < total; frame += 16384 )
        positions.push_back(frame);
    positions.push_back(std::min(std::size_t(17), total - 1));
    positions.push_back(total - 1);
    // 压缩格式在倒退后可能需要重新处理初始延迟，故逐帧比较而非听感判断。
    // 允许后台异步准备，但不允许用相邻位置的相似波形通过定位校验。
    // 比较容差只容纳浮点舍入，不允许用近似能量代替实际样本位置。
    // 尤其是压缩格式的初始延迟变化，整体时间错位也必须使本用例失败。
    // 最后一个单帧位置覆盖尾页，而不仅检查每页开头的连续区域。
    for ( const auto start : positions ) {
        const auto count = std::min(std::size_t(512), total - start);
        cached->read(expected, start, count);
        if ( !awaitCondition([&] {
                 if ( streamed->read(actual, start, count) != count )
                     return false;
                 for ( std::size_t channel = 0; channel < actual.num_channels();
                       ++channel ) {
                     for ( std::size_t frame = 0; frame < count; ++frame ) {
                         if ( std::abs(actual.raw_ptrs()[channel][frame] -
                                       expected.raw_ptrs()[channel][frame]) >
                              1.0e-6F )
                             return false;
                     }
                 }
                 return true;
             }) ) {
            XERROR("Streaming PCM differs at frame {} in {}", start, path);
            return false;
        }
    }
    // 用单帧读取验证重采样排尾余量：请求小于尾部时不可把余下样本丢掉。
    // 完整缓存是相同后端的大块读取参考，不需要另一个编码器的舍入假设。
    ice::FFmpegDecoderFactory factory;
    auto                      instance =
        factory.create_instance(path, ice::ICEConfig::internal_format);
    // 两种策略都以 seek(0) 建立一致起点；后端新打开状态未必等同于定位后状态。
    // 这里复现同一初始化协议，单帧探针只改变输出块长，不改变定位语义。
    if ( !instance || !instance->seek(0) ) return false;
    // 单帧目标缓冲逼迫重采样尾部跨越多次 read，能稳定触发丢余量的旧缺陷。
    // 缓存和实例使用完全相同的输出格式，比较不混用源与目标采样率。
    std::size_t read = 0;
    while ( instance->read(actual.raw_ptrs(), 1) != 0 ) {
        if ( read >= total || cached->read(expected, read, 1) != 1 )
            return false;
        for ( std::size_t channel = 0; channel < actual.num_channels();
              ++channel ) {
            if ( std::abs(actual.raw_ptrs()[channel][0] -
                          expected.raw_ptrs()[channel][0]) > 1.0e-6F )
                return false;
        }
        ++read;
    }
    // 如果单帧读少于参考长度，说明后端把暂时容不下的排尾样本丢弃了。
    // 如果多于参考长度，前面的越界保护已经失败，不允许用截断掩盖差异。
    if ( read != total ) return false;
    // 再切回完整缓存时不能把正在播放的流式对象作为分析资源返回。
    // 还要保持最初缓存对象的身份，否则交替分析与播放会不断重复加载。
    // 两种策略的对象都被外部强引用保活，可排除正常资源回收造成的地址变化。
    auto analysis =
        pool.get_or_load(threads, path, ice::CachingStrategy::CACHY).lock();
    return analysis && analysis == cached && analysis != streamed &&
           analysis->cachingStrategy() == ice::CachingStrategy::CACHY;
}
/// @brief 设置往返及旧配置回退，保持默认编辑体验。
/// @return 新偏好可以往返，缺少字段的旧配置仍保持完整缓存。
/// 本测试只处理内存 JSON，不加载或保存个人软件配置文件。
/// 将旧配置恢复到已经启用流式的对象，验证反序列化确实重置默认值。
bool testSettings()
{
    MMM::Config::EditorSettings settings;
    settings.audioDecodingMode = MMM::Config::AudioDecodingMode::Streaming;
    nlohmann::json json;
    to_json(json, settings);
    MMM::Config::EditorSettings restored;
    from_json(json, restored);
    if ( restored.audioDecodingMode !=
         MMM::Config::AudioDecodingMode::Streaming )
        return false;
    // 首次反序列化留下 Streaming，再删除字段，确保默认值不是偶然来自构造。
    // 这覆盖从新版偏好对象读取旧项目环境时的实际状态迁移。
    json.erase("audioDecodingMode");
    from_json(json, restored);
    if ( restored.audioDecodingMode != MMM::Config::AudioDecodingMode::Cached )
        return false;
    // 未知文本也必须回退，避免未来版本写入的枚举悄悄改变旧版播放行为。
    json["audioDecodingMode"] = "future-mode";
    from_json(json, restored);
    return restored.audioDecodingMode == MMM::Config::AudioDecodingMode::Cached;
}
}  // namespace

/// @brief 所有数据均在内存生成，真实音频只读访问传入资源。
/// 任一数据比较失败立即终止探针，不能让后续成功掩盖前一格式的回归。
/// 用例不初始化播放器设备，允许在没有音频硬件的构建机上运行。
/// @param argc 必须提供一个只读音频路径，不从个人最近项目选择资源。
/// @param argv 参数一来自 CTest 或显式隔离配置下的格式探针。
/// @return 零表示全部通过；各失败路径输出对应类别，便于定位回归。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 2;
    if ( !testStreaming() ) {
        XERROR("Streaming paging regression failed");
        return 1;
    }
    if ( !testRealFile(argv[1]) ) {
        XERROR("Streaming FFmpeg/pool regression failed");
        return 1;
    }
    if ( !testSettings() ) {
        XERROR("Decoding settings regression failed");
        return 1;
    }
    return 0;
}
