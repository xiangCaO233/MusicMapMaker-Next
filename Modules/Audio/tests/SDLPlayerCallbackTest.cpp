#include <ice/config/config.hpp>
#include <ice/core/IAudioNode.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <ice/out/play/sdl/SDLPlayer.hpp>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <semaphore>
#include <thread>

namespace
{
// 本测试使用 SDL dummy 的真实设备回调，不通过私有成员或手工调用模拟供数。
// 固定图块为质数帧，设备请求通常为二的幂，使未提交块尾跨越多次设备取数。
// 最终混音观察器检查连续样本身份，不能只用调用次数证明没有丢帧和重复。
// 测试中的短暂等待只用于观察异步设备及生命周期屏障，不进入产品音频路径。
// 全部测试资源为内存样本；不要求声卡，也不会在源码资源目录生成文件。

/// @brief 等待真实异步设备状态，超时只限制测试进程而不影响产品时钟。
/// @param predicate 只读取原子诊断的完成条件，不在其中操作设备生命周期。
/// @return 条件在截止时间前成立时返回 true，不因超时强制销毁设备。
/// @details 调用方仍负责关闭播放器，让失败分支和成功分支共享清理协议。
template<typename Predicate> bool waitFor(Predicate predicate)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ( !predicate() ) {
        if ( std::chrono::steady_clock::now() >= deadline ) return false;
        // 测试线程让出 CPU，设备线程继续运行；这不是音频线程的供数策略。
        SDL_Delay(1);
    }
    return true;
}

/// @brief 根据绝对帧号生成可识别的有限样本，穿插非有限输入验证原有静音约定。
/// @param frame 音频源自启动起的绝对帧号，暂停和恢复不重置。
/// @return 期望的左声道设备样本，右声道取相反数。
/// @details 波形周期与图块长度不同，整块遗漏不能被相邻块相同的波形掩盖。
/// 特殊帧返回零，对应音源主动提供非有限值后应得到的静音输出。
float sampleAt(std::size_t frame)
{
    return frame % 37 == 0 ? 0.0F
                           : static_cast<float>(frame % 251 + 1) / 1024.0F;
}

/// @brief 仅由音频回调推进的连续测试源，控制侧只观察原子帧计数。
/// @details 生成和检查分别位于图入口、最终混音出口，避免共用同一缓冲。
/// 后端重复提交、跳过块尾或交换声道时，观察器会检查到样本身份差异。
/// 生成幅度位于单位区间内，不依赖设备对超幅信号采取何种限幅策略。
class SignalNode final : public ice::IAudioNode
{
public:
    /// @brief 按固定块生成左右相反的序列，并主动提供 NaN 与无穷样本。
    /// @param buffer 播放器事先清零的固定双声道平面缓冲。
    /// @details 不在测试源交错样本，使后端自己的布局转换参与验证。
    /// @warning 音频热路径不分配、不加锁；relaxed
    /// 帧计数仅供测试观察，不发布样本。
    void process(ice::AudioBuffer& buffer) override
    {
        const auto first = m_generatedFrames.load(std::memory_order_relaxed);
        if ( buffer.num_frames() != 257 ) m_wrongBlock.store(true);
        for ( std::size_t index = 0; index < buffer.num_frames(); ++index ) {
            const auto frame = first + index;
            // 两侧非有限值都必须由后端变成静音，有限值则保持声道和连续顺序。
            buffer.raw_ptrs()[0][index] =
                frame % 37 == 0 ? std::numeric_limits<float>::quiet_NaN()
                                : sampleAt(frame);
            buffer.raw_ptrs()[1][index] =
                frame % 37 == 0 ? std::numeric_limits<float>::infinity()
                                : -sampleAt(frame);
        }
        m_generatedFrames.fetch_add(buffer.num_frames(),
                                    std::memory_order_relaxed);
    }

    /// @brief 总生成帧数只作进度诊断，停止后才作为稳定计数断言。
    std::atomic<std::size_t> m_generatedFrames{ 0 };
    /// @brief 固定图块契约被改变时由回调记录，控制侧只读取。
    std::atomic<bool> m_wrongBlock{ false };
};

/// @brief 最终设备混音的连续性观察器，停止回调后才读取非原子游标。
/// @details 单一 dummy 设备串行调用，控制线程不读取正在修改的样本游标。
/// 完成计数只决定测试何时暂停或停止，不充当缓冲所有权发布协议。
/// 错误标志保持为真，不能因后续恰好正确的块覆盖已经发生的错误。
struct OutputProbe {
    /// @brief 回调独占的下一期望帧，跨暂停和重新启动保留以检测块尾丢失。
    std::size_t m_nextFrame{ 0 };
    /// @brief 向控制侧报告已核对帧数，只用于测试完成条件。
    std::atomic<std::size_t> m_verifiedFrames{ 0 };
    /// @brief 格式或样本错误的粘性标志，由设备回调写、控制侧读。
    std::atomic<bool> m_failed{ false };

    /// @brief 检查交错声道及连续样本，包含残余块尾与非有限样本静音。
    /// @param context 生命周期覆盖设备回调的观察器地址。
    /// @param spec 最终输出格式，须保持夹具要求的采样率和声道数。
    /// @param samples SDL 最终混音缓冲，只检查，不改变输出。
    /// @param bytes 缓冲字节数，不能误当成帧数或单声道样本数。
    /// @warning 每设备块执行；不分配、不等待、不访问播放器控制接口。
    static void SDLCALL inspect(void* context, const SDL_AudioSpec* spec,
                                float* samples, int bytes)
    {
        auto& probe = *static_cast<OutputProbe*>(context);
        if ( spec->channels != 2 || spec->freq != 48000 ) {
            // dummy 夹具须保持原采样率，避免把重采样差异误判为后端丢帧。
            probe.m_failed.store(true);
            return;
        }
        const auto frames =
            static_cast<std::size_t>(bytes) / (2 * sizeof(float));
        for ( std::size_t frame = 0; frame < frames; ++frame ) {
            const auto expected = sampleAt(probe.m_nextFrame++);
            if ( std::abs(samples[frame * 2] - expected) > 1.0e-6F ||
                 std::abs(samples[frame * 2 + 1] + expected) > 1.0e-6F ||
                 !std::isfinite(samples[frame * 2]) ||
                 !std::isfinite(samples[frame * 2 + 1]) ) {
                probe.m_failed.store(true);
            }
        }
        probe.m_verifiedFrames.store(probe.m_nextFrame,
                                     std::memory_order_relaxed);
    }
};

/// @brief 验证暂停、停止、重复启动及重开设备，样本身份跨暂停和停止保持连续。
/// @return 所有生命周期转换和最终输出身份均符合契约时返回 true。
/// @details 进度只表示已有足够数据可核对，最终判断在回调屏障后进行。
/// 同一流重新启动时，源已生成的尾部也须继续播放，不能只检查总帧数。
/// 重开设备会销毁旧流，只验证新流独立启动，不要求延续旧流的最终混音。
bool testPlayback()
{
    auto signal = std::make_shared<SignalNode>();
    // 观察器先于播放器构造，提前返回时播放器也会先禁用其借用的回调。
    OutputProbe    probe;
    ice::SDLPlayer player(ice::AudioDataFormat{ 2, 48000 });
    player.set_source(signal);
    if ( !player.open() ) return false;
    SDL_Delay(20);
    if ( signal->m_generatedFrames.load() != 0 || player.is_running() )
        return false;
    if ( !SDL_SetAudioPostmixCallback(
             player.get_current_device(), OutputProbe::inspect, &probe) ||
         !player.start() || player.start() )
        return false;
    if ( !waitFor([&] { return probe.m_verifiedFrames.load() >= 4096; }) )
        return false;

    // pause 返回须完成回调屏障；暂停期间不再推进图，也不提前生成多块静音。
    player.pause();
    const auto pausedFrames = signal->m_generatedFrames.load();
    SDL_Delay(20);
    if ( !player.is_running() ||
         signal->m_generatedFrames.load() != pausedFrames )
        return false;
    const auto pausedOutput = probe.m_verifiedFrames.load();
    // 原观察器继续使用绝对游标，恢复首个样本必须紧接暂停前的末尾。
    player.resume();
    if ( !waitFor([&] {
             return probe.m_verifiedFrames.load() >= pausedOutput + 2048;
         }) )
        return false;

    // stop/start 保留同一流的样本顺序，尤其不能丢掉尚未提交的固定块尾。
    player.stop();
    const auto stoppedFrames = signal->m_generatedFrames.load();
    SDL_Delay(20);
    if ( player.is_running() ||
         signal->m_generatedFrames.load() != stoppedFrames )
        return false;
    player.stop();
    const auto stoppedOutput = probe.m_verifiedFrames.load();
    // 重复 stop 必须幂等，不能重复重置回调注册状态或本地块尾。
    if ( !player.start() || !waitFor([&] {
             return probe.m_verifiedFrames.load() >= stoppedOutput + 2048;
         }) )
        return false;
    player.stop();
    // 卸载观察器本身也等待在途 postmix，随后读取稳定结果。
    SDL_SetAudioPostmixCallback(player.get_current_device(), nullptr, nullptr);
    if ( probe.m_failed.load() || signal->m_wrongBlock.load() ) return false;
    player.close();
    player.close();
    if ( player.get_current_device() != 0 || player.start() ) return false;

    // 重开不依赖已销毁流的注册状态，空源仍须安全输出静音并正常停止。
    player.set_source({});
    if ( !player.open() || !player.start() ) return false;
    SDL_Delay(20);
    player.close();
    return !player.is_running();
}

/// @brief 故意破坏输出活动长度，验证设备回调拒绝不完整块并报告停止状态。
/// @details 只改变合法容量内的活动区间，不引入越界写入或释放中的缓冲访问。
/// 失败须由后端状态体现，不能依靠崩溃或设备偶然播放静音来发现。
class InvalidNode final : public ice::IAudioNode
{
public:
    /// @brief 缩短活动长度而不分配，不能把旧缓冲尾部提交给 SDL。
    /// @param buffer 长度大于零的固定图块，缩短后仍持有原有容量。
    /// @warning 仅供测试的音频块错误输入，不执行 IO、等待或分配。
    void process(ice::AudioBuffer& buffer) override
    {
        buffer.set_active_frames(buffer.num_frames() - 1);
    }
};

/// @brief 验证错误路径禁用后仍执行回调屏障，损坏块不能直接重新启动。
/// @return 回调报告停止且重启拒绝损坏缓冲时返回 true。
/// @details is_running 为 false 不能证明回调已结束，须 stop 后再重启。
/// 局部播放器析构继续覆盖错误后的 close，避免只验证正常路径的销毁。
bool testInvalidBuffer()
{
    ice::SDLPlayer player(ice::AudioDataFormat{ 2, 48000 });
    player.set_source(std::make_shared<InvalidNode>());
    if ( !player.open() || !player.start() ) return false;
    if ( !waitFor([&] { return !player.is_running(); }) ) return false;
    player.stop();
    return !player.start();
}

/// @brief 用可控门闩保持一次图处理在途，检查生命周期操作不会提前返回。
/// @details 门闩由测试线程释放，不依赖声卡完成某个特定播放周期。
/// 音源由测试和播放器共同持有，验证结束后才释放用于同步的成员。
class BlockingNode final : public ice::IAudioNode
{
public:
    /// @brief 首块等待测试线程放行，后续块直接返回播放器预先清零的样本。
    /// @details 只阻塞首块，避免放行后的后续设备请求再次陷入等待。
    /// @warning 仅供屏障回归测试，故意阻塞设备线程，不可用于产品音频图。
    void process(ice::AudioBuffer&) override
    {
        if ( m_enteredOnce ) return;
        m_enteredOnce = true;
        m_entered.release();
        m_release.acquire();
    }
    /// @brief 设备线程发布已经进入图处理，控制测试线程读取。
    std::binary_semaphore m_entered{ 0 };
    /// @brief 控制测试线程允许设备回调返回，确保阻塞区间可重复验证。
    std::binary_semaphore m_release{ 0 };
    /// @brief 仅设备回调访问，保证门闩只阻塞首块。
    bool m_enteredOnce{ false };
};

/// @brief stop 和 close 都必须等待真正的在途图处理结束，才允许调用方释放源。
/// @param closeDevice 为 true 时检查销毁流，否则检查保留设备的停止操作。
/// @return 控制操作等待图处理放行且最终报告停止时返回 true。
/// @details 控制线程与设备线程分离，避免回调自行停止而制造非产品死锁。
/// 放行后始终回收控制线程，提前失败也不能留下对播放器的悬空访问。
bool testCallbackBarrier(bool closeDevice)
{
    auto           signal = std::make_shared<BlockingNode>();
    ice::SDLPlayer player(ice::AudioDataFormat{ 2, 48000 });
    player.set_source(signal);
    if ( !player.open() || !player.start() ) return false;
    if ( !signal->m_entered.try_acquire_for(std::chrono::seconds(2)) ) {
        // 超时时也放行可能迟到的回调，避免清理过程被测试自身门闩卡住。
        signal->m_release.release();
        return false;
    }
    std::binary_semaphore begun{ 0 }, finished{ 0 };
    // begun 确认控制线程已获得执行机会；finished 检查控制操作返回边界。
    std::thread controller([&] {
        begun.release();
        if ( closeDevice )
            player.close();
        else
            player.stop();
        finished.release();
    });
    begun.acquire();
    // 操作未完成前源仍被回调借用；测试等待不进入实际设备控制实现。
    const bool returnedEarly =
        finished.try_acquire_for(std::chrono::milliseconds(20));
    signal->m_release.release();
    controller.join();
    return !returnedEarly && !player.is_running();
}

/// @brief 在播放器销毁后恢复测试修改的全局块大小并退出 SDL 音频子系统。
/// @details 局部析构顺序保证各场景播放器先退出，SDL 后端最后释放。
/// 不在音频线程修改全局格式，夹具也遵守产品的控制线程约束。
struct BackendScope {
    /// @brief 其他测试可能使用的原始默认块大小。
    uint32_t m_originalBlockFrames{ ice::ICEConfig::default_buffer_size };
    /// @brief 所有局部播放器均先析构，随后才能安全退出 SDL。
    ~BackendScope()
    {
        ice::ICEConfig::default_buffer_size = m_originalBlockFrames;
        ice::SDLPlayer::quit_backend();
        SDL_Quit();
    }
};
}  // namespace

/// @brief 返回码分别标识初始化、连续播放、错误缓冲、stop 屏障和 close
/// 屏障失败。
/// @return 成功为零，非零按场景定位失败，不依赖可能禁用的 assert。
/// @details 配置只影响独立测试进程，不改写用户设置或项目默认配置。
int main()
{
    // dummy 不访问真实声卡；短设备周期与质数图块确保分段供数路径得到执行。
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "128");
    if ( !ice::SDLPlayer::init_backend() ) return 1;
    // 只在后端初始化成功后创建清理守卫，避免失败路径错误释放未取得的引用。
    // 块长只影响随后构造的播放器，不在设备线程运行期间修改该静态配置。
    // 本测试验证调度和样本身份，不把 dummy 的完成耗时当作实际设备延迟。
    // 各场景局部对象先清理，即使中途失败，守卫也不会抢先退出音频子系统。
    BackendScope backend;
    ice::ICEConfig::default_buffer_size = 257;
    if ( !testPlayback() ) return 2;
    if ( !testInvalidBuffer() ) return 3;
    if ( !testCallbackBarrier(false) ) return 4;
    if ( !testCallbackBarrier(true) ) return 5;
    return 0;
}
