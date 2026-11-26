#include "audio/StemAudition.h"

#include "audio/AudioTimelineMixerNode.h"

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/thread/ThreadPool.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace MMM::Audio;

// 所有匹配均使用既有混音器输出，测试无需建立 AudioManager 或设备线程。
// 时间和采样值都是确定性的，既不取墙钟也不等待线程调度。

/// @brief 生成各声道同值的确定性 PCM，避免设备和编解码误差干扰坐标断言。
/// @param frames 单声道源帧，测试可从字面量直接看出跨轨的预期求和值。
/// @return 采用引擎内部布局的固定缓存，其生命周期与 test buffer 独立。
auto track(std::vector<float> frames)
{
    // 各声道布局一致，但 PCM 由正式工厂转移，覆盖实际节点的读取契约。
    std::vector<std::vector<float>> channels(
        ice::ICEConfig::internal_format.channels, std::move(frames));
    return PreparedTimelineAudio::fromOwnedChannels(std::move(channels));
}

/// @brief 逐声道比对读取结果，同时验证每个空白位置确实已覆盖。
/// @param buffer 可能已用于先前混音或摘录的可复用缓冲区。
/// @param expected 每声道的预期源帧，零值表示必须由节点填充静音。
/// @return 每个声道完整匹配时才返回 true。
bool matches(const ice::AudioBuffer& buffer, std::span<const float> expected)
{
    // 必须先检查长度，避免首帧正确而尾部丢失时产生假阳性。
    if ( buffer.num_frames() != expected.size() ) {
        return false;
    }
    // 以每声道独立检查空白位置，防止单声道正确掩盖另一声道短写。
    // 所有样本使用可以精确表示的二进制分数，严格相等能直接检出错位。
    for ( std::size_t channel = 0; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0; frame < expected.size(); ++frame ) {
            if ( buffer.raw_ptrs()[channel][frame] != expected[frame] ) {
                return false;
            }
        }
    }
    return true;
}
/// @brief 确认请求失败且错误符合调用契约。
/// @param result 调用方未经解引用的错误结果。
/// @param error 本次调用应携带的精确错误类别。
/// @details 先检查状态再读取错误，避免接受非法请求时测试读取无效分支。
bool failedWith(const std::expected<void, StemAuditionError>& result,
                StemAuditionError                             error)
{
    return !result && result.error() == error;
}
/// @brief 通过桌面使用的文件导入入口准备两条真实 WAV。
/// @param paths CTest 提供的只读音频资源路径。
/// @return 同一区间的实体 PCM 能在不启动声卡的情况下按轨读取。
/// @details 每条轨先独立取样，再核对双轨输出。验证真实解码路径以及文件 ID
/// 到源时钟的对应关系，避免只覆盖人工缓冲提供者。
bool verifyAlignedFiles(const std::array<std::string, 2>& paths)
{
    // 测试进程是文件任务的调用者，解码池只负责解码。
    // 池先构造后析构，引用解码 PCM 的目录会先释放。
    ice::ThreadPool decode(2);
    auto            stems = StemAudition::loadAlignedFiles(paths, decode);
    // 与合成夹具不同，文件长度只能在解码后确认。短到不足摘录时主动失败。
    if ( !stems || stems->tracks().size() != paths.size() ||
         stems->endFrame() < 257 ) {
        return false;
    }

    // 从第二帧开始读，刻意避开文件开头可能出现的统一零填充。
    const auto       format = ice::ICEConfig::internal_format;
    ice::AudioBuffer left(format, 256);
    ice::AudioBuffer right(format, 256);
    ice::AudioBuffer combined(format, 256);
    const std::span  selected(paths);
    if ( !stems->sample(selected.first(1), 1, left) ||
         !stems->sample(selected.last(1), 1, right) ||
         !stems->sample(selected, 1, combined) ) {
        return false;
    }

    // 逐声道独立读取可发现载入时单声道布局转换遗漏。
    bool audible = false;
    for ( std::size_t channel = 0; channel < combined.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0; frame < combined.num_frames(); ++frame ) {
            // 单次累加使用相同调度及增益，因此与独听的浮点和完全一致。
            const float sum = left.raw_ptrs()[channel][frame] +
                              right.raw_ptrs()[channel][frame];
            if ( combined.raw_ptrs()[channel][frame] != sum ) {
                return false;
            }
            audible |= sum != 0.0F;
        }
    }
    // 全零也满足求和关系，但无法证明解码成功且取到了有效声音。
    return audible;
}
}  // namespace

/// @brief 使用错位且长度不同的两个声部验证公共帧坐标和块边界切换。
/// @return 0 为成功，其他退出码分别定位到目录、摘录、播放、循环或切换。
/// @details 先用独立摘录建立预期，再驱动真实节点，检查选轨不会重置播放头。
/// 正常区间验证同时覆盖合并、前导零和短轨补零。边界错误检查针对 API 输入。
/// 播放阶段不启动设备，因此待发布调度总在下一次 process 精确生效。
/// 同一测试复用缓冲，可检验全关时残留 PCM 会被清零。
int main(int argc, char** argv)
{
    // 人声从第 1 帧开始，鼓从第 3 帧开始，二者仅在第 3 帧交叠。
    // 第 6 帧属于原曲但两轨均结束，可用于验证目录的独立终点。
    const auto first  = track({ 0.125F, 0.25F, 0.375F });
    const auto second = track({ 0.5F, 0.625F, 0.75F });
    auto       stems  = StemAudition::create(
        7, { { "voice", 1, first }, { "drums", 3, second } });
    if ( !stems || stems->tracks().size() != 2 ) {
        return 1;
    }

    // 请求顺序特意与目录相反，混音结果应仍遵循节点的固定顺序。
    const std::array combined{ std::string("drums"), std::string("voice") };
    // 后续两次选择共用一个载入的目录，验证不会产生新的源资源副本。
    const std::array voice{ std::string("voice") };
    const std::array drums{ std::string("drums") };
    const std::array unknown{ std::string("missing") };
    const std::array duplicate{ std::string("voice"), std::string("voice") };
    // 四帧输出比实时暂存块长，必须经过多段混音。
    ice::AudioBuffer excerpt(ice::ICEConfig::internal_format, 4);

    // 摘录在轨道中间开始，同时覆盖交叠、短轨结束和越界错误。
    // 同一缓冲区接着取独听，确保前一次混音的尾部不会污染新摘录。
    // 未知轨、重复轨和无效区间必须返回不同的明确错误。
    if ( !stems->sample(combined, 2, excerpt) ||
         !matches(excerpt, std::array{ 0.25F, 0.875F, 0.625F, 0.75F }) ||
         !stems->sample(voice, 2, excerpt) ||
         !matches(excerpt, std::array{ 0.25F, 0.375F, 0.0F, 0.0F }) ||
         !failedWith(stems->sample(unknown, 2, excerpt),
                     StemAuditionError::UnknownTrack) ||
         !failedWith(stems->sample(duplicate, 2, excerpt),
                     StemAuditionError::DuplicateTrack) ||
         !failedWith(stems->sample(combined, 4, excerpt),
                     StemAuditionError::InvalidInterval) ||
         !failedWith(stems->sample(combined, -1, excerpt),
                     StemAuditionError::InvalidInterval) ) {
        return 2;
    }

    // 从同一位置播放应与摘录逐帧一致，即使实时暂存块比输出短。
    // 这样同时检查了两个实时子块的输出偏移，而不仅验证一帧 PCM。
    // 播放头必须落在下一次将消费的第 6 帧。
    auto playback = stems->makePlayback(combined, 2);
    if ( !playback ) {
        return 3;
    }
    auto& node = **playback;
    // 先排入定位再播放，验证这些命令在首个块按约定顺序消费。
    node.seek(2);
    node.play();
    node.process(excerpt);
    if ( !matches(excerpt, std::array{ 0.25F, 0.875F, 0.625F, 0.75F }) ||
         node.positionFrame() != 6 ) {
        return 4;
    }

    // 播放中切换为独听，必须继承 seek、播放状态及循环范围。
    // 同一块会越过循环终点，从第 2 帧开始续填含静音和鼓的两个样本。
    // 这也验证尚未消费的控制命令会应用到新调度。
    if ( !node.setLoop({ 2, 5 }) || !stems->select(node, drums, 2) ) {
        return 5;
    }
    // 定位恰在两轨交叠的位置，切换后应只听到鼓。
    node.seek(3);
    node.process(excerpt);
    if ( !matches(excerpt, std::array{ 0.5F, 0.625F, 0.0F, 0.5F }) ||
         node.positionFrame() != 4 ) {
        return 6;
    }
    // 模仿宿主在音频块完成后回收旧调度，切换后的结果仍可读取。
    static_cast<void>(node.reclaimRetiredSchedules());

    // 全关继续推进时间，重新选择后从当前帧发声。
    // 这次切换不调用 seek，可检验调度替换本身保留播放头。
    // 播放头从 4 开始，输出四帧后正好落回半开循环的起点。
    // 空列表是明确的播放选择，仍使用原曲结束位置。
    if ( !stems->select(node, {}, 2) ) {
        return 7;
    }
    node.process(excerpt);
    if ( !matches(excerpt, std::array{ 0.0F, 0.0F, 0.0F, 0.0F }) ||
         node.positionFrame() != 2 || !stems->select(node, voice, 2) ) {
        return 8;
    }
    node.process(excerpt);
    // 循环后回到第 2 帧，人声的首个有效样本应再次出现。
    if ( !matches(excerpt, std::array{ 0.25F, 0.375F, 0.0F, 0.25F }) ) {
        return 9;
    }

    // 重名或在原曲结束后仍有数据的轨道在建组时拒绝。
    // 在发布前拒绝不一致输入，可防止摘录时悄悄延长原曲边界。
    if ( StemAudition::create(7,
                              { { "same", 1, first }, { "same", 2, second } })
             .has_value() ||
         StemAudition::create(4, { { "overrun", 2, first } }).has_value() ) {
        return 10;
    }
    // 临时目录与试听节点已覆盖完整内存输入路径。
    // 独立测试最后才解码真实夹具，故前面的失败码不会被文件读错掩盖。
    // CTest 明确传入两个路径，测试不依赖当前目录。
    // 最后验证与文件选择器同源的导入入口，不把合成夹具冒充真实解码。
    return argc == 3 && verifyAlignedFiles({ argv[1], argv[2] }) ? 0 : 11;
}
