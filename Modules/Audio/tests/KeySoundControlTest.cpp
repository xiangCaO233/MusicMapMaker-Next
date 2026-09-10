#include "audio/KeySoundControl.h"

#include "log/colorful-log.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>

namespace
{

// 本测试覆盖固定容量 KeySound 控制库的值语义与并发协议：
//
// - 所有区域、轨道和类别默认未静音且使用单位增益；
// - 静音与增益占同一原子字的独立位，单字段更新不能覆盖另一字段；
// - 增益限制在 [0, 2]，NaN 与无穷不会进入实时乘法；
// - 玩家、草稿与 BGM 的逐轨数组彼此隔离；
// - Bound 与 Unbound 类别控制彼此隔离但可跨玩家/草稿区域复用；
// - 普通 SFX 关闭 KeySound 控制后始终取得单位增益；
// - 越界轨道写入无效、读取为单位控制；
// - 完整 store 并发读取不撕裂，两个单字段 CAS 写入也不会互相丢失。
//
// 每个测试函数只创建栈上控制库，不启动音频设备。除并发场景外，断言都在
// setter 返回后同步读取；并发场景使用固定迭代次数而不是固定时长等待。测试
// 不访问内部 packed 位布局，所有结果都通过公开控制 API 与有效增益入口观察。
// 浮点结果只在量化误差内比较，静音结果则要求严格为零。
// 轨道索引覆盖正常非零值与最大无效值，不依赖特定 KEY_SOUND_TRACK_LIMIT 数字。
// 玩家、草稿、BGM 和类别四种存储层都至少覆盖一次写入与读取。
// 完整快照并发与单字段并发分开测试，便于定位 store 或 CAS 协议回归。
// 测试只验证控制数据，不把 SoundEffectPool 的 voice 生命周期混入本文件。

/// @brief 定点量化后浮点比较允许的最大误差。
///
/// 该值略大于单个量化步长，足以容纳 pack/unpack 舍入但不能掩盖控制组合错误。
constexpr float CONTROL_EPSILON = 4.0e-5F;

/// @brief 判断量化后的 Key 音增益是否符合预期。
/// @param actual 控制库解码得到的实际增益。
/// @param expected 未量化的期望线性增益。
/// @return 差值不超过一个量化误差余量时返回 true。
bool nearlyEqual(float actual, float expected) noexcept
{
    return std::abs(actual - expected) <= CONTROL_EPSILON;
}

/// @brief 验证所有控制项默认全开且使用单位增益。
/// @return 各区域、轨道、类别及有效增益均为默认值时返回 true。
///
/// 选取非零轨道索引，避免测试只覆盖数组首元素初始化。玩家有效控制与 BGM
/// 有效控制同时验证，确保默认 packed 值不仅 getter 正确，也能参与组合计算。
///
/// 默认检查包含：
///
/// - 玩家区域与第 12 轨未静音；
/// - 草稿区域与第 12 轨未静音；
/// - BGM 区域与第 7 轨未静音；
/// - Bound 与 Unbound 两个类别均未静音；
/// - 上述所有可调增益均解码为单位值；
/// - 玩家三级组合与 BGM 两级组合最终仍为单位值。
bool testDefaults()
{
    MMM::Audio::KeySoundControlBank controls;
    const auto player = MMM::Audio::KeySoundPlaybackControl{
        .enabled          = true,
        .playerTrackIndex = 12U,
        .effectGroup      = MMM::Audio::KeySoundEffectGroup::Unbound,
    };

    // 一次性覆盖三类区域、两类音效组及两个有效增益入口。
    const bool valid =
        !controls.isPlayerAreaMuted() && !controls.isPlayerTrackMuted(12U) &&
        nearlyEqual(controls.getPlayerTrackGain(12U), 1.0F) &&
        !controls.isDraftAreaMuted() && !controls.isDraftTrackMuted(12U) &&
        nearlyEqual(controls.getDraftTrackGain(12U), 1.0F) &&
        !controls.isBgmAreaMuted() &&
        nearlyEqual(controls.getBgmAreaGain(), 1.0F) &&
        !controls.isBgmTrackMuted(7U) &&
        nearlyEqual(controls.getBgmTrackGain(7U), 1.0F) &&
        !controls.isEffectGroupMuted(
            MMM::Audio::KeySoundEffectGroup::Unbound) &&
        !controls.isEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound) &&
        nearlyEqual(controls.getEffectGroupGain(
                        MMM::Audio::KeySoundEffectGroup::Unbound),
                    1.0F) &&
        nearlyEqual(
            controls.getEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound),
            1.0F) &&
        nearlyEqual(controls.effectivePlayerGain(player), 1.0F) &&
        nearlyEqual(controls.effectiveBgmTrackGain(7U), 1.0F);
    if ( !valid ) XERROR("Key sound control defaults are not unity");
    return valid;
}

/// @brief 验证静音修改保留增益，增益输入被限制到 0~2。
/// @return 位字段保留、上下界与非有限值处理均正确时返回 true。
///
/// 先写 1.25，再开关静音并复查增益，直接验证 setMuted CAS 保留低 16 位。
/// 随后依次覆盖负值、超上限、NaN、正无穷和其他类别负值的规范化路径。
///
/// 1.25 可由当前定点比例精确表示，因此保留测试不受量化误差影响。2.0 位于
/// 公开上边界，内部会饱和到 16 位最大值，nearlyEqual 容纳其一个量化步误差。
/// 非有限值统一归零，确保后续有效增益计算不会产生 NaN 并污染整个音频 block。
/// BGM 区域与 Bound 类别额外覆盖同一规范化 helper 被不同控制槽复用的行为。
bool testMutePreservesGainAndClamping()
{
    MMM::Audio::KeySoundControlBank controls;
    controls.setPlayerTrackGain(3U, 1.25F);
    controls.setPlayerTrackMuted(3U, true);
    controls.setPlayerTrackMuted(3U, false);
    if ( !nearlyEqual(controls.getPlayerTrackGain(3U), 1.25F) ) {
        XERROR("Player track mute overwrote its gain");
        return false;
    }

    // 三个异常范围分别应回退到零、上边界二和零。
    controls.setPlayerTrackGain(3U, -1.0F);
    if ( !nearlyEqual(controls.getPlayerTrackGain(3U), 0.0F) ) return false;
    controls.setPlayerTrackGain(3U, 9.0F);
    if ( !nearlyEqual(controls.getPlayerTrackGain(3U), 2.0F) ) return false;
    controls.setPlayerTrackGain(3U, std::numeric_limits<float>::quiet_NaN());
    if ( !nearlyEqual(controls.getPlayerTrackGain(3U), 0.0F) ) return false;

    controls.setBgmAreaGain(std::numeric_limits<float>::infinity());
    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound, -3.0F);
    const bool valid = nearlyEqual(controls.getBgmAreaGain(), 0.0F) &&
                       nearlyEqual(controls.getEffectGroupGain(
                                       MMM::Audio::KeySoundEffectGroup::Bound),
                                   0.0F);
    if ( !valid ) XERROR("Key sound gains were not clamped safely");
    return valid;
}

/// @brief 验证玩家轨道、类别和 BGM 区域使用正确的乘法组合。
/// @return 各级增益乘积与任一级静音短路均正确时返回 true。
///
/// 玩家轨 0.5 与 Bound 类别 1.5 组合为 0.75，之后逐项开启轨道、类别、区域
/// 静音，任何一级都应得到严格零。BGM 再独立验证区域与逐轨的两级组合。
///
/// 各静音分支都在断言后恢复，确保下一分支只验证当前层级。普通 SFX 检查放在
/// 玩家区域仍静音时执行，能证明 enabled=false 确实在读取任何控制前直接旁路。
/// BGM 区域使用 0.5、轨道使用 1.5，结果同样为 0.75，便于对照三级玩家路径。
/// 最后区域静音断言在逐轨静音已恢复后执行，明确验证 BGM 总控的最高优先级。
bool testGainComposition()
{
    MMM::Audio::KeySoundControlBank controls;
    const auto playback = MMM::Audio::KeySoundPlaybackControl{
        .enabled          = true,
        .playerTrackIndex = 4U,
        .effectGroup      = MMM::Audio::KeySoundEffectGroup::Bound,
    };

    controls.setPlayerTrackGain(4U, 0.5F);
    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound, 1.5F);
    if ( !nearlyEqual(controls.effectivePlayerGain(playback), 0.75F) ) {
        XERROR("Player track and effect group gains were not composed");
        return false;
    }

    controls.setPlayerTrackMuted(4U, true);
    if ( controls.effectivePlayerGain(playback) != 0.0F ) return false;
    controls.setPlayerTrackMuted(4U, false);
    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound, true);
    if ( controls.effectivePlayerGain(playback) != 0.0F ) return false;
    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound, false);
    controls.setPlayerAreaMuted(true);
    if ( controls.effectivePlayerGain(playback) != 0.0F ) return false;

    // enabled=false 的普通音效必须旁路仍处于静音的玩家区域。
    const auto ordinarySfx = MMM::Audio::KeySoundPlaybackControl{};
    if ( !nearlyEqual(controls.effectivePlayerGain(ordinarySfx), 1.0F) ) {
        XERROR("Ordinary SFX unexpectedly inherited Key sound controls");
        return false;
    }

    controls.setBgmAreaGain(0.5F);
    controls.setBgmTrackGain(2U, 1.5F);
    if ( !nearlyEqual(controls.effectiveBgmTrackGain(2U), 0.75F) ) {
        XERROR("BGM area and track gains were not composed");
        return false;
    }
    controls.setBgmTrackMuted(2U, true);
    if ( controls.effectiveBgmTrackGain(2U) != 0.0F ) return false;
    controls.setBgmTrackMuted(2U, false);
    controls.setBgmAreaMuted(true);
    return controls.effectiveBgmTrackGain(2U) == 0.0F;
}

/// @brief 验证草稿区总控和逐轨增益独立于玩家区控制。
/// @return 草稿事件只服从草稿区、对应草稿轨和共享音效类别时返回 true。
///
/// player 与 draft 使用相同轨道和 Bound 类别，仅 area 不同。草稿轨增益首先
/// 只改变 draft；玩家区域静音随后只影响 player；草稿轨和区域静音则反向验证。
///
/// Bound 类别设为 1.5 后两区共享该倍率，因此 player 得到 1.5，draft 还乘
/// 自己的 0.5 得到 0.75。最终同时断言 isDraftAreaMuted，确保有效增益为零并非
/// 来自某个遗留的逐轨静音状态。
bool testDraftAreaIsolation()
{
    MMM::Audio::KeySoundControlBank controls;
    const auto                      draft = MMM::Audio::KeySoundPlaybackControl{
                             .enabled = true,
                             .area    = MMM::Audio::KeySoundPlaybackArea::Draft,
                             .playerTrackIndex = 2U,
                             .effectGroup = MMM::Audio::KeySoundEffectGroup::Bound,
    };
    const auto player = MMM::Audio::KeySoundPlaybackControl{
        .enabled          = true,
        .playerTrackIndex = 2U,
        .effectGroup      = MMM::Audio::KeySoundEffectGroup::Bound,
    };

    // 共享类别增益同时作用两区，而草稿逐轨增益只作用 draft。
    controls.setDraftTrackGain(2U, 0.5F);
    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound, 1.5F);
    if ( !nearlyEqual(controls.effectivePlayerGain(draft), 0.75F) ||
         !nearlyEqual(controls.effectivePlayerGain(player), 1.5F) ) {
        XERROR("Draft track gain leaked into player Key sounds");
        return false;
    }

    controls.setPlayerAreaMuted(true);
    if ( controls.effectivePlayerGain(player) != 0.0F ||
         !nearlyEqual(controls.effectivePlayerGain(draft), 0.75F) ) {
        XERROR("Player area mute leaked into draft Key sounds");
        return false;
    }
    controls.setPlayerAreaMuted(false);
    controls.setDraftTrackMuted(2U, true);
    if ( controls.effectivePlayerGain(draft) != 0.0F ||
         controls.effectivePlayerGain(player) == 0.0F ) {
        XERROR("Draft track mute leaked into player Key sounds");
        return false;
    }
    controls.setDraftTrackMuted(2U, false);
    controls.setDraftAreaMuted(true);
    return controls.effectivePlayerGain(draft) == 0.0F &&
           controls.effectivePlayerGain(player) != 0.0F &&
           controls.isDraftAreaMuted();
}

/// @brief 验证未绑定与已绑定打击音的静音和增益互不影响。
/// @return 两个固定类别槽互不泄漏时返回 true。
///
/// 第一阶段只静音 Unbound 并确认 Bound 仍为单位增益；第二阶段恢复 Unbound、
/// 设置 Bound 增益并静音，最终反向确认 Unbound 仍保留原来的 0.25。
///
/// 两个 playback control 除 effectGroup 外完全一致，因此任何输出差异都来自
/// 类别数组索引映射。getter 与 effectivePlayerGain 同时验证，避免只有组合入口
/// 正确而公开类别查询读取了错误槽位。
/// Unbound 控制的 0.25 在多次静音切换后仍保留，也间接覆盖类别槽的位字段保留。
bool testEffectGroupIsolation()
{
    MMM::Audio::KeySoundControlBank controls;
    const auto unbound = MMM::Audio::KeySoundPlaybackControl{
        .enabled     = true,
        .effectGroup = MMM::Audio::KeySoundEffectGroup::Unbound,
    };
    const auto bound = MMM::Audio::KeySoundPlaybackControl{
        .enabled     = true,
        .effectGroup = MMM::Audio::KeySoundEffectGroup::Bound,
    };

    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Unbound,
                                0.25F);
    // 恢复静音不应清除此前写入的 Unbound 0.25 增益。
    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Unbound,
                                 true);
    if ( controls.effectivePlayerGain(unbound) != 0.0F ||
         !nearlyEqual(controls.effectivePlayerGain(bound), 1.0F) ||
         controls.isEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound) ) {
        XERROR("Unbound Key sound controls leaked into Bound controls");
        return false;
    }

    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Unbound,
                                 false);
    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound, 1.5F);
    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound, true);
    const bool valid =
        controls.effectivePlayerGain(bound) == 0.0F &&
        nearlyEqual(controls.effectivePlayerGain(unbound), 0.25F) &&
        !controls.isEffectGroupMuted(
            MMM::Audio::KeySoundEffectGroup::Unbound) &&
        nearlyEqual(controls.getEffectGroupGain(
                        MMM::Audio::KeySoundEffectGroup::Unbound),
                    0.25F);
    if ( !valid )
        XERROR("Bound Key sound controls leaked into Unbound controls");
    return valid;
}

/// @brief 验证越界轨道不会写入固定控制库且读取为单位值。
/// @return 玩家、草稿、BGM 的全部越界接口保持单位语义时返回 true。
///
/// 使用 uint32 最大值保证索引远超固定上限。测试同时调用单字段 setter 和玩家
/// 完整快照 setter，确认任一入口都不会截断索引后误写数组内的有效轨道。
///
/// 草稿与 BGM setter 也全部调用，随后分别验证 mute、gain 和 BGM 有效增益。
/// 越界读取采用单位值而不是静音，使异常谱面轨道仍可播放并由上层决定降级策略。
/// 本场景不读取数组首尾有效索引，边界内行为已由其他场景覆盖。
bool testOutOfRangeTracks()
{
    MMM::Audio::KeySoundControlBank controls;
    constexpr std::uint32_t         INVALID_TRACK =
        std::numeric_limits<std::uint32_t>::max();
    controls.setPlayerTrackMuted(INVALID_TRACK, true);
    controls.setPlayerTrackGain(INVALID_TRACK, 0.0F);
    controls.setDraftTrackMuted(INVALID_TRACK, true);
    controls.setDraftTrackGain(INVALID_TRACK, 0.0F);
    controls.setBgmTrackMuted(INVALID_TRACK, true);
    controls.setBgmTrackGain(INVALID_TRACK, 0.0F);
    controls.setPlayerTrackControl(INVALID_TRACK,
                                   { .muted = true, .gain = 0.0F });

    // getter 统一返回未静音、单位增益，BGM 有效增益仍服从区域默认单位值。
    const auto snapshot = controls.getPlayerTrackControl(INVALID_TRACK);
    const bool valid =
        !controls.isPlayerTrackMuted(INVALID_TRACK) &&
        nearlyEqual(controls.getPlayerTrackGain(INVALID_TRACK), 1.0F) &&
        !snapshot.muted && nearlyEqual(snapshot.gain, 1.0F) &&
        !controls.isDraftTrackMuted(INVALID_TRACK) &&
        nearlyEqual(controls.getDraftTrackGain(INVALID_TRACK), 1.0F) &&
        !controls.isBgmTrackMuted(INVALID_TRACK) &&
        nearlyEqual(controls.getBgmTrackGain(INVALID_TRACK), 1.0F) &&
        nearlyEqual(controls.effectiveBgmTrackGain(INVALID_TRACK), 1.0F);
    if ( !valid ) XERROR("Out-of-range Key sound track was not unity");
    return valid;
}

/// @brief 验证并发发布期间读者只能看到两个完整 mute/gain 快照。
/// @return 完整快照不撕裂且并发单字段写入最终合并时返回 true。
///
/// 第一阶段单 writer 在两个关联快照间切换，reader 只能观察完整的 first 或
/// second，不能出现静音来自一份而增益来自另一份。第二阶段使用两个 writer
/// 分别更新 mute 与 gain，验证 CAS 重试不会覆盖对方已经提交的位字段。
///
/// first 为未静音 0.25，second 为静音 1.75，两个字段都不同，任何撕裂组合都
/// 与合法值集合区分。reader 不依赖观察次数，只持续到 writer 的 release 标志。
/// 第二阶段每个 writer 执行偶数次循环但最后一次索引为奇数，因此预期最终值
/// 明确为 muted=true、gain=1.5；错误的 load/modify/store 会丢掉其中一个结果。
///
/// writerFinished 使用 release/acquire 只负责结束循环，不参与控制槽正确性；
/// invalidSnapshot 使用 relaxed 即可，因为 writer join 与当前线程自身访问提供
/// 最终检查顺序。第二阶段两个线程 join 后再读取 merged，确保观察到全部写入。
bool testConcurrentCompleteSnapshots()
{
    MMM::Audio::KeySoundControlBank controls;
    constexpr std::uint32_t         TRACK_INDEX = 19U;
    const auto                      first = MMM::Audio::KeySoundControlSnapshot{
                             .muted = false,
                             .gain  = 0.25F,
    };
    const auto second = MMM::Audio::KeySoundControlSnapshot{
        .muted = true,
        .gain  = 1.75F,
    };
    controls.setPlayerTrackControl(TRACK_INDEX, first);

    std::atomic_bool writerFinished{ false };
    std::atomic_bool invalidSnapshot{ false };
    // 高迭代次数扩大 reader 与完整原子 store 交错的机会，不使用固定时长等待。
    std::thread writer([&controls, &writerFinished, first, second]() {
        for ( std::size_t iteration = 0U; iteration < 200000U; ++iteration ) {
            controls.setPlayerTrackControl(
                TRACK_INDEX, (iteration & 1U) == 0U ? second : first);
        }
        writerFinished.store(true, std::memory_order_release);
    });

    // 读循环只接受两个合法组合，发现首个撕裂值即可结束并记录。
    do {
        const auto snapshot = controls.getPlayerTrackControl(TRACK_INDEX);
        const bool isFirst =
            !snapshot.muted && nearlyEqual(snapshot.gain, first.gain);
        const bool isSecond =
            snapshot.muted && nearlyEqual(snapshot.gain, second.gain);
        if ( !isFirst && !isSecond ) {
            invalidSnapshot.store(true, std::memory_order_relaxed);
            break;
        }
    } while ( !writerFinished.load(std::memory_order_acquire) );
    // join 确保 writer 的生命周期结束，再根据 reader 记录判定是否发生撕裂。
    writer.join();

    if ( invalidSnapshot.load(std::memory_order_relaxed) ) {
        XERROR("Concurrent reader observed a torn Key sound snapshot");
        return false;
    }

    // 两个单字段 writer 同时竞争同一 packed 原子，最终奇数迭代值已知。
    std::thread muteWriter([&controls]() {
        for ( std::size_t iteration = 0U; iteration < 100000U; ++iteration ) {
            controls.setPlayerTrackMuted(TRACK_INDEX, (iteration & 1U) != 0U);
        }
    });
    std::thread gainWriter([&controls]() {
        for ( std::size_t iteration = 0U; iteration < 100000U; ++iteration ) {
            controls.setPlayerTrackGain(TRACK_INDEX,
                                        (iteration & 1U) != 0U ? 1.5F : 0.5F);
        }
    });
    // 两个 join 完成后，最终 packed 字必须同时保留各 writer 的最后字段值。
    muteWriter.join();
    gainWriter.join();

    const auto merged = controls.getPlayerTrackControl(TRACK_INDEX);
    const bool valid  = merged.muted && nearlyEqual(merged.gain, 1.5F);
    if ( !valid ) XERROR("Concurrent field updates lost a Key sound value");
    return valid;
}

}  // namespace

/// @brief 运行固定容量 Key 音运行时控制测试。
/// @return 七个值语义与并发场景全部通过时返回零。
///
/// 各场景重新构造控制库，避免前一场景的静音、增益和并发最终值污染后续断言。
/// 使用短路连接保留首个失败函数写出的明确日志，并避免在基本默认值错误后继续
/// 执行依赖相同不变量的并发压力场景。
int main()
{
    const bool passed = testDefaults() && testMutePreservesGainAndClamping() &&
                        testGainComposition() && testDraftAreaIsolation() &&
                        testEffectGroupIsolation() && testOutOfRangeTracks() &&
                        testConcurrentCompleteSnapshots();
    return passed ? 0 : 1;
}
