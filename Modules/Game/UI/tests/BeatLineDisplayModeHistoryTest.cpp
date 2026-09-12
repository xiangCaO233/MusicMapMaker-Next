#include "ui/imgui/manager/BeatLineDisplayModeHistory.h"

/// @file BeatLineDisplayModeHistoryTest.cpp
/// @brief 分拍线快捷切换的默认配对、最近配对和重复观察回归测试。
/// @details 测试只操作值类型历史，不依赖 UI 上下文；每个场景使用独立实例，
/// 防止前一场景的最近模式污染后续断言。

namespace
{
using MMM::Config::BeatLineDisplayMode;
using MMM::UI::BeatLineDisplayModeHistory;

/// @brief 验证无历史时沿用显示与隐藏之间的传统切换。
/// @return 默认切换组合符合预期时返回 true。
/// @note Always、NearCursor 与 Hidden 三种起始模式均单独覆盖。
bool testDefaultPair()
{
    // 显示模式在没有观察历史时默认切换到隐藏。
    BeatLineDisplayModeHistory visibleHistory;
    if ( visibleHistory.toggleTarget(BeatLineDisplayMode::Always) !=
             BeatLineDisplayMode::Hidden ||
         visibleHistory.toggleTarget(BeatLineDisplayMode::Hidden) !=
             BeatLineDisplayMode::Always ) {
        return false;
    }

    // 自动显示也属于可见状态，因此默认目标仍是隐藏。
    BeatLineDisplayModeHistory automaticHistory;
    if ( automaticHistory.toggleTarget(BeatLineDisplayMode::NearCursor) !=
         BeatLineDisplayMode::Hidden ) {
        return false;
    }

    // 从隐藏开始时必须回到传统的始终显示模式。
    BeatLineDisplayModeHistory hiddenHistory;
    return hiddenHistory.toggleTarget(BeatLineDisplayMode::Hidden) ==
           BeatLineDisplayMode::Always;
}

/// @brief 验证快捷键只在最近使用的两个模式间往返。
/// @return 连续快捷键切换未进入第三种模式时返回 true。
/// @note 第一次观察的 Always 应在第三种模式出现后退出最近配对。
bool testRecentPairToggle()
{
    BeatLineDisplayModeHistory history;
    // 依次模拟用户从始终显示切换到跟随光标，再切换到隐藏。
    history.observe(BeatLineDisplayMode::Always);
    history.observe(BeatLineDisplayMode::NearCursor);
    history.observe(BeatLineDisplayMode::Hidden);
    // 快捷键只应在最后两个不同模式之间往返。
    return history.toggleTarget(BeatLineDisplayMode::Hidden) ==
               BeatLineDisplayMode::NearCursor &&
           history.toggleTarget(BeatLineDisplayMode::NearCursor) ==
               BeatLineDisplayMode::Hidden &&
           history.toggleTarget(BeatLineDisplayMode::Hidden) ==
               BeatLineDisplayMode::NearCursor;
}

/// @brief 验证重复选择当前模式不会挤掉最近的另一个模式。
/// @return 最近另一个模式保持不变时返回 true。
/// @note 连续观察 NearCursor 模拟 UI 重复同步同一配置值。
bool testDuplicateObservation()
{
    BeatLineDisplayModeHistory history;
    // 重复上报当前模式不应覆盖前一个不同模式。
    history.observe(BeatLineDisplayMode::Always);
    history.observe(BeatLineDisplayMode::NearCursor);
    history.observe(BeatLineDisplayMode::NearCursor);

    return history.toggleTarget(BeatLineDisplayMode::NearCursor) ==
           BeatLineDisplayMode::Always;
}
}  // namespace

/// @brief 运行分拍线最近模式切换回归测试。
/// @return 所有断言通过时返回 0。
/// @note 三个场景共同覆盖无历史、有历史以及重复事件输入。
int main()
{
    // 失败统一返回非零值，由调试器定位具体布尔场景。
    return testDefaultPair() && testRecentPairToggle() &&
                   testDuplicateObservation()
               ? 0
               : 1;
}
