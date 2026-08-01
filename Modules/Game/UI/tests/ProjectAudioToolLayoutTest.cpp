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

/// @brief 验证默认宽度能完整容纳文件名且尊重不同类型的宽度下限。
bool testDefaultWidth()
{
    return near(MMM::UI::ProjectAudioToolLayout::calculateDefaultWidth(
                    120.0F, 6.0F, 92.0F),
                134.0F) &&
           near(MMM::UI::ProjectAudioToolLayout::calculateDefaultWidth(
                    60.0F, 6.0F, 202.0F),
                202.0F);
}

/// @brief 验证方块尺寸下限完整容纳统一按钮行、进度条和两行标签。
bool testControlDrivenMinimumSize()
{
    return near(MMM::UI::ProjectAudioToolLayout::calculateControlMinimumWidth(
                    28.0F, 4.0F, 6.0F, 4U),
                136.0F) &&
           near(MMM::UI::ProjectAudioToolLayout::calculateControlMinimumHeight(
                    20.0F, 5.0F, 3.0F, 28.0F, 4.0F, 6.0F),
                96.0F);
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
    if ( !near(snapped.x, 200.0F) || !near(snapped.y, 130.0F) ||
         !locks.x.targetLine || !near(*locks.x.targetLine, 200.0F) ||
         !locks.y.targetLine || !near(*locks.y.targetLine, 130.0F) ) {
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

/// @brief 验证缩放活动边可通过自身边缘或中心吸附到目标锚点。
bool testResizeSnapping()
{
    const Rect              canvas{ 0.0F, 0.0F, 400.0F, 300.0F };
    const std::vector<Rect> targets{
        Rect{ 200.0F, 100.0F, 100.0F, 100.0F },
    };
    MMM::UI::ProjectAudioToolLayout::SnapLocks locks;
    auto resized = MMM::UI::ProjectAudioToolLayout::snapResizeRect(
        Rect{ 100.0F, 50.0F, 97.0F, 147.0F },
        MMM::UI::ProjectAudioToolLayout::ResizeEdge::Maximum,
        MMM::UI::ProjectAudioToolLayout::ResizeEdge::Maximum,
        canvas,
        targets,
        48.0F,
        48.0F,
        6.0F,
        12.0F,
        locks);
    if ( !near(resized.right(), 200.0F) || !near(resized.bottom(), 200.0F) ||
         !locks.x.targetLine || !near(*locks.x.targetLine, 200.0F) ||
         !locks.y.targetLine || !near(*locks.y.targetLine, 200.0F) ) {
        return false;
    }

    locks   = {};
    resized = MMM::UI::ProjectAudioToolLayout::snapResizeRect(
        Rect{ 103.0F, 50.0F, 197.0F, 150.0F },
        MMM::UI::ProjectAudioToolLayout::ResizeEdge::Minimum,
        MMM::UI::ProjectAudioToolLayout::ResizeEdge::None,
        canvas,
        targets,
        48.0F,
        48.0F,
        6.0F,
        12.0F,
        locks);
    return near(resized.x, 100.0F) && near(resized.right(), 300.0F);
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

/// @brief 验证移动遮挡的增量标签区域会立即转移到仍可见的最大一侧。
bool testIncrementalVisibleLabelCell()
{
    const Rect base{ 0.0F, 0.0F, 100.0F, 100.0F };
    const Rect occluder{ 0.0F, 0.0F, 65.0F, 100.0F };
    const Rect visible =
        MMM::UI::ProjectAudioToolLayout::largestVisibleCellWithOneOccluder(
            base, occluder);
    if ( !near(visible.x, 65.0F) || !near(visible.width, 35.0F) ||
         !near(visible.height, 100.0F) ) {
        return false;
    }

    const Rect movedAway{ 150.0F, 0.0F, 65.0F, 100.0F };
    const Rect restored =
        MMM::UI::ProjectAudioToolLayout::largestVisibleCellWithOneOccluder(
            base, movedAway);
    return near(restored.x, base.x) && near(restored.y, base.y) &&
           near(restored.width, base.width) &&
           near(restored.height, base.height);
}

/// @brief 验证扩大前景方块时会停在下层方块的 35% 可见边界。
bool testResizeVisibilityConstraint()
{
    const Rect base{ 100.0F, 100.0F, 100.0F, 100.0F };
    const MMM::UI::ProjectAudioToolLayout::VisibilityConstraint constraint{
        .base = base,
    };
    const std::vector constraints{ constraint };
    const Rect        result =
        MMM::UI::ProjectAudioToolLayout::constrainResizeVisibility(
            Rect{ 100.0F, 100.0F, 50.0F, 100.0F },
            Rect{ 100.0F, 100.0F, 100.0F, 100.0F },
            constraints,
            0.35F);
    const std::vector occluders{ result };
    return MMM::UI::ProjectAudioToolLayout::visibleRatio(base, occluders) >=
               0.35F - 1e-4F &&
           result.width > 64.9F && result.width < 65.1F;
}

/// @brief 验证预处理会剔除无关遮挡且候选方块仍按固定遮挡并集限制。
bool testPreparedVisibilityConstraint()
{
    const Rect        base{ 100.0F, 100.0F, 100.0F, 100.0F };
    const std::vector fixedOccluders{
        Rect{ 100.0F, 100.0F, 30.0F, 100.0F },
        Rect{ 500.0F, 500.0F, 100.0F, 100.0F },
    };
    const auto constraint =
        MMM::UI::ProjectAudioToolLayout::prepareVisibilityConstraint(
            base, fixedOccluders);
    if ( constraint.fixedOccluders.size() != 1 ||
         !near(constraint.fixedCoveredArea, 3000.0F) ) {
        return false;
    }

    const std::vector constraints{ constraint };
    const Rect result = MMM::UI::ProjectAudioToolLayout::constrainVisibility(
        Rect{ 130.0F, 100.0F, 70.0F, 100.0F },
        Rect{ 0.0F, 0.0F, 600.0F, 600.0F },
        constraints,
        0.35F);
    return MMM::UI::ProjectAudioToolLayout::visibleRatioWithCandidate(
               constraint, result) >= 0.35F - 1e-4F;
}

/// @brief 验证旧布局已有轻微可见率缺口时不会让无关拖动反复尝试修复。
bool testExistingVisibilityDeficitIsBaseline()
{
    const Rect        base{ 100.0F, 100.0F, 100.0F, 100.0F };
    const std::vector fixedOccluders{
        Rect{ 100.0F, 100.0F, 66.0F, 100.0F },
    };
    const auto constraint =
        MMM::UI::ProjectAudioToolLayout::prepareVisibilityConstraint(
            base, fixedOccluders);
    const Rect unrelatedCandidate{ 400.0F, 400.0F, 100.0F, 100.0F };
    const Rect worseningCandidate{ 166.0F, 100.0F, 34.0F, 100.0F };
    return near(MMM::UI::ProjectAudioToolLayout::visibilityDeficit(
                    constraint, unrelatedCandidate, 0.35F),
                0.0F) &&
           MMM::UI::ProjectAudioToolLayout::visibilityDeficit(
               constraint, worseningCandidate, 0.35F) > 0.0F;
}

/// @brief 验证批量方块并集去重并在整体平移时共同遵守下层可见率。
bool testBatchMoveVisibilityConstraint()
{
    const std::vector selectedRects{
        Rect{ 0.0F, 0.0F, 100.0F, 100.0F },
        Rect{ 50.0F, 0.0F, 100.0F, 100.0F },
    };
    const auto unionCells =
        MMM::UI::ProjectAudioToolLayout::buildUnionCells(selectedRects);
    float unionArea = 0.0F;
    for ( const auto& cell : unionCells ) {
        unionArea += MMM::UI::ProjectAudioToolLayout::area(cell);
    }
    if ( !near(unionArea, 15000.0F) ) return false;

    const auto fixedConstraint =
        MMM::UI::ProjectAudioToolLayout::prepareVisibilityConstraint(
            Rect{ 200.0F, 0.0F, 100.0F, 100.0F }, std::vector<Rect>{});
    const std::vector constraints{ fixedConstraint };
    const Rect        initialBounds{ 0.0F, 0.0F, 150.0F, 100.0F };
    const Rect        result =
        MMM::UI::ProjectAudioToolLayout::constrainTranslatedVisibility(
            initialBounds,
            Rect{ 200.0F, 0.0F, 150.0F, 100.0F },
            initialBounds,
            unionCells,
            Rect{ 0.0F, 0.0F, 500.0F, 500.0F },
            constraints,
            0.35F);
    return result.x < 200.0F &&
           MMM::UI::ProjectAudioToolLayout::translatedVisibilityDeficit(
               constraints,
               unionCells,
               result.x - initialBounds.x,
               result.y - initialBounds.y,
               0.35F) <= 1e-4F;
}

}  // namespace

/// @brief 运行项目音频工具几何布局测试。
int main()
{
    if ( !testDefaultWidth() ) return 1;
    if ( !testControlDrivenMinimumSize() ) return 2;
    if ( !testEdgeAndCenterSnapping() ) return 3;
    if ( !testStackingSnapAndHysteresis() ) return 4;
    if ( !testResizeSnapping() ) return 5;
    if ( !testMinimumVisibleRatio() ) return 6;
    if ( !testVisibleLabelCell() ) return 7;
    if ( !testIncrementalVisibleLabelCell() ) return 8;
    if ( !testResizeVisibilityConstraint() ) return 9;
    if ( !testPreparedVisibilityConstraint() ) return 10;
    if ( !testExistingVisibilityDeficitIsBaseline() ) return 11;
    if ( !testBatchMoveVisibilityConstraint() ) return 12;
    return 0;
}
