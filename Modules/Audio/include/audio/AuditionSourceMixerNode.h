#pragma once

#include <ice/config/config.hpp>
#include <ice/core/IAudioNode.hpp>
#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/manage/AudioBuffer.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace MMM::Audio
{
/// @brief 在主时间线拉伸前合并试听歌曲与按源帧预约的节拍器。
/// 歌曲先发布本块起点，再推进源游标，节拍池仍按同一块起点定位。
/// 两种声音共享后续拉伸历史，不能在 UI 层估算算法延迟并补偿。
class AuditionSourceMixerNode final : public ice::IAudioNode
{
public:
    /// @brief 在资源加载阶段建立固定容量的混音节点。
    /// @param source 独立试听源，音量只作用于歌曲。
    /// @param effects 仅包含试听节拍器的总线。
    /// @param blockStart 生命周期覆盖全部音频回调的参考帧邮箱。
    /// @param maximumFrames 拉伸器单次允许请求的最大输入帧数。
    /// @warning 低频构造会分配；图发布后来源及缓冲容量保持不变。
    AuditionSourceMixerNode(std::shared_ptr<ice::SourceNode> source,
                            std::shared_ptr<ice::MixBus>     effects,
                            std::atomic<std::size_t>&        blockStart,
                            std::size_t                      maximumFrames)
        : m_source(std::move(source))
        , m_effects(std::move(effects))
        , m_blockStart(blockStart)
        , m_effectBuffer(ice::ICEConfig::internal_format, maximumFrames)
    {
    }

    /// @brief 按歌曲本块起始帧定位节拍，并将二者送入同一个拉伸器。
    /// @warning 每音频块执行；禁止分配、等待和复制共享所有权。
    /// 原子邮箱仅由音频线程写，音效 provider 读取，不发布其他状态。
    void process(ice::AudioBuffer& buffer) override
    {
        // 主谱面可独自继续播放；暂停试听时专用节拍池不能独自消费旧预约。
        // 不暂停父总线，避免破坏编辑器的其他效果音。
        if ( !m_source->isplaying() ) {
            buffer.clear();
            return;
        }
        // 超出构造容量时拒绝整块，不能临时扩容或留下半块歌曲。
        if ( !m_effectBuffer.set_active_frames(buffer.num_frames()) ) {
            buffer.clear();
            return;
        }
        // 必须在歌曲推进游标前记录，避免节拍提前一个输入 block。
        m_blockStart.store(m_source->get_playpos(), std::memory_order_relaxed);
        m_source->process(buffer);
        m_effects->process(m_effectBuffer);
        // 这里只求和；试听源的静音和音量不应静音节拍器。
        for ( std::size_t channel = 0; channel < buffer.num_channels();
              ++channel ) {
            for ( std::size_t frame = 0; frame < buffer.num_frames();
                  ++frame ) {
                buffer.raw_ptrs()[channel][frame] +=
                    m_effectBuffer.raw_ptrs()[channel][frame];
            }
        }
    }

private:
    /// @brief 固定图边保持歌曲源有效。
    /// @warning 共享所有权只在低频建图时转移，回调仅借用稳定节点。
    std::shared_ptr<ice::SourceNode> m_source;
    /// @brief 固定图边保持节拍总线有效。
    /// @warning 回调不复制所有权，总线与源共用单个音频线程。
    std::shared_ptr<ice::MixBus> m_effects;
    /// @brief AudioManager 常驻的源帧邮箱，卸载歌曲后仍可被旧 voice 安全读取。
    /// @warning 每块由本节点单写，节拍 provider 单读；relaxed 不发布其他状态。
    std::atomic<std::size_t>& m_blockStart;
    /// @brief 构造时分配的节拍暂存空间，回调只调整活动长度。
    /// 歌曲和节拍必须保持相同活动帧数，不能独自补齐整块。
    ice::AudioBuffer m_effectBuffer;
};
}  // namespace MMM::Audio
