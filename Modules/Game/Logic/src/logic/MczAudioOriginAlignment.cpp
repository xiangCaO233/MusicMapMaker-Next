#include "logic/MczAudioOriginAlignment.h"

#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MMM::Logic
{
namespace
{

/// @brief 时间计算使用的毫秒容差。
constexpr double ALIGNMENT_TIME_EPSILON_MS = 1.0e-6;

/// @brief 对一个持久化时间戳应用统一相位平移。
/// @param timestamp 待修改的毫秒时间戳。
/// @param phaseMilliseconds 需要从时间戳中减去的相位。
/// @note 负相位使时间后移，正相位使时间前移；两者均使用毫秒单位。
void shiftTimestamp(double& timestamp, double phaseMilliseconds) noexcept
{
    // 整体平移只改变锚点，不缩放物件时长或其他相对时间字段。
    timestamp -= phaseMilliseconds;
    if ( std::abs(timestamp) <= ALIGNMENT_TIME_EPSILON_MS ) timestamp = 0.0;
    // 清除零点附近的浮点残差，避免导出出现极小负时间或负零。
}

}  // namespace

/// @brief 根据首个 BPM 红线计算需要对齐的音频原点相位。
/// @param beatMap 未变换的谱面模型。
/// @return 成功时携带毫秒相位；缺少有效 BPM 时携带失败原因。
/// @note 只计算参数，不改变时间线、音频或物件数据。
/// @details 以首红线拍长建立原点网格，不用后续 BPM 段决定裁切相位。
/// @warning 导出参数准备路径，线性检查时间线，不用于每帧播放校准。
MczAudioOriginAlignmentTiming calculateMczAudioOriginAlignmentTiming(
    const BeatMap& beatMap)
{
    MczAudioOriginAlignmentTiming result;
    // 保持默认失败状态，全部校验通过后才设置 success，零相位也是合法结果。
    const auto firstBpm =
        // 不要求原容器已经排序，优先 BPM 类别并在其中取最早时间戳。
        std::min_element(beatMap.m_timings.begin(),
                         beatMap.m_timings.end(),
                         [](const Timing& lhs, const Timing& rhs) {
                             if ( lhs.m_timingEffect != TimingEffect::BPM )
                                 return false;
                             if ( rhs.m_timingEffect != TimingEffect::BPM )
                                 return true;
                             return lhs.m_timestamp < rhs.m_timestamp;
                         });
    if ( firstBpm == beatMap.m_timings.end() ||
         firstBpm->m_timingEffect != TimingEffect::BPM ) {
        result.errorMessage = "谱面没有可用于音频原点对齐的 BPM 红线";
        return result;
    }
    // 首红线的时间和 BPM 都参与相位运算，不能只验证 BPM 是否大于零。
    if ( !std::isfinite(firstBpm->m_timestamp) ||
         !std::isfinite(firstBpm->m_bpm) || firstBpm->m_bpm <= 0.0 ) {
        result.errorMessage = "首个 BPM 红线的时间或 BPM 无效";
        return result;
    }

    const double beatLengthMilliseconds = 60000.0 / firstBpm->m_bpm;
    // 不调用编辑器 BPM 回退策略，导出对齐必须明确拒绝不能计算的源数据。
    // 毫秒单位与持久化时间戳一致，除法后的结果还需排除溢出或退化拍长。
    if ( !std::isfinite(beatLengthMilliseconds) ||
         beatLengthMilliseconds <= 0.0 ) {
        result.errorMessage = "首个 BPM 红线无法计算有效拍长";
        return result;
    }

    result.phaseMilliseconds =
        // 只取不足一拍的有符号余量，避免按长前导时间裁切或填充整段音频。
        std::fmod(firstBpm->m_timestamp, beatLengthMilliseconds);
    // fmod 保留首时间戳符号，让前移与后移的音频处理使用同一相位约定。
    if ( std::abs(result.phaseMilliseconds) <= ALIGNMENT_TIME_EPSILON_MS ||
         std::abs(std::abs(result.phaseMilliseconds) -
                  beatLengthMilliseconds) <= ALIGNMENT_TIME_EPSILON_MS ) {
        // 浮点余量接近零或整拍边界都视为已对齐，避免无意义音频变换。
        result.phaseMilliseconds = 0.0;
    }
    // 计算成功只证明相位可求，主音频数量与零点约束仍由应用阶段检查。
    result.success = true;
    return result;
}

/// @brief 将谱面时间域平移到已经处理过的主音频原点。
/// @param beatMap 待原地修改的导出谱面。
/// @param mainAudioReferences 需要对齐的非 OGG 主音频资源引用。
/// @param phaseMilliseconds 从各物件时间中减去的有限毫秒相位。
/// @param errorMessage 失败原因，入口先清除上一次内容。
/// @return 前置检查与变换完成时返回 true，检查失败不修改谱面。
/// @pre 音频裁切或补静音由调用方负责，本函数仅调整谱面模型。
/// @note 相位必须与实际音频处理一致；此处不再次计算或替换调用方参数。
/// @note 不处理音频文件路径替换，调用方需将模型引用与处理后的资源一并导出。
/// @details 主音频原点保持零，其他事件统一平移，二者不能重复应用相位。
/// @warning 导出低频路径，完整遍历物件并同步模型，不用于交互更新。
bool applyMczAudioOriginAlignment(
    BeatMap&                               beatMap,
    const std::unordered_set<std::string>& mainAudioReferences,
    double phaseMilliseconds, std::string& errorMessage)
{
    errorMessage.clear();
    // 每次调用独立返回诊断，成功不能携带上次失败残留的错误文本。
    if ( !std::isfinite(phaseMilliseconds) ) {
        // 非有限相位会污染所有时间戳，必须在取得任何可写目标之前拒绝。
        errorMessage = "主音频原点对齐相位无效";
        return false;
    }
    if ( mainAudioReferences.empty() ) {
        // 没有明确目标资源时不能推断主音频，避免误平移普通效果音。
        errorMessage = "谱面没有可识别的非 OGG Main 自动采样";
        return false;
    }

    auto firstBpm =
        // 应用阶段重新定位红线，不持有计算阶段容器元素的外部指针。
        std::min_element(beatMap.m_timings.begin(),
                         beatMap.m_timings.end(),
                         [](const Timing& lhs, const Timing& rhs) {
                             if ( lhs.m_timingEffect != TimingEffect::BPM )
                                 return false;
                             if ( rhs.m_timingEffect != TimingEffect::BPM )
                                 return true;
                             return lhs.m_timestamp < rhs.m_timestamp;
                         });
    if ( firstBpm == beatMap.m_timings.end() ||
         firstBpm->m_timingEffect != TimingEffect::BPM ) {
        errorMessage = "谱面没有可用于音频原点对齐的 BPM 红线";
        return false;
    }

    AudioSampleEvent* pairedMainSample = nullptr;
    // 以下指针仅在本次变换内借用，后续不增删样本容器以保持地址稳定。
    double nearestMainTime = std::numeric_limits<double>::infinity();
    // 候选距离以实际触发时间的绝对值比较，局部偏移可抵消非零锚点。
    for ( auto& sample : beatMap.m_audioSamples ) {
        if ( !mainAudioReferences.contains(sample.m_audioResourceId) ) continue;
        const double absoluteTime = std::abs(sample.effectiveTimestamp());
        // 匹配使用锚点加局部偏移后的实际播放时间，而不是仅看锚点字段。
        if ( absoluteTime < nearestMainTime ) {
            // 此处仅选择零点候选，随后仍需独立检查目标引用是否重复出现。
            nearestMainTime  = absoluteTime;
            pairedMainSample = &sample;
        }
    }
    if ( pairedMainSample == nullptr ) {
        // 引用集合非空不代表本谱面实际使用其中资源，必须找到对应自动采样。
        errorMessage = "谱面没有引用目标非 OGG Main 音频的自动采样";
        return false;
    }
    if ( nearestMainTime > ALIGNMENT_TIME_EPSILON_MS ) {
        // 仅支持原点播放的主音频，延后触发不能套用全局裁头或补静音方案。
        errorMessage =
            "目标非 OGG Main 自动采样不在时间原点，无法安全裁切或补静音";
        return false;
    }
    for ( const auto& sample : beatMap.m_audioSamples ) {
        // 同一对齐音频被多次触发时无法同时保持每次局部起点，明确拒绝变换。
        if ( &sample == pairedMainSample ||
             !mainAudioReferences.contains(sample.m_audioResourceId) ) {
            // 非目标效果音不参与唯一性限制，之后仍会随谱面平移。
            continue;
        }
        errorMessage =
            "谱面包含多个目标非 OGG Main 自动采样，无法安全生成单一对齐音频";
        return false;
    }

    // 只移动首 BPM 红线的整数拍部分。相同 BPM 下整数拍移动不改变网格相位，
    // 同时避免为了很长的前导时间裁掉或补入数拍音频。旧 Malody delay
    // 不得覆盖新的零相位，否则重新导出会再次引入歌曲偏移。
    firstBpm->m_timestamp = phaseMilliseconds;
    // 所有失败检查已完成，从此处开始修改，避免返回失败时留下半平移谱面。
    if ( auto malodyProperties = firstBpm->m_metadata.timing_properties.find(
             TimingMetadataType::MALODY);
         malodyProperties != firstBpm->m_metadata.timing_properties.end() ) {
        // 查找而非创建来源属性表，没有 Malody 扩展时不引入空元数据。
        malodyProperties->second.erase("delay");
        // 只移除会再次引入旧相位的 delay，保留其他 Malody 扩展属性。
    }

    for ( auto& timing : beatMap.m_timings ) {
        // 首红线此前设成相位，此次减去相位后恰好落到零点。
        shiftTimestamp(timing.m_timestamp, phaseMilliseconds);
    }
    for ( auto& note : beatMap.m_noteData.notes ) {
        // 对所有普通音符应用相同位移，保留相互时间差而不是逐个吸附到拍线。
        shiftTimestamp(note.m_timestamp, phaseMilliseconds);
    }
    for ( auto& hold : beatMap.m_noteData.holds ) {
        // 长条仅移动起点，持续时间保持不变，因此尾部随整条一起平移。
        shiftTimestamp(hold.m_timestamp, phaseMilliseconds);
    }
    for ( auto& flick : beatMap.m_noteData.flicks ) {
        // 滑键的轨道跨度不受原点变换影响，不改横向几何数据。
        shiftTimestamp(flick.m_timestamp, phaseMilliseconds);
    }
    for ( auto& polyline : beatMap.m_noteData.polylines ) {
        // 此处只处理当前模型中折线的时间锚点，不重建其节点形状。
        shiftTimestamp(polyline.m_timestamp, phaseMilliseconds);
    }
    for ( auto& sample : beatMap.m_audioSamples ) {
        // 配对主音频由新文件承担相位处理，其余采样跟随谱面整体平移。
        if ( &sample == pairedMainSample ) continue;
        // 保留样本的原有毫秒偏移，锚点平移已调整其有效触发时间。
        shiftTimestamp(sample.m_timestamp, phaseMilliseconds);
    }
    for ( auto& annotation : beatMap.m_annotations ) {
        // 批注必须保持与音符的相对位置，不能留在变换前的时间轴上。
        shiftTimestamp(annotation.m_timestamp, phaseMilliseconds);
    }

    // 变换后的音频文件本身已在零点完成裁头或补静音，主 SOUND 不得再次携带
    // 原来的局部时间，否则 Malody 会重复应用相位。
    pairedMainSample->m_timestamp = 0.0;
    pairedMainSample->m_offsetMs  = 0;
    // 同时清除主音频的锚点与偏移，避免其中一项仍贡献旧的局部播放时间。
    beatMap.sync();
    // 完成所有持久化数据修改后统一同步模型，避免各物件类型分批发布中间状态。
    return true;
}

}  // namespace MMM::Logic
