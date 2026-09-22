#include "logic/ecs/system/HitFXSystem.h"

#include "audio/StereoGainEnvelope.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{

// 测试验证声像包络及皮肤序列的 CPU 快照，不加载音频文件、不启动声卡或图形设备。
// 资源键采用符号字符串，sample.wav 无需存在；不能据此声称实际混音链路已验证。

/// @brief 使用小容差比较声道增益。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
/// @note 比较的是线性增益，不转换成分贝，也不模拟听感上的等响度误差。
bool near(float lhs, float rhs)
{
    // 同一比较器也用于像素矩形检查，夹具使用确定的小数，不涉及屏幕采样误差。
    // 严格小于容差；非有限差值不能通过比较，避免异常增益被视为匹配。
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 创建用于声像计算测试的打击事件。
/// @param type 物件类型。
/// @param trackIndex 零起始轨道索引。
/// @param trackOffset Flick 滑动轨道偏移。
/// @return 固定时间的打击事件。
/// @note 默认是无采样绑定的独立玩家物件，草稿用例需显式补充 isDraft。
/// @note trackSpan 固定为 1，声像终点由 trackOffset
/// 决定，不以覆盖轨数替代轨差。
MMM::Logic::System::HitFXSystem::HitEvent makeEvent(MMM::NoteType type,
                                                    int           trackIndex,
                                                    int trackOffset = 0)
{
    using HitEvent = MMM::Logic::System::HitFXSystem::HitEvent;
    // 返回独立值对象，各用例修改绑定或草稿标志不会改变其他用例的事件。
    // 时间和持续时间置零，将测试变化限制在类型、轨道与声像开关。
    // Role::None 排除折线首尾角色，不让角色规则干扰独立物件的声像断言。
    return {
        0.0, type, HitEvent::Role::None, 1, trackIndex, trackOffset, 0.0, false,
    };
}

/// @brief 验证普通物件按物件中心获得固定双声道音量。
/// @return 四轨第二轨物件得到左 0.625、右 0.375 时返回 true。
/// @note 起点和终点增益相同才证明包络固定，不能只检查音效开始时的位置。
bool testStaticTrackPosition()
{
    // 这里约定线性左右分配，不使用平方和为 1 的等功率声像算法。
    const auto envelope =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::NOTE, 1), 4, true);
    // 第二轨中心为 (1 + 0.5) / 4 = 0.375，右增益取该位置，左增益取补数。
    // 另验左右和为 1，避免位置比例正确但整体被额外放大的实现通过测试。
    if ( !near(envelope.startLeft, 0.625F) ||
         !near(envelope.startRight, 0.375F) ||
         !near(envelope.endLeft, 0.625F) || !near(envelope.endRight, 0.375F) ||
         !near(envelope.startLeft + envelope.startRight, 1.0F) ) {
        XERROR("Static hit effect stereo position did not match track center");
        return false;
    }
    return true;
}

/// @brief 验证 Flick 音效从起始轨道线性移动到目标轨道。
/// @return 四轨第二轨滑向第三轨时首尾和中点音量符合预期。
/// @note 通过纯包络插值检查半程，不靠真实音频持续时间或等待播放到中点。
bool testFlickMovesAcrossChannels()
{
    // 本场景使用正一轨差验证向右移动，负轨差与跨多轨移动不由这一组断言覆盖。
    const auto envelope =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::FLICK, 1, 1), 4, true);
    const auto middle = MMM::Audio::stereoGainAtProgress(envelope, 0.5F);
    // 使用生产插值入口消费包络，不在测试中另写插值函数后仅检查自算结果。
    // 起点 0.375、终点 0.625 关于中心对称，中点两声道应各为 0.5。
    // 首尾使用相反偏向，能识别声道不动、方向反转或轨差未生效的问题。
    if ( !near(envelope.startLeft, 0.625F) ||
         !near(envelope.startRight, 0.375F) ||
         !near(envelope.endLeft, 0.375F) || !near(envelope.endRight, 0.625F) ||
         !near(middle.left, 0.5F) || !near(middle.right, 0.5F) ||
         !near(envelope.endLeft + envelope.endRight, 1.0F) ) {
        XERROR("Flick hit effect did not move linearly between track centers");
        return false;
    }
    return true;
}

/// @brief 验证负坐标草稿轨正确映射到草稿区局部轨道和声像。
/// @return 四轨草稿最左与最右轨分别映射到局部 0 和 3 时返回 true。
/// @note 不传独立草稿轨数，覆盖沿用玩家轨数的兼容调用形式。
bool testDraftTrackMappingAndStereo()
{
    // 两个负轨号是区域边缘有效值，不作为非法轨道钳位的测试输入。
    using HitFXSystem  = MMM::Logic::System::HitFXSystem;
    auto leftDraft     = makeEvent(MMM::NoteType::NOTE, -4);
    auto rightDraft    = makeEvent(MMM::NoteType::NOTE, -1);
    leftDraft.isDraft  = true;
    rightDraft.isDraft = true;

    // 负轨号与草稿标志同时提供；仅凭负值测试会漏掉区域类型参与映射的约束。
    const auto leftEnvelope =
        HitFXSystem::stereoGainEnvelopeForEvent(leftDraft, 4, true);
    const auto rightEnvelope =
        HitFXSystem::stereoGainEnvelopeForEvent(rightDraft, 4, true);
    // 四轨区域的边缘轨中心仍距边界半轨，因此不是完全静音另一声道的极端位置。
    // -4 对应 0.125，-1 对应 0.875；局部索引和声道值分别断言。
    if ( HitFXSystem::areaTrackIndexForEvent(leftDraft, 4) != 0 ||
         HitFXSystem::areaTrackIndexForEvent(rightDraft, 4) != 3 ||
         !near(leftEnvelope.startLeft, 0.875F) ||
         !near(leftEnvelope.startRight, 0.125F) ||
         !near(rightEnvelope.startLeft, 0.125F) ||
         !near(rightEnvelope.startRight, 0.875F) ) {
        XERROR("Draft hit event did not use draft-local lane mapping");
        return false;
    }
    return true;
}

/// @brief 验证草稿轨数量独立增长后仍使用草稿区自身宽度定位声像。
/// @return 六轨草稿最左与最右轨分别映射到局部 0 和 5 时返回 true。
/// @note 玩家仍是四轨，只增加草稿轨数，专门区分两个区域的宽度来源。
bool testDynamicDraftTrackMappingAndStereo()
{
    // 最右草稿仍使用 -1，新增轨道向左扩展，不把原有地址整体改成另一套编号。
    using HitFXSystem  = MMM::Logic::System::HitFXSystem;
    auto leftDraft     = makeEvent(MMM::NoteType::NOTE, -6);
    auto rightDraft    = makeEvent(MMM::NoteType::NOTE, -1);
    leftDraft.isDraft  = true;
    rightDraft.isDraft = true;

    const auto leftEnvelope =
        HitFXSystem::stereoGainEnvelopeForEvent(leftDraft, 4, true, 6);
    // 显式传入 6，不能让夹具自动把玩家轨数也改成 6 而掩盖错误的分母来源。
    const auto rightEnvelope =
        HitFXSystem::stereoGainEnvelopeForEvent(rightDraft, 4, true, 6);
    // 六轨边缘中心距边界 1/12，对应外侧声道为 11/12。
    // 这里只断言两侧主声道与局部索引，不宣称逐项检查全部包络端点。
    if ( HitFXSystem::areaTrackIndexForEvent(leftDraft, 4, 6) != 0 ||
         HitFXSystem::areaTrackIndexForEvent(rightDraft, 4, 6) != 5 ||
         !near(leftEnvelope.startLeft, 11.0F / 12.0F) ||
         !near(rightEnvelope.startRight, 11.0F / 12.0F) ) {
        XERROR("Dynamic draft hit event used player lane width for stereo");
        return false;
    }
    return true;
}

/// @brief 验证画面两侧轨道与实际左右声道方向一致。
/// @return 最左轨左声道更响且最右轨右声道更响时返回 true。
/// @note 本场景只比较方向关系，精确增益由前面的固定位置用例覆盖。
bool testTrackSidesMatchChannels()
{
    // 验证事件数据中的左右方向，不包含操作系统声道交换或硬件接线验收。
    const auto leftTrack =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::NOTE, 0), 4, true);
    const auto rightTrack =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::NOTE, 3), 4, true);
    // 两边都检查，避免错误实现把所有轨道统一偏到同一侧却通过单边断言。
    if ( leftTrack.startLeft <= leftTrack.startRight ||
         rightTrack.startRight <= rightTrack.startLeft ) {
        XERROR("Hit effect stereo channels were mirrored across the canvas");
        return false;
    }
    return true;
}

/// @brief 验证关闭功能时保留未经衰减的原始立体声音效。
/// @return 首尾左右声道增益均为 1 时返回 true。
/// @note 关闭的是按轨道分配声像，不是把原始双声道折成居中的单声道。
bool testDisabledKeepsOriginalStereo()
{
    // 单位增益保持输入各声道幅度，不能回退成启用态的两侧各 0.5。
    const auto envelope =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::FLICK, 1, 1), 4, false);
    // 选择本会发生移动的 Flick 来关闭功能，确保整个包络都退回单位增益。
    // 这里左右和应为 2，不能沿用启用声像时的左右和为 1 的断言。
    if ( !near(envelope.startLeft, 1.0F) || !near(envelope.startRight, 1.0F) ||
         !near(envelope.endLeft, 1.0F) || !near(envelope.endRight, 1.0F) ) {
        XERROR("Disabled stereo hit effects changed the original channel gain");
        return false;
    }
    return true;
}

/// @brief 验证绑定音效严格优先于内置 Note/Flick 音效。
/// @return 非空绑定返回原资源，空绑定按物件类型返回内置资源时返回 true。
/// @note 检查资源键优先级，不检验资源解码成功或绑定音量的混音结果。
bool testBoundSoundOverridesDefault()
{
    using HitFXSystem = MMM::Logic::System::HitFXSystem;

    auto boundEvent          = makeEvent(MMM::NoteType::FLICK, 1, 1);
    boundEvent.sampleBinding = MMM::AudioSampleBinding{ "sample.wav", 0.35F };
    // 同时给出 Flick 默认类型与显式绑定，绑定必须覆盖默认类型对应的内置键。
    if ( HitFXSystem::soundEffectKeyForEvent(
             boundEvent, MMM::NoteType::FLICK) != "sample.wav" ) {
        XERROR("Bound note sound did not override the built-in Flick sound");
        return false;
    }

    const auto noteEvent  = makeEvent(MMM::NoteType::NOTE, 0);
    const auto flickEvent = makeEvent(MMM::NoteType::FLICK, 0, 1);
    // 默认资源名是后续皮肤查找的键，不是本测试要求存在的文件系统路径。
    // 缺省分支分别覆盖 Note 与 Flick，不能只证明绑定优先而忽略默认资源选择。
    if ( HitFXSystem::soundEffectKeyForEvent(noteEvent, MMM::NoteType::NOTE) !=
             "hiteffect.note" ||
         HitFXSystem::soundEffectKeyForEvent(
             flickEvent, MMM::NoteType::FLICK) != "hiteffect.flick" ) {
        XERROR("Empty bound sound did not select the built-in hit effect");
        return false;
    }
    return true;
}

/// @brief 验证自定义采样的物件音量独立进入打击音效倍率。
/// @return 有绑定时返回物件音量、无绑定时返回 1。
/// @note 只验证事件局部倍率，不乘皮肤总音量、轨道声像或全局输出音量。
bool testBoundSampleVolume()
{
    // 缺省倍率 1.0 表示不额外改变默认音效增益，不是系统最终输出为满音量。
    using HitFXSystem = MMM::Logic::System::HitFXSystem;

    auto boundEvent          = makeEvent(MMM::NoteType::NOTE, 1);
    boundEvent.sampleBinding = MMM::AudioSampleBinding{ "sample.wav", 0.35F };
    // 用非单位音量区分真实读取绑定字段与始终返回默认值的实现。
    if ( !near(HitFXSystem::sampleVolumeForEvent(boundEvent), 0.35F) ||
         !near(HitFXSystem::sampleVolumeForEvent(
                   makeEvent(MMM::NoteType::NOTE, 1)),
               1.0F) ) {
        XERROR("Bound sample volume was not applied independently");
        return false;
    }
    return true;
}

/// @brief 验证仅非空资源绑定会进入已绑定打击音效分组。
/// @return 无绑定和空绑定为未绑定，非空绑定为已绑定时返回 true。
/// @note optional 存在性不等于有效资源绑定，资源标识为空须进入未绑定分组。
bool testBoundSoundClassification()
{
    // 分组不查磁盘；资源键非空但实际缺失的情况由后续加载流程处理。
    using HitFXSystem = MMM::Logic::System::HitFXSystem;

    auto emptyBindingEvent          = makeEvent(MMM::NoteType::NOTE, 0);
    emptyBindingEvent.sampleBinding = MMM::AudioSampleBinding{ "", 0.5F };
    // 空绑定仍有非零音量，分组不能拿音量或 optional 是否存在代替资源键判断。
    auto boundEvent          = makeEvent(MMM::NoteType::NOTE, 0);
    boundEvent.sampleBinding = MMM::AudioSampleBinding{ "sample.wav", 0.5F };
    // 三种状态在同一断言中比较，不需要真正创建 sample.wav 来确定事件分组。
    if ( HitFXSystem::hasBoundSoundEffect(makeEvent(MMM::NoteType::NOTE, 0)) ||
         HitFXSystem::hasBoundSoundEffect(emptyBindingEvent) ||
         !HitFXSystem::hasBoundSoundEffect(boundEvent) ) {
        XERROR("Hit sound binding groups were classified incorrectly");
        return false;
    }
    return true;
}

/// @brief 验证旧皮肤的固定模式仍在判定线中心按原尺寸绘制。
/// @return 固定矩形的中心、宽高与旧算法一致时返回 true。
/// @note 检查 CPU 矩形计算，不生成纹理图元或验证皮肤图片透明边缘。
bool testFixedHitEffectBounds()
{
    // 目标轨位于有效区域内部，本场景不验证越界目标轨的钳位结果。
    // 判定线与轨道底部故意不同，可区分固定中心定位和误用整轨底部的实现。
    using HitFXSystem = MMM::Logic::System::HitFXSystem;
    // 四轨区域左端 100、单轨宽 50，起始轨 1 加偏移 1 落在索引 2。
    // 目标中心为 225，固定宽 40 的左边界应为 205。
    const auto bounds = HitFXSystem::calculateRenderBounds(
        MMM::Config::HitEffectLayoutMode::Fixed,
        4,
        1,
        1,
        300.0F,
        100.0F,
        20.0F,
        500.0F,
        50.0F,
        40.0F,
        20.0F);
    // 矩形 y 是底边而非上边；判定线 300 加半高 10 得到 310。
    // 固定模式不拉伸到轨道上下边界，宽高仍是传入的 40 与 20。
    if ( !near(bounds.x, 205.0F) || !near(bounds.y, 310.0F) ||
         !near(bounds.width, 40.0F) || !near(bounds.height, 20.0F) ) {
        XERROR("Fixed hit effect bounds no longer match legacy placement");
        return false;
    }
    return true;
}

/// @brief 验证草稿事件不会进入玩家轨道 KPS 统计。
/// @return 草稿 Flick 即使偏移落入非负轨道也不增加玩家 KPS 时返回 true。
/// @note 用全新系统隔离历史计数，只验证本次事件不能贡献玩家区域数据。
bool testDraftEventsDoNotAffectPlayerKps()
{
    using HitFXSystem  = MMM::Logic::System::HitFXSystem;
    auto draftFlick    = makeEvent(MMM::NoteType::FLICK, -1, 1);
    draftFlick.isDraft = true;

    // -1 加横向偏移 1 恰好得到 0，能暴露只按最终非负轨号判断玩家事件的错误。
    MMM::Config::EditorConfig config;
    config.visual.canvasComponents.kps.visible = true;
    // 开启 KPS 更新，否则未执行统计的全零结果也会误通过草稿排除测试。
    HitFXSystem system;
    system.update(0.0, { draftFlick }, 4, config);
    // 事件与更新时间同为零，草稿排除不能由“事件太旧”偶然造成。
    const auto kps = system.trackKps();
    // 先要求玩家四轨计数容器存在，再检查全部为零，不能接受未初始化的空容器。
    if ( kps.size() != 4U ||
         std::any_of(kps.begin(), kps.end(), [](std::uint32_t count) {
             return count != 0U;
         }) ) {
        XERROR("Draft hit event leaked into player KPS statistics");
        // 任意玩家轨出现非零计数都失败，不只检查目标零号轨。
        return false;
    }
    return true;
}

/// @brief 验证整轨模式覆盖 Flick 目标轨道的完整可见区域。
/// @return 目标轨道宽度和上下边界均精确匹配时返回 true。
/// @note 参数与固定模式用例相同，只改变布局模式，以区分两种尺寸策略。
bool testTrackFillHitEffectBounds()
{
    // 非零 leftX/topY 验证偏移不能丢失，原点为零的夹具无法暴露这类错误。
    // 整轨宽度是单轨 50，不是起始轨到目标轨的双轨跨度。
    using HitFXSystem = MMM::Logic::System::HitFXSystem;
    // 目标仍为索引 2，整轨模式从 100 + 2 * 50 = 200 开始，不按固定宽居中。
    const auto bounds = HitFXSystem::calculateRenderBounds(
        MMM::Config::HitEffectLayoutMode::TrackFill,
        4,
        1,
        1,
        300.0F,
        100.0F,
        20.0F,
        500.0F,
        50.0F,
        40.0F,
        20.0F);
    // 顶部 20、底部 500 对应高度 480；原固定尺寸 40×20 不应影响整轨结果。
    // 同时检查 x、y、宽、高，防止只算对面积却放在错误位置。
    if ( !near(bounds.x, 200.0F) || !near(bounds.y, 500.0F) ||
         !near(bounds.width, 50.0F) || !near(bounds.height, 480.0F) ) {
        XERROR("Track-fill hit effect did not cover the destination track");
        return false;
    }
    return true;
}

/// @brief 验证非 Hold 特效按视觉时长结束并循环序列帧。
/// @return 时长边界正确且超过一轮后回到对应帧时返回 true。
/// @note 视觉结束时长与序列帧索引分开验证，不把序列播一轮等同于特效必须结束。
bool testNonHoldHitEffectPlayback()
{
    // elapsed 直接传入计算入口，不通过 sleep 制造时长边界。
    using HitFXSystem = MMM::Logic::System::HitFXSystem;
    // 0.119 仍在区间内，0.12 命中结束边界；两侧一起检查包含关系。
    if ( HitFXSystem::isNonHoldEffectFinished(0.119, 0.12F) ||
         !HitFXSystem::isNonHoldEffectFinished(0.12, 0.12F) ) {
        XERROR("Non-Hold hit effect duration boundary was not respected");
        return false;
    }

    const auto firstLoopFrame =
        HitFXSystem::loopingEffectFrameIndex(0.125, 60.0F, 6U);
    // 0.125 秒对应 7.5 帧，向下取帧后按六帧循环得到索引 1。
    const auto invalidFrame =
        HitFXSystem::loopingEffectFrameIndex(0.125, 0.0F, 6U);
    // 两次查询都使用非空六帧序列，只改变帧率，不把空序列失败混入帧率场景。
    // 无效帧率必须返回空值，不把默认第零帧误当成可播放的有效帧。
    if ( !firstLoopFrame || *firstLoopFrame != 1U || invalidFrame ) {
        // 左侧短路保证先检查 optional 存在，断言自身不能解引用失败结果。
        XERROR("Non-Hold hit effect frames did not loop safely");
        return false;
    }
    return true;
}

/// @brief 验证从持续区间中段播放时补建普通 Hold 与 Polyline subHold 特效。
/// @return 两类有效 Hold 均补建且已结束或未开始的事件被忽略时返回 true。
/// @note 断言只观察恢复数量，不核对每个活跃特效的纹理或剩余寿命。
/// @pre 默认配置启用打击特效，否则恢复入口会按开关直接返回零。
bool testRestoreActiveHoldEffectsFromMiddle()
{
    // 返回值计的是满足恢复条件的事件次数，不等同于活跃特效容器最终大小。
    using HitFXSystem = MMM::Logic::System::HitFXSystem;
    using HitEvent    = HitFXSystem::HitEvent;

    const std::vector<HitEvent> events{
        // 时间递增是输入前提，恢复入口可在遇到未来事件时停止扫描。
        // 已结束的短长条：0.5 到 0.75，不覆盖查询时间 3.0。
        { 0.5,
          MMM::NoteType::HOLD,
          HitEvent::Role::None,
          1,
          3,
          0,
          0.25,
          false },
        // 普通长条覆盖 1.0 到 5.0，查询落在其中，应补建一个特效。
        { 1.0, MMM::NoteType::HOLD, HitEvent::Role::None, 1, 0, 0, 4.0, false },
        // 折线内部长条覆盖 2.0 到 4.0，子节点身份不能令它被误过滤。
        { 2.0,
          MMM::NoteType::HOLD,
          HitEvent::Role::Internal,
          1,
          1,
          0,
          2.0,
          true },
        // 已经过判定点的普通点击不需要在 seek 后重新补播视觉特效。
        { 2.5, MMM::NoteType::NOTE, HitEvent::Role::None, 1, 2, 0, 0.0, false },
        // 未来长条尚未开始，按时间排序的输入允许恢复扫描在此停止。
        { 4.0, MMM::NoteType::HOLD, HitEvent::Role::None, 1, 2, 0, 1.0, false },
    };
    MMM::Config::EditorConfig config;
    HitFXSystem               system;
    // 过滤场景包含已结束、未来与非长条，不能只放两个应成功对象验证正向路径。
    // 两个应恢复的长条分处不同轨道，不让同轨覆盖规则干扰恢复数量的场景解释。
    const std::size_t restoredCount =
        system.restoreActiveHoldEffects(3.0, events, config);
    // 查询严格位于两条长条内部，不用恰好结束的容差行为替代中段恢复。
    if ( restoredCount != 2U ) {
        XERROR("Playback middle did not restore Hold and subHold effects");
        return false;
    }
    return true;
}

/// @brief 验证同图集在透明覆盖、加法、透明覆盖之间切换时保持命令边界。
/// @return 三个连续批次保留各自混合状态且重复设置不会产生空批次。
/// @note 使用真实 Batcher 提交几何，不模拟渲染命令或依赖 GPU。
bool testEffectBlendBatchBoundaries()
{
    // 快照在批处理器之后析构，flush 写入期间目标容器始终有效。
    // 此用例只检查 CPU 命令，不以像素颜色判断混合状态。
    MMM::Logic::RenderSnapshot  snapshot;
    MMM::Logic::System::Batcher batcher(&snapshot);
    using TextureID = MMM::Logic::TextureID;
    // 两个纹理共享图集，原纹理切批规则会合并它们，必须由混合状态切分。
    snapshot.uvMap[static_cast<uint32_t>(TextureID::Note)] = {
        0, 0, 0.1F, 0.1F
    };
    snapshot.uvMap[1000] = { 0.2F, 0, 0.1F, 0.1F };
    batcher.setTexture(TextureID::Note);
    batcher.pushQuad(0, 10, 10, 10, { 1, 1, 1, 1 });
    // 从普通音符切到特效时，即便底层图集相同也必须结束覆盖批次。
    // 半透明顶点保留在几何中，混合开关属于命令而非顶点属性。
    batcher.setAdditiveBlend(true);
    batcher.setTexture(static_cast<TextureID>(1000));
    batcher.pushQuad(10, 10, 10, 10, { 1, 1, 1, 0.5F });
    // 同状态不应切分加法批次；恢复覆盖后同纹理仍需生成新命令。
    batcher.setAdditiveBlend(true);
    batcher.pushQuad(20, 10, 10, 10, { 1, 1, 1, 0.5F });
    // 从加法切回覆盖不能复用上一命令，否则后续普通物件也会叠亮。
    batcher.setAdditiveBlend(false);
    batcher.pushQuad(30, 10, 10, 10, { 1, 1, 1, 1 });
    // 最后一个覆盖批次尚未遇到下一次状态切换，必须显式提交尾批。
    // 一个四边形产生六个索引，因此中间两个四边形合并后应有十二个。
    // 偏移检查同时约束几何顺序，防止正确的数量掩盖跨批次重排。
    // 先确认三条命令存在，再访问各命令，避免失败检查自身发生越界。
    batcher.flush();
    if ( snapshot.cmds.size() != 3 || snapshot.cmds[0].additiveBlend ||
         !snapshot.cmds[1].additiveBlend || snapshot.cmds[2].additiveBlend ||
         snapshot.cmds[0].indexCount != 6 ||
         snapshot.cmds[1].indexCount != 12 ||
         snapshot.cmds[2].indexCount != 6 ||
         snapshot.cmds[1].indexOffset != 6 ||
         snapshot.cmds[2].indexOffset != 18 ) {
        XERROR("Effect blend modes merged across atlas batches");
        return false;
    }
    return true;
}

/// @brief 验证普通长条与折线 Hold 始终取独立循环帧，尾部清理及跳转恢复一致。
/// @param holdKey 新皮肤使用独立键，旧皮肤用单键键验证兼容回退。
/// @note UV 只注册当前期望帧；选错序列或取模错误都会导致缺少几何。
/// 直接驱动触发、寿命更新和快照链路，不以纯辅助函数的返回值替代实际行为。
/// 本用例的 Hold 持续两秒，超过所有测试皮肤的一轮动画与普通命中特效寿命。
/// 同一组时刻用于普通 Hold 和折线内部段，确保两者没有不同的序列选择规则。
/// 皮肤在函数执行期间保持不变，序列指针不能跨下一次加载继续借用。
/// UV 哨兵同时覆盖帧起始 ID 与相对帧号，不只是检查特效容器有成员。
bool verifyHoldVisualPlayback(const std::string& holdKey)
{
    using System         = MMM::Logic::System::HitFXSystem;
    using Strategy       = MMM::Config::PolylineSfxStrategy;
    auto&       manager  = MMM::Config::SkinManager::instance();
    const auto* sequence = manager.getEffectSequence(holdKey);
    // 无序列直接失败，避免空资源测试因为“没有绘制”而虚假通过。
    if ( !sequence || sequence->frames.empty() ) return false;
    // 折线的声音简化策略不应改变长按视觉；四种策略都必须覆盖。
    for ( const auto strategy : { Strategy::Exact,
                                  Strategy::InternalAsNormal,
                                  Strategy::OnlyTailExact,
                                  Strategy::AllAsNormal } ) {
        for ( const bool subNote : { false, true } ) {
            // 每种策略和物件身份使用全新系统，不能继承前一场景的活跃事件。
            // config 只改变键音策略；视觉开关显式打开，不读取个人设置。
            MMM::Config::EditorConfig config;
            config.settings.sfxConfig.polylineStrategy = strategy;
            config.visual.enableHitEffects             = true;
            auto event      = makeEvent(MMM::NoteType::HOLD, 0);
            event.duration  = 2.0;
            event.isSubNote = subNote;
            // Internal 是简化策略会改成单键的关键角色，最容易误选旧动画。
            // 普通长条则保留 None，不让测试事件本身混入折线角色语义。
            event.role = subNote ? System::HitEvent::Role::Internal
                                 : System::HitEvent::Role::None;
            System system;
            system.update(0.0, { event }, 4, config);
            // 时间跨越多轮动画和普通特效寿命，验证长按不会被单键寿命裁掉。
            // 取非整周期时刻，避免错误地总取第一帧也能通过。
            for ( const double time : { 0.0, 0.041, 0.317, 1.073 } ) {
                // 后续 update 不重发命中事件，否则会掩盖持续实例提前消失。
                // 每帧使用新快照，不能让上一帧的顶点冒充本帧输出。
                system.update(time, {}, 4, config);
                MMM::Logic::RenderSnapshot  snapshot;
                MMM::Logic::System::Batcher batcher(&snapshot);
                const auto frame = System::loopingEffectFrameIndex(
                    time, manager.getEffectBaseFps(), sequence->frames.size());
                if ( !frame ) return false;
                // 只有正确帧拥有 UV；错误选到 note/flick 时无法提交矩形。
                // 图集比例为一仅固定几何尺寸，不参与动画帧选择算法。
                snapshot.uvMap[sequence->startId +
                               static_cast<uint32_t>(*frame)] = { 0, 0, 1, 1 };
                system.generateSnapshot(
                    batcher, time, config, 4, 300, 0, 0, 600, 100);
                // 两种布局都应输出一个完整矩形；RM 的 Hold
                // 同时必须保持加法混合。
                // Batcher 用四顶点与索引表达一个特效，不应叠画单键加 Hold。
                if ( snapshot.vertices.size() != 4U ||
                     snapshot.cmds.size() != 1U ||
                     snapshot.cmds.front().additiveBlend !=
                         sequence->additiveBlend ) {
                    XERROR("Hold did not render its expected looping frame: {}",
                           holdKey);
                    return false;
                }
            }
            // 恢复中段使用原始事件起点作为相位，不允许从单键帧或首帧重新闪起。
            system.clearActiveEffects();
            // 恢复不重发声音，仍通过同一个视觉触发入口选择 Hold 序列。
            const std::vector<System::HitEvent> events{ event };
            if ( system.restoreActiveHoldEffects(1.0, events, config) != 1U )
                return false;
            MMM::Logic::RenderSnapshot  snapshot;
            MMM::Logic::System::Batcher batcher(&snapshot);
            const auto                  frame = System::loopingEffectFrameIndex(
                1.0, manager.getEffectBaseFps(), sequence->frames.size());
            // 前面的多时刻检查已验证帧率与帧数有效，恢复仍按原事件相位计算。
            snapshot.uvMap[sequence->startId +
                           static_cast<uint32_t>(*frame)] = { 0, 0, 1, 1 };
            system.generateSnapshot(
                batcher, 1.0, config, 4, 300, 0, 0, 600, 100);
            if ( snapshot.vertices.empty() ) return false;
            // 清理检查提供整组有效 UV，避免三秒时帧号变化造成缺纹理假阳性。
            // 这里刻意放宽资源哨兵，让唯一的无几何原因只能是活动实例已结束。
            snapshot.vertices.clear();
            snapshot.cmds.clear();
            for ( std::size_t index = 0; index < sequence->frames.size();
                  ++index )
                snapshot.uvMap[sequence->startId +
                               static_cast<uint32_t>(index)] = { 0, 0, 1, 1 };
            // 只注册当前 Hold 序列；不同皮肤中的帧号和起始 ID 不共享。
            system.update(3.0, {}, 4, config);
            // 检查时刻既超过物件尾部也超过最短动画周期，两种寿命条件均成立。
            system.generateSnapshot(
                batcher, 3.0, config, 4, 300, 0, 0, 600, 100);
            if ( !snapshot.vertices.empty() ) return false;
        }
    }
    return true;
}

/// @brief 加载四套真实皮肤及缺少 Hold 键的旧皮肤，验证序列选择和回退。
/// @note 只解析配置与构造 CPU 快照；这里不上传纹理，也不声称 GPU 像素验收。
/// @param outputRoot 专用构建输出目录，所有夹具都写在此处。
/// @param skinsRoot 随仓库分发的四套皮肤入口父目录。
/// @param translationsRoot 公共翻译资源，不借用用户配置中的本地化文件。
/// @return 任一真实皮肤无法绘制或旧皮肤无法回退时返回 false。
/// @warning 仅测试初始化路径执行文件访问，不进入正式应用的渲染循环。
bool testHoldVisualSkins(const std::filesystem::path& skinsRoot,
                         const std::filesystem::path& translationsRoot,
                         const std::filesystem::path& outputRoot)
{
    auto& manager = MMM::Config::SkinManager::instance();
    for ( const auto* name : { "mmm-default", "ivm", "rm", "rm-old" } ) {
        // 每次重新加载都重新查询序列，避免排序分配后的帧 ID 跨皮肤混用。
        // 完整资源路径、截取范围及混合声明另由 SkinThemeBindingTest 检查。
        if ( !manager.loadSkin(
                 MMM::Config::pathToUtf8(skinsRoot / name / "skin.lua"),
                 translationsRoot) ||
             !verifyHoldVisualPlayback("note.effect.hold") )
            return false;
    }
    // 模拟旧包只声明单键序列，不能为了测试回退而修改真实皮肤。
    std::error_code error;
    std::filesystem::create_directories(outputRoot, error);
    // 文件系统失败属于测试环境错误，不忽略后继续加载上次运行的夹具。
    if ( error ) return false;
    const auto path = outputRoot / "legacy-skin.lua";
    {
        // 固定夹具文件只覆盖本测试的构建输出；不递归删除任何资源目录。
        std::ofstream file(path);
        file << "return { meta={effectbasefps=60}, "
                "assets={note={effect={note='note/[1 .. 3].png'}}} }";
        // 加载器只展开路径；CPU 快照提供合成 UV，不要求生成测试 PNG。
        file.close();
        // 显式关闭后再加载，以便把写入或关闭失败作为测试失败而非解析噪声。
        if ( !file ) return false;
    }
    return manager.loadSkin(MMM::Config::pathToUtf8(path), translationsRoot) &&
           // 未声明 Hold 时，实际渲染必须消费单键帧的 ID，而不是静默消失。
           verifyHoldVisualPlayback("note.effect.note");
}
}  // namespace

/// @brief 运行 HitEffect 立体声定位测试。
/// @return 全部测试通过时返回 0。
/// @param argc 应为程序名加三个独立路径参数。
/// @param argv 内置皮肤根、翻译根和专用测试输出目录。
/// @note 同时覆盖绑定分类、视觉边界和中途恢复；名称不代表只运行声道计算。
/// @note 资源与夹具根由 CTest 显式传入，不读取用户谱面或产生音频文件。
int main(int argc, char* argv[])
{
    // CTest 提供全部路径，缺参时明确失败而非按当前工作目录猜测资源位置。
    if ( argc != 4 ) return 1;
    // 各有状态场景自行创建系统，不复用前一场景的 KPS 或活跃特效。
    // 纯计算用例先执行，状态恢复用例最后执行，顺序不表示状态应跨场景传递。
    // 短路执行；只有退出零才能证明全部场景都已执行且通过。
    // 非零退出由首个失败场景的日志定位，不把尚未运行的后续场景算作成功。
    return testEffectBlendBatchBoundaries() && testStaticTrackPosition() &&
                   testFlickMovesAcrossChannels() &&
                   testDraftTrackMappingAndStereo() &&
                   testDynamicDraftTrackMappingAndStereo() &&
                   testTrackSidesMatchChannels() &&
                   testDisabledKeepsOriginalStereo() &&
                   testBoundSoundOverridesDefault() &&
                   testBoundSampleVolume() && testBoundSoundClassification() &&
                   testFixedHitEffectBounds() &&
                   testDraftEventsDoNotAffectPlayerKps() &&
                   testTrackFillHitEffectBounds() &&
                   testNonHoldHitEffectPlayback() &&
                   testRestoreActiveHoldEffectsFromMiddle() &&
                   testHoldVisualSkins(MMM::Config::utf8ToPath(argv[1]),
                                       MMM::Config::utf8ToPath(argv[2]),
                                       MMM::Config::utf8ToPath(argv[3]))
               ? 0
               : 1;
}
