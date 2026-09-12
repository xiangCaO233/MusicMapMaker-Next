#include "ui/imgui/menu/package/PackageDefaultSelection.h"

#include <array>

/// @file PackageDefaultSelectionTest.cpp
/// @brief 打包窗口默认谱面选择策略的前台、回退和无效会话回归测试。
/// @details 固定数组模拟停靠快照，不依赖真实项目；路径仅作为稳定会话键参与
/// 匹配，不执行文件系统访问。

namespace
{
using MMM::UI::PackageOpenBeatmapState;
using MMM::UI::shouldDefaultSelectPackageBeatmap;

/// @brief 检查单停靠组只默认选择前台谱面。
/// @return 行为符合预期时返回 true。
/// @note 第二个谱面故意仅标记为活动，用于确认前台快照优先。
bool checkSingleForegroundSelection()
{
    // 单停靠组只有一个前台画布，后台活动标记不能覆盖前台判定。
    constexpr std::array states{
        PackageOpenBeatmapState{ "/project/a.mc", true, false, false },
        PackageOpenBeatmapState{ "/project/b.mc", false, true, false },
    };
    return shouldDefaultSelectPackageBeatmap("/project/a.mc", states) &&
           !shouldDefaultSelectPackageBeatmap("/project/b.mc", states);
}

/// @brief 检查并排停靠组会同时默认选择各自前台谱面。
/// @return 行为符合预期时返回 true。
/// @note 第三个候选没有任何选中标记，作为负例保留。
bool checkMultipleForegroundSelection()
{
    // 并排停靠允许每个组各有一个前台画布，二者都应默认勾选。
    constexpr std::array states{
        PackageOpenBeatmapState{ "/project/a.mc", true, true, false },
        PackageOpenBeatmapState{ "/project/b.mc", true, false, false },
        PackageOpenBeatmapState{ "/project/c.mc", false, false, false },
    };
    return shouldDefaultSelectPackageBeatmap("/project/a.mc", states) &&
           shouldDefaultSelectPackageBeatmap("/project/b.mc", states) &&
           !shouldDefaultSelectPackageBeatmap("/project/c.mc", states);
}

/// @brief 检查无前台快照时只回退当前活动谱面且不会全选。
/// @return 行为符合预期时返回 true。
/// @note 未出现在快照中的候选也必须保持未选中。
bool checkActiveFallbackSelection()
{
    // 没有任何前台快照时，才允许使用全局活动谱面兜底。
    constexpr std::array states{
        PackageOpenBeatmapState{ "/project/a.mc", false, false, false },
        PackageOpenBeatmapState{ "/project/b.mc", false, true, false },
    };
    return !shouldDefaultSelectPackageBeatmap("/project/a.mc", states) &&
           shouldDefaultSelectPackageBeatmap("/project/b.mc", states) &&
           !shouldDefaultSelectPackageBeatmap("/project/c.mc", states);
}

/// @brief 检查占位画布、空路径和无映射候选均不会默认选中。
/// @return 行为符合预期时返回 true。
/// @note 占位标记优先于前台与活动标记，防止打包无实体会话。
bool checkInvalidSessionSelection()
{
    // 占位会话即使同时标记前台和活动，也不代表可打包谱面。
    constexpr std::array states{
        PackageOpenBeatmapState{ "/project/a.mc", true, true, true },
        PackageOpenBeatmapState{ {}, true, false, false },
    };
    return !shouldDefaultSelectPackageBeatmap("/project/a.mc", states) &&
           !shouldDefaultSelectPackageBeatmap("/project/b.mc", states) &&
           !shouldDefaultSelectPackageBeatmap({}, states);
}
}  // namespace

/// @brief 覆盖打包窗口按前台停靠画布计算默认谱面选择的场景。
/// @return 所有断言通过时返回 0。
/// @note 四个场景分别使用独立快照，避免候选状态相互影响。
int main()
{
    // 短路组合保留首个失败场景的调试位置。
    return checkSingleForegroundSelection() &&
                   checkMultipleForegroundSelection() &&
                   checkActiveFallbackSelection() &&
                   checkInvalidSessionSelection()
               ? 0
               : 1;
}
