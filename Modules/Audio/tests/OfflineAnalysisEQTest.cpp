#include "audio/OfflineAnalysisEQ.h"
#include "log/colorful-log.h"

#include <ice/core/IAudioNode.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/manage/AudioBuffer.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
/// @brief 让两个滤波实现收到同一段可重复生成的双声道信号。
/// 第一个块为左声道冲激，后续静音暴露历史；右声道使用独立的非零起点。
class AnalysisSignal final : public ice::IAudioNode
{
public:
    /// @brief 将指定起点的测试信号写入整个块。
    /// @param output 两个实现之一准备好的立体声输出缓冲。
    /// @note 重复读取同一个块时必须返回同一内容，游标由测试显式推进。
    /// @warning 每个测试工作块调用，不能分配或更改 PCM 形状。
    void process(ice::AudioBuffer& output) override
    {
        for ( std::size_t i = 0; i < output.num_frames(); ++i ) {
            // 左右冲激相隔多个工作块，可检查两边历史是否独立。
            const auto frame        = offset + i;
            output.raw_ptrs()[0][i] = frame == 0 ? 1.0F : 0.0F;
            output.raw_ptrs()[1][i] = frame == 61 ? -0.5F : 0.0F;
        }
    }

    /// @brief 当前块的绝对帧偏移，两个处理器共享这一测试输入位置。
    std::size_t offset{ 0 };
};

/// @brief 比较一次启用 EQ 的离线计算与引擎播放均衡器输出。
/// @param settings 本轮分析的自包含频段快照。
/// @return 两种实现各工作块的有效 PCM 一致且相对原始信号产生可见变化。
bool matchesEngine(const MMM::Audio::OfflineAnalysisEQSettings& settings)
{
    // 不等长的连续块保留冲激尾音：重置历史、交换频段和串用声道都会改变输出。
    // 固定格式使参考滤波器和离线链使用同一采样率生成峰值滤波系数。
    // 冲激信号只依赖帧索引，不读取项目或系统声卡，确保测试环境可复现。
    // 分块包含一个单帧边界，捕捉不同实现对短块或跨块历史的处理差异。
    constexpr ice::AudioDataFormat FORMAT{ .channels = 2, .samplerate = 48000 };
    constexpr std::array<std::size_t, 4> BLOCKS{ 32, 23, 1, 32 };
    std::vector<double>                  frequencies;
    for ( const auto& band : settings ) frequencies.push_back(band.frequency);

    auto                  signal = std::make_shared<AnalysisSignal>();
    ice::GraphicEqualizer reference(frequencies);
    // 最大块在前，后续块反复缩短并延长，以使有效长度独立于容量。
    reference.prepare(FORMAT, BLOCKS.front());
    reference.set_inputnode(signal);

    // 用旧分析路径的逐项 setter 作为参数和 PCM 的参考，覆盖 Q 回退规则。
    for ( std::size_t i = 0; i < settings.size(); ++i ) {
        reference.set_band_gain_db(i, settings[i].gain);
        reference.set_band_q_factor(i, settings[i].q);
    }

    // 两个离线对象各自只构造一次，后续块之间不得用新的参数覆盖递推历史。
    // 空快照需要和启用 EQ 的输入使用相同的样本，便于检查效果确实生效。
    MMM::Audio::OfflineAnalysisEQ offline(
        settings, FORMAT.samplerate, FORMAT.channels);
    MMM::Audio::OfflineAnalysisEQ bypass(
        {}, FORMAT.samplerate, FORMAT.channels);
    ice::AudioBuffer actual, expected, input;
    bool             changed = false;
    for ( auto frames : BLOCKS ) {
        // 测试数据使用合成 PCM，缓冲无法准备即失败，不以零块跳过用例。
        // 输入、输出和参考保持相同格式与活动长度，只比较该块实际写入范围。
        if ( !actual.resize(FORMAT, frames) ||
             !expected.resize(FORMAT, frames) || !input.resize(FORMAT, frames) )
            return false;

        // 两个离线实例以同一块 PCM 起步。空快照须逐样本原样保留输入。
        signal->process(actual);
        signal->process(input);
        // 禁用场景按精确相等验证，避免用误差容限掩盖微小但多余的滤波。
        bypass.process(input);
        for ( std::uint16_t ch = 0; ch < FORMAT.channels; ++ch )
            for ( std::size_t f = 0; f < frames; ++f )
                if ( input.raw_ptrs()[ch][f] != actual.raw_ptrs()[ch][f] )
                    return false;

        // 两个实现先处理当前块再推进游标，共享一组绝对 PCM 时间点。
        reference.process(expected);
        offline.process(actual);
        for ( std::uint16_t ch = 0; ch < FORMAT.channels; ++ch ) {
            for ( std::size_t f = 0; f < frames; ++f ) {
                // 使用小容差允许不同平台的浮点聚合方式，拒绝非有限值。
                // 保留 float 写回舍入；上游 ICE 输入输出本身都使用 float。
                const float sample = actual.raw_ptrs()[ch][f];
                const float error =
                    std::abs(sample - expected.raw_ptrs()[ch][f]);
                if ( !(error <= 1e-6F) ) return false;
                // 使用比参考误差更大的阈值，确保不是舍入而是参数改变了输出。
                changed |= std::abs(sample - input.raw_ptrs()[ch][f]) > 1e-4F;
            }
        }
        // 右冲激之前存在一个仅含左侧尾音的块，应保留右声道精确静音。
        // 编排的最后一块覆盖右侧的冲激和尾音，避免测试只验证左声道。
        signal->offset += frames;
    }
    // 所有块通过后单独检查效果生效，不能只凭参考一致性判定成功。
    return changed;
}
}  // namespace

/// @brief 验证每次重算一次性应用完整参数，且一次重算内部保留声道历史。
int main()
{
    // 同时改变频率、增益和 Q；保留频段个数，避免仅判断拓扑尺寸即可通过。
    // 第二轮故意给一个非正 Q，验证跟原引擎 setter 一样使用初始品质因数。
    // 每轮重新构造后，首帧从零历史开始，不应依赖上轮测试的尾音。
    if ( matchesEngine({ { 125.0F, 8.0F, 0.8F }, { 1000.0F, -9.0F, 1.9F } }) &&
         matchesEngine({ { 160.0F, -8.0F, 0.0F }, { 1450.0F, 12.0F, 1.2F } }) )
        return 0;

    // 任一参数组失配或出现错误透传都由 CTest 记录为失败。
    XERROR("Offline analysis PCM differs from GraphicEqualizer");
    return 1;
}
