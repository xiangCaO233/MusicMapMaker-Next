#include "ui/imgui/menu/package/PackageDefaultSelection.h"

#include <array>

namespace
{
using MMM::UI::PackageOpenBeatmapState;
using MMM::UI::shouldDefaultSelectPackageBeatmap;

/// @brief 检查单停靠组只默认选择前台谱面。
/// @return 行为符合预期时返回 true。
bool checkSingleForegroundSelection()
{
    constexpr std::array states{
        PackageOpenBeatmapState{ "/project/a.mc", true, false, false },
        PackageOpenBeatmapState{ "/project/b.mc", false, true, false },
    };
    return shouldDefaultSelectPackageBeatmap("/project/a.mc", states) &&
           !shouldDefaultSelectPackageBeatmap("/project/b.mc", states);
}

/// @brief 检查并排停靠组会同时默认选择各自前台谱面。
/// @return 行为符合预期时返回 true。
bool checkMultipleForegroundSelection()
{
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
bool checkActiveFallbackSelection()
{
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
bool checkInvalidSessionSelection()
{
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
int main()
{
    return checkSingleForegroundSelection() &&
                   checkMultipleForegroundSelection() &&
                   checkActiveFallbackSelection() &&
                   checkInvalidSessionSelection()
               ? 0
               : 1;
}
