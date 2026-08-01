#include "ui/imgui/manager/BeatLineDisplayModeHistory.h"

namespace
{
using MMM::Config::BeatLineDisplayMode;
using MMM::UI::BeatLineDisplayModeHistory;

/// @brief 验证无历史时沿用显示与隐藏之间的传统切换。
/// @return 默认切换组合符合预期时返回 true。
bool testDefaultPair()
{
    BeatLineDisplayModeHistory visibleHistory;
    if ( visibleHistory.toggleTarget(BeatLineDisplayMode::Always) !=
             BeatLineDisplayMode::Hidden ||
         visibleHistory.toggleTarget(BeatLineDisplayMode::Hidden) !=
             BeatLineDisplayMode::Always ) {
        return false;
    }

    BeatLineDisplayModeHistory automaticHistory;
    if ( automaticHistory.toggleTarget(BeatLineDisplayMode::NearCursor) !=
         BeatLineDisplayMode::Hidden ) {
        return false;
    }

    BeatLineDisplayModeHistory hiddenHistory;
    return hiddenHistory.toggleTarget(BeatLineDisplayMode::Hidden) ==
           BeatLineDisplayMode::Always;
}

/// @brief 验证快捷键只在最近使用的两个模式间往返。
/// @return 连续快捷键切换未进入第三种模式时返回 true。
bool testRecentPairToggle()
{
    BeatLineDisplayModeHistory history;
    history.observe(BeatLineDisplayMode::Always);
    history.observe(BeatLineDisplayMode::NearCursor);
    history.observe(BeatLineDisplayMode::Hidden);

    return history.toggleTarget(BeatLineDisplayMode::Hidden) ==
               BeatLineDisplayMode::NearCursor &&
           history.toggleTarget(BeatLineDisplayMode::NearCursor) ==
               BeatLineDisplayMode::Hidden &&
           history.toggleTarget(BeatLineDisplayMode::Hidden) ==
               BeatLineDisplayMode::NearCursor;
}

/// @brief 验证重复选择当前模式不会挤掉最近的另一个模式。
/// @return 最近另一个模式保持不变时返回 true。
bool testDuplicateObservation()
{
    BeatLineDisplayModeHistory history;
    history.observe(BeatLineDisplayMode::Always);
    history.observe(BeatLineDisplayMode::NearCursor);
    history.observe(BeatLineDisplayMode::NearCursor);

    return history.toggleTarget(BeatLineDisplayMode::NearCursor) ==
           BeatLineDisplayMode::Always;
}
}  // namespace

/// @brief 运行分拍线最近模式切换回归测试。
/// @return 所有断言通过时返回 0。
int main()
{
    return testDefaultPair() && testRecentPairToggle() &&
                   testDuplicateObservation()
               ? 0
               : 1;
}
