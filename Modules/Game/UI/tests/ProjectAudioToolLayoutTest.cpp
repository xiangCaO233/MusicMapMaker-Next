#include "ui/imgui/manager/ProjectAudioToolLayout.h"

#include <cmath>
#include <vector>

namespace
{

using MMM::UI::ProjectAudioToolLayout::Rect;

/// @brief 使用小容差比较布局坐标。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 验证自身边缘和中心可吸附到方块及可见画布的对应锚点。
bool testEdgeAndCenterSnapping()
{
    const Rect              canvas{ 0.0F, 0.0F, 400.0F, 300.0F };
    const std::vector<Rect> targets{
        Rect{ 100.0F, 80.0F, 100.0F, 100.0F },
    };

    MMM::UI::ProjectAudioToolLayout::SnapLocks locks;
    auto snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 197.0F, 132.0F, 50.0F, 40.0F },
        canvas,
        targets,
        6.0F,
        12.0F,
        locks);
    if ( !near(snapped.x, 200.0F) || !near(snapped.y, 130.0F) ) {
        return false;
    }

    locks   = {};
    snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 347.0F, 257.0F, 50.0F, 40.0F },
        canvas,
        targets,
        6.0F,
        12.0F,
        locks);
    return near(snapped.x, 350.0F) && near(snapped.y, 260.0F);
}

/// @brief 验证 35% 叠层锚点可吸附并在拖过释放阈值前保持锁定。
bool testStackingSnapAndHysteresis()
{
    const Rect              canvas{ 0.0F, 0.0F, 400.0F, 300.0F };
    const std::vector<Rect> targets{
        Rect{ 100.0F, 80.0F, 100.0F, 100.0F },
    };
    MMM::UI::ProjectAudioToolLayout::SnapLocks locks;
    auto snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 133.0F, 80.0F, 50.0F, 50.0F },
        canvas,
        targets,
        4.0F,
        12.0F,
        locks);
    if ( !near(snapped.x, 135.0F) ) return false;

    snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 143.0F, 80.0F, 50.0F, 50.0F },
        canvas,
        targets,
        4.0F,
        12.0F,
        locks);
    if ( !near(snapped.x, 135.0F) ) return false;

    snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 150.0F, 80.0F, 50.0F, 50.0F },
        canvas,
        targets,
        4.0F,
        12.0F,
        locks);
    return !near(snapped.x, 135.0F);
}

/// @brief 验证前景方块不能把任一下层方块遮挡到不足 35%。
bool testMinimumVisibleRatio()
{
    const Rect base{ 100.0F, 100.0F, 100.0F, 100.0F };
    const MMM::UI::ProjectAudioToolLayout::VisibilityConstraint constraint{
        .base = base,
    };
    const std::vector constraints{ constraint };
    const Rect result = MMM::UI::ProjectAudioToolLayout::constrainVisibility(
        Rect{ 100.0F, 100.0F, 100.0F, 100.0F },
        Rect{ 0.0F, 0.0F, 400.0F, 300.0F },
        constraints,
        0.35F);
    const std::vector occluders{ result };
    return MMM::UI::ProjectAudioToolLayout::visibleRatio(base, occluders) >=
           0.35F - 1e-4F;
}

/// @brief 验证下层标签区域落在叠层后仍露出的部分。
bool testVisibleLabelCell()
{
    const Rect        base{ 0.0F, 0.0F, 100.0F, 100.0F };
    const std::vector occluders{
        Rect{ 0.0F, 0.0F, 65.0F, 100.0F },
    };
    const Rect visible =
        MMM::UI::ProjectAudioToolLayout::largestVisibleCell(base, occluders);
    return near(visible.x, 65.0F) && near(visible.width, 35.0F) &&
           near(visible.height, 100.0F);
}

}  // namespace

/// @brief 运行项目音频工具几何布局测试。
int main()
{
    if ( !testEdgeAndCenterSnapping() ) return 1;
    if ( !testStackingSnapAndHysteresis() ) return 2;
    if ( !testMinimumVisibleRatio() ) return 3;
    if ( !testVisibleLabelCell() ) return 4;
    return 0;
}
