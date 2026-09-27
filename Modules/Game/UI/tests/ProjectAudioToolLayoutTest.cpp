#include "ui/imgui/manager/ProjectAudioToolLayout.h"

#include <array>
#include <cmath>
#include <vector>

namespace
{

using MMM::UI::ProjectAudioToolLayout::Rect;

/// @brief 项目音频工具纯几何测试约定。
///
/// 测试不创建 ImGui 上下文，也不打开工程。所有输入都使用逻辑画布坐标，直接验证
/// `ProjectAudioToolLayout` 的确定性几何函数。
///
/// 覆盖范围包括默认尺寸、控件驱动下限、移动与缩放吸附、锁定迟滞、多层堆叠、
/// 遮挡可见率、标签最大可见单元、批量移动并集以及相机缩放锚点。
///
/// 浮点结果统一通过 `near` 比较，涉及可见率下限时允许同等级容差。测试矩形刻意
/// 使用整数边界，使失败能够直接反映几何分支而非累计舍入噪声。
///
/// `main` 返回值与测试顺序一一对应，CI 可从退出码快速定位失败场景。新增案例时
/// 必须使用新的稳定退出码，不能改变既有编号含义。
///
/// 吸附测试参数约定：第四个浮点值是进入吸附距离，第五个是释放锁的迟滞距离。
/// 释放阈值大于进入阈值，保证指针在目标附近小幅抖动时布局不会反复跳变。
///
/// 可见率测试把后加入矩形视为更高图层。`VisibilityConstraint` 保存下层 base 与
/// 固定遮挡并集，待移动候选只能维持或改善历史缺口，不能进一步降低可见面积。
///
/// 相机测试区分屏幕坐标、滚动坐标和逻辑画布坐标。DPI 与 camera zoom 都必须
/// 进入换算，缩放前后的指针锚点在逻辑空间保持一致。
///
/// 测试只验证公开纯函数结果，不复刻实现内部候选排序。期望值来自用户可见几何
/// 契约，重构算法时只要契约不变就不应修改这些断言。
///
/// 所有容器和矩形都在栈上构造，测试不依赖全局可变状态。各案例可以独立运行，
/// 失败不会留下影响下一案例的锁或缓存。
///
/// SnapLocks 仅在单个模拟拖动序列内复用；开始新场景前显式清空或重新构造，确保
/// 测试不会把不相关交互误当作同一次迟滞过程。
///
/// 可见率阈值统一使用 0.35，与产品要求的最小可见比例保持一致。

/// @brief 使用小容差比较布局坐标。
/// @param lhs 左侧浮点值。
/// @param rhs 右侧浮点值。
/// @return 两值绝对差小于布局容差时返回 true。
bool near(float lhs, float rhs)
{
    // 容差只吸收浮点表示误差，不应掩盖可见像素级偏差。
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 验证默认宽度能完整容纳文件名且尊重不同类型的宽度下限。
///
/// 第一组输入由文件名宽度、两侧留白和控件余量共同主导；第二组输入则由外部类型
/// 下限主导。两条断言共同覆盖 max 分支。
bool testDefaultWidth()
{
    // 两个子案例必须同时通过，任一分支回归都返回 false。
    // 文件名 120 加两侧 6 与固定余量后得到 134。
    return near(MMM::UI::ProjectAudioToolLayout::calculateDefaultWidth(
                    120.0F, 6.0F, 92.0F),
                134.0F) &&
           // 类型下限 202 大于内容需求，因此直接采用 202。
           near(MMM::UI::ProjectAudioToolLayout::calculateDefaultWidth(
                    60.0F, 6.0F, 202.0F),
                202.0F);
}

/// @brief 验证方块尺寸下限完整容纳统一按钮行、进度条和两行标签。
///
/// 宽度案例覆盖四个方形按钮及按钮间距；高度案例覆盖两行文字、控制行、进度条和
/// 垂直间距，防止主题尺寸增大后内部控件相互覆盖。
bool testControlDrivenMinimumSize()
{
    // 输入数值独立于主题对象，使测试在无 ImGui 上下文环境可运行。
    // 四个 28 像素按钮配合三段间距和左右 padding 得到宽度下限。
    return near(MMM::UI::ProjectAudioToolLayout::calculateControlMinimumWidth(
                    28.0F, 4.0F, 6.0F, 4U),
                136.0F) &&
           // 高度计算应完整累加标签、控制与进度区域。
           near(MMM::UI::ProjectAudioToolLayout::calculateControlMinimumHeight(
                    20.0F, 5.0F, 3.0F, 28.0F, 4.0F, 6.0F),
                96.0F);
}

/// @brief 验证自身边缘和中心可吸附到方块及可见画布的对应锚点。
///
/// 首次移动同时命中目标右边缘和垂直中心，验证两轴锁及 targetLine；第二次清空锁
/// 后靠近画布右下边界，验证画布锚点同样参与候选。
bool testEdgeAndCenterSnapping()
{
    // 画布提供左右、上下和中心锚点。
    const Rect canvas{ 0.0F, 0.0F, 400.0F, 300.0F };
    // 唯一目标位于画布内部，边界和中心均为整值。
    const std::vector<Rect> targets{
        Rect{ 100.0F, 80.0F, 100.0F, 100.0F },
    };

    // 初始无锁状态允许两轴分别选择距离最近候选。
    MMM::UI::ProjectAudioToolLayout::SnapLocks locks;
    auto snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 197.0F, 132.0F, 50.0F, 40.0F },
        canvas,
        targets,
        6.0F,
        12.0F,
        locks);
    // 结果应移动到 x=200 边缘并让中心线落在 y=130。
    if ( !near(snapped.x, 200.0F) || !near(snapped.y, 130.0F) ||
         !locks.x.targetLine || !near(*locks.x.targetLine, 200.0F) ||
         !locks.y.targetLine || !near(*locks.y.targetLine, 130.0F) ) {
        return false;
    }

    // 清空目标锁，避免上一场景的迟滞影响画布边缘断言。
    locks   = {};
    snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 347.0F, 257.0F, 50.0F, 40.0F },
        canvas,
        targets,
        6.0F,
        12.0F,
        locks);
    // 50x40 方块贴到 400x300 画布时左上角应为 350x260。
    return near(snapped.x, 350.0F) && near(snapped.y, 260.0F);
}

/// @brief 验证同尺寸方块精确垂直堆叠并在拖过释放阈值前保持锁定。
///
/// 用四次连续调用模拟真实拖动：先只建立横向锁，再进入 65% 覆盖的垂直吸附，随后
/// 在释放阈值内保持两轴锁，最后越过迟滞阈值释放。
bool testStackingSnapAndHysteresis()
{
    // 单一目标便于隔离堆叠吸附和迟滞行为。
    const Rect              canvas{ 0.0F, 0.0F, 400.0F, 300.0F };
    const std::vector<Rect> targets{
        Rect{ 100.0F, 80.0F, 100.0F, 100.0F },
    };
    MMM::UI::ProjectAudioToolLayout::SnapLocks locks;
    // y=50 尚未接近任何堆叠层级，只应把 x 对齐到目标。
    auto snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 102.0F, 50.0F, 100.0F, 100.0F },
        canvas,
        targets,
        4.0F,
        12.0F,
        locks);
    if ( !near(snapped.x, 100.0F) || near(snapped.y, 115.0F) ) return false;

    // y=113 进入 115 候选吸附距离，建立垂直锁。
    snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 102.0F, 113.0F, 100.0F, 100.0F },
        canvas,
        targets,
        4.0F,
        12.0F,
        locks);
    if ( !near(snapped.x, 100.0F) || !near(snapped.y, 115.0F) ) return false;

    // 偏移到 110/125 仍未超过 12 像素释放阈值，应保持锁定结果。
    snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 110.0F, 125.0F, 100.0F, 100.0F },
        canvas,
        targets,
        4.0F,
        12.0F,
        locks);
    if ( !near(snapped.x, 100.0F) || !near(snapped.y, 115.0F) ) return false;

    // 进一步拖动越过释放阈值，两轴都应离开旧锁定坐标。
    snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 113.0F, 128.0F, 100.0F, 100.0F },
        canvas,
        targets,
        4.0F,
        12.0F,
        locks);
    return !near(snapped.x, 100.0F) && !near(snapped.y, 115.0F);
}

/// @brief 验证同尺寸方块支持覆盖 65%、50% 和 25% 的精确堆叠等级。
///
/// 三个候选分别对应目标顶部向下 35、50、75 像素的偏移，即前景覆盖目标高度的
/// 65%、50% 与 25%。每轮使用全新锁，确保结果来自候选生成而非迟滞复用。
bool testStackingSnapLevels()
{
    // 目标固定为 100x100，便于直接用百分比解释垂直坐标。
    const Rect              canvas{ 0.0F, 0.0F, 400.0F, 300.0F };
    const std::vector<Rect> targets{
        Rect{ 100.0F, 80.0F, 100.0F, 100.0F },
    };
    // 原始位置各距目标层级一像素，处于四像素吸附阈值内。
    constexpr std::array<float, 3> RAW_VERTICAL_POSITIONS{
        114.0F,
        129.0F,
        154.0F,
    };
    // 期望位置是三个精确堆叠层级。
    constexpr std::array<float, 3> EXPECTED_VERTICAL_POSITIONS{
        115.0F,
        130.0F,
        155.0F,
    };
    for ( std::size_t index = 0; index < RAW_VERTICAL_POSITIONS.size();
          ++index ) {
        // 每个层级独立构造锁，避免前一迭代锁定状态污染结果。
        MMM::UI::ProjectAudioToolLayout::SnapLocks locks;
        const auto snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
            Rect{ 101.0F, RAW_VERTICAL_POSITIONS[index], 100.0F, 100.0F },
            canvas,
            targets,
            4.0F,
            12.0F,
            locks);
        // 横向边缘与对应垂直层级必须同时精确对齐。
        if ( !near(snapped.x, 100.0F) ||
             !near(snapped.y, EXPECTED_VERTICAL_POSITIONS[index]) ) {
            return false;
        }
    }
    // 三个层级均准确吸附才视为通过。
    return true;
}

/// @brief 验证连续堆叠可在不同方块之间自由组合精确覆盖等级。
///
/// 第二个目标本身已按 65% 覆盖第一个目标，待移动方块再吸附到 y=165，验证候选
/// 不局限于最底层目标并可形成多层组合。
bool testStackingSnapLevelsCanCombine()
{
    // 两个目标共享 x，但处于不同堆叠高度。
    const Rect              canvas{ 0.0F, 0.0F, 400.0F, 360.0F };
    const std::vector<Rect> targets{
        Rect{ 100.0F, 80.0F, 100.0F, 100.0F },
        Rect{ 100.0F, 115.0F, 100.0F, 100.0F },
    };
    MMM::UI::ProjectAudioToolLayout::SnapLocks locks;
    // 原始 y=164 距组合目标层级一像素。
    const auto snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 101.0F, 164.0F, 100.0F, 100.0F },
        canvas,
        targets,
        4.0F,
        12.0F,
        locks);
    return near(snapped.x, 100.0F) && near(snapped.y, 165.0F);
}

/// @brief 验证多个同尺寸目标都可吸附时优先使用最高图层。
///
/// 两个候选几乎重合且都在阈值内，后加入的矩形代表更高图层。结果必须跟随顶层的
/// x=102 与派生 y=113，而非底层的 x=100/y=115。
bool testStackingSnapUsesTopmostLayer()
{
    // targets 顺序同时表达图层从低到高。
    const Rect              canvas{ 0.0F, 0.0F, 400.0F, 300.0F };
    const std::vector<Rect> targets{
        Rect{ 100.0F, 80.0F, 100.0F, 100.0F },
        Rect{ 102.0F, 78.0F, 100.0F, 100.0F },
    };
    MMM::UI::ProjectAudioToolLayout::SnapLocks locks;
    const auto snapped = MMM::UI::ProjectAudioToolLayout::snapRect(
        Rect{ 101.0F, 114.0F, 100.0F, 100.0F },
        canvas,
        targets,
        4.0F,
        12.0F,
        locks);
    // 两轴都应来自同一个顶层目标，避免混合锚点造成视觉错位。
    return near(snapped.x, 102.0F) && near(snapped.y, 113.0F);
}

/// @brief 验证缩放活动边可通过自身边缘或中心吸附到目标锚点。
///
/// 第一场景拖动右边和下边，分别吸附目标左边与下边；第二场景只拖动左边并保持
/// 右边固定，验证尺寸随活动边移动而正确增长。
bool testResizeSnapping()
{
    // 目标提供 x=200 左边缘与 y=200 下边缘锚点。
    const Rect              canvas{ 0.0F, 0.0F, 400.0F, 300.0F };
    const std::vector<Rect> targets{
        Rect{ 200.0F, 100.0F, 100.0F, 100.0F },
    };
    MMM::UI::ProjectAudioToolLayout::SnapLocks locks;
    // Maximum/Maximum 表示右下角两条活动边参与吸附。
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
    // 结果边界和两轴锁定目标线必须同步报告 200。
    if ( !near(resized.right(), 200.0F) || !near(resized.bottom(), 200.0F) ||
         !locks.x.targetLine || !near(*locks.x.targetLine, 200.0F) ||
         !locks.y.targetLine || !near(*locks.y.targetLine, 200.0F) ) {
        return false;
    }

    // 清锁后单独验证 Minimum 水平边，不允许垂直方向改变。
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
    // 左边吸附到 100，同时原右边 300 保持不动。
    return near(resized.x, 100.0F) && near(resized.right(), 300.0F);
}

/// @brief 验证前景方块不能把任一下层方块遮挡到不足 35%。
///
/// 候选最初完全覆盖 base，约束函数必须沿可行方向推出，直到下层可见率恢复到
/// 0.35。断言重新用实际结果计算并集可见率。
bool testMinimumVisibleRatio()
{
    // base 同时作为唯一受保护下层方块。
    const Rect base{ 100.0F, 100.0F, 100.0F, 100.0F };
    const MMM::UI::ProjectAudioToolLayout::VisibilityConstraint constraint{
        .base = base,
    };
    const std::vector constraints{ constraint };
    // 完全重叠候选是最严重的单遮挡输入。
    const Rect result = MMM::UI::ProjectAudioToolLayout::constrainVisibility(
        Rect{ 100.0F, 100.0F, 100.0F, 100.0F },
        Rect{ 0.0F, 0.0F, 400.0F, 300.0F },
        constraints,
        0.35F);
    // 用约束结果作为唯一遮挡者重新验证公开 visibleRatio。
    const std::vector occluders{ result };
    return MMM::UI::ProjectAudioToolLayout::visibleRatio(base, occluders) >=
           0.35F - 1e-4F;
}

/// @brief 验证下层标签区域落在叠层后仍露出的部分。
///
/// 遮挡者覆盖 base 左侧 65%，最大可见单元应是右侧完整高度的 35x100 矩形。
bool testVisibleLabelCell()
{
    // 单个轴对齐遮挡形成唯一最大可见单元，避免平局歧义。
    const Rect        base{ 0.0F, 0.0F, 100.0F, 100.0F };
    const std::vector occluders{
        Rect{ 0.0F, 0.0F, 65.0F, 100.0F },
    };
    // largestVisibleCell 返回实际可放置标签的矩形而非仅面积。
    const Rect visible =
        MMM::UI::ProjectAudioToolLayout::largestVisibleCell(base, occluders);
    return near(visible.x, 65.0F) && near(visible.width, 35.0F) &&
           near(visible.height, 100.0F);
}

/// @brief 验证移动遮挡的增量标签区域会立即转移到仍可见的最大一侧。
///
/// 先验证遮挡存在时返回右侧可见条带，再把同一遮挡者移出 base，验证增量 helper
/// 不残留旧切分结果并恢复完整标签区域。
bool testIncrementalVisibleLabelCell()
{
    // 左侧 65% 遮挡留下右侧 35% 可见区域。
    const Rect base{ 0.0F, 0.0F, 100.0F, 100.0F };
    const Rect occluder{ 0.0F, 0.0F, 65.0F, 100.0F };
    const Rect visible =
        MMM::UI::ProjectAudioToolLayout::largestVisibleCellWithOneOccluder(
            base, occluder);
    if ( !near(visible.x, 65.0F) || !near(visible.width, 35.0F) ||
         !near(visible.height, 100.0F) ) {
        return false;
    }

    // 移出 base 后交集为空，结果应恢复 base 全矩形。
    const Rect movedAway{ 150.0F, 0.0F, 65.0F, 100.0F };
    const Rect restored =
        MMM::UI::ProjectAudioToolLayout::largestVisibleCellWithOneOccluder(
            base, movedAway);
    return near(restored.x, base.x) && near(restored.y, base.y) &&
           near(restored.width, base.width) &&
           near(restored.height, base.height);
}

/// @brief 验证扩大前景方块时会停在下层方块的 35% 可见边界。
///
/// 从 50% 覆盖宽度向 100% 扩大，约束应截断到 65 像素，使 base 仍保留 35%。
bool testResizeVisibilityConstraint()
{
    // initial 与 proposed 只改变宽度，隔离缩放可见率约束。
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
    // 同时验证语义可见率与精确边界位置。
    return MMM::UI::ProjectAudioToolLayout::visibleRatio(base, occluders) >=
               0.35F - 1e-4F &&
           result.width > 64.9F && result.width < 65.1F;
}

/// @brief 验证预处理会剔除无关遮挡且候选方块仍按固定遮挡并集限制。
///
/// 预处理输入包含一个相交遮挡和一个远端无关矩形，缓存应只保留前者并计算 3000
/// 固定覆盖面积；随后新增候选仍不能突破剩余可见率下限。
bool testPreparedVisibilityConstraint()
{
    // 第二个固定遮挡完全位于 base 外，应在预处理阶段剔除。
    const Rect        base{ 100.0F, 100.0F, 100.0F, 100.0F };
    const std::vector fixedOccluders{
        Rect{ 100.0F, 100.0F, 30.0F, 100.0F },
        Rect{ 500.0F, 500.0F, 100.0F, 100.0F },
    };
    const auto constraint =
        MMM::UI::ProjectAudioToolLayout::prepareVisibilityConstraint(
            base, fixedOccluders);
    // 固定遮挡缓存数量和并集面积都必须正确。
    if ( constraint.fixedOccluders.size() != 1 ||
         !near(constraint.fixedCoveredArea, 3000.0F) ) {
        return false;
    }

    const std::vector constraints{ constraint };
    // 使用公开约束入口验证预处理结果能被后续移动算法消费。
    // 候选从固定遮挡右缘继续覆盖，约束需考虑二者并集。
    const Rect result = MMM::UI::ProjectAudioToolLayout::constrainVisibility(
        Rect{ 130.0F, 100.0F, 70.0F, 100.0F },
        Rect{ 0.0F, 0.0F, 600.0F, 600.0F },
        constraints,
        0.35F);
    return MMM::UI::ProjectAudioToolLayout::visibleRatioWithCandidate(
               constraint, result) >= 0.35F - 1e-4F;
}

/// @brief 验证旧布局已有轻微可见率缺口时不会让无关拖动反复尝试修复。
///
/// 基线固定遮挡已经比 65% 多一像素。远处候选不增加缺口时应返回零 deficit；新增
/// 右侧遮挡进一步恶化时才报告正缺口。
bool testExistingVisibilityDeficitIsBaseline()
{
    // 66% 固定遮挡建立一个允许保持但不能继续恶化的历史基线。
    const Rect        base{ 100.0F, 100.0F, 100.0F, 100.0F };
    const std::vector fixedOccluders{
        Rect{ 100.0F, 100.0F, 66.0F, 100.0F },
    };
    const auto constraint =
        MMM::UI::ProjectAudioToolLayout::prepareVisibilityConstraint(
            base, fixedOccluders);
    // 一个无交集候选与一个覆盖剩余右侧区域的候选形成对照。
    const Rect unrelatedCandidate{ 400.0F, 400.0F, 100.0F, 100.0F };
    const Rect worseningCandidate{ 166.0F, 100.0F, 34.0F, 100.0F };
    // 基线差额归零，只有新增遮挡造成的额外缺口才为正。
    return near(MMM::UI::ProjectAudioToolLayout::visibilityDeficit(
                    constraint, unrelatedCandidate, 0.35F),
                0.0F) &&
           MMM::UI::ProjectAudioToolLayout::visibilityDeficit(
               constraint, worseningCandidate, 0.35F) > 0.0F;
}

/// @brief 验证批量方块并集去重并在整体平移时共同遵守下层可见率。
///
/// 两个选中矩形重叠 50x100，并集面积应为 15000 而非简单求和 20000。整体移动
/// 接近受保护方块时，约束作用于平移后的并集，而不是逐块重复计算。
bool testBatchMoveVisibilityConstraint()
{
    // 两个选择矩形组成连续 150x100 的并集区域。
    const std::vector selectedRects{
        Rect{ 0.0F, 0.0F, 100.0F, 100.0F },
        Rect{ 50.0F, 0.0F, 100.0F, 100.0F },
    };
    const auto unionCells =
        MMM::UI::ProjectAudioToolLayout::buildUnionCells(selectedRects);
    // 累加离散 union cells，验证预处理已去除重叠面积。
    float unionArea = 0.0F;
    for ( const auto& cell : unionCells ) {
        // union cells 互不重叠，可直接求和得到几何并集面积。
        unionArea += MMM::UI::ProjectAudioToolLayout::area(cell);
    }
    if ( !near(unionArea, 15000.0F) ) return false;
    // 面积预处理失败时不继续测试平移约束，保持退出原因单一。

    // 右侧 100x100 方块作为唯一受保护的下层目标。
    const auto fixedConstraint =
        MMM::UI::ProjectAudioToolLayout::prepareVisibilityConstraint(
            Rect{ 200.0F, 0.0F, 100.0F, 100.0F }, std::vector<Rect>{});
    const std::vector constraints{ fixedConstraint };
    const Rect        initialBounds{ 0.0F, 0.0F, 150.0F, 100.0F };
    // proposed 把并集左边推到 x=200，约束应在到达前截停。
    const Rect result =
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

/// @brief 验证相机缩放后鼠标指向的逻辑画布坐标保持不变。
///
/// 在 DPI=2、滚动非零的场景计算缩放前后逻辑锚点，确保滚轮缩放围绕鼠标而非
/// 画布原点。正滚轮一步应同时采用固定 CAMERA_ZOOM_STEP。
bool testMouseAnchoredCameraZoom()
{
    // 常量分离便于直接复核屏幕坐标到逻辑坐标的换算。
    constexpr float DPI_SCALE = 2.0F;
    constexpr float POINTER_X = 100.0F;
    constexpr float POINTER_Y = 60.0F;
    constexpr float SCROLL_X  = 300.0F;
    constexpr float SCROLL_Y  = 180.0F;
    const auto result = MMM::UI::ProjectAudioToolLayout::zoomCameraAtPointer(
        1.0F, 1.0F, DPI_SCALE, SCROLL_X, SCROLL_Y, POINTER_X, POINTER_Y);
    // 缩放前逻辑锚点只受 DPI 影响。
    const float anchorBeforeX = (SCROLL_X + POINTER_X) / DPI_SCALE;
    const float anchorBeforeY = (SCROLL_Y + POINTER_Y) / DPI_SCALE;
    // 缩放后还需除以 camera zoom，滚动值应补偿倍率变化。
    const float anchorAfterX =
        (result.scrollX + POINTER_X) / (DPI_SCALE * result.zoom);
    const float anchorAfterY =
        (result.scrollY + POINTER_Y) / (DPI_SCALE * result.zoom);
    return near(result.zoom,
                MMM::UI::ProjectAudioToolLayout::CAMERA_ZOOM_STEP) &&
           near(anchorBeforeX, anchorAfterX) &&
           near(anchorBeforeY, anchorAfterY);
}

/// @brief 验证相机倍率上下限和画布原点附近的非负滚动约束。
///
/// 极大正负滚轮量分别撞到最大和最小倍率；鼠标位于原点附近时，最小倍率计算出的
/// 滚动不能变成负数。
bool testCameraZoomLimits()
{
    // 两个调用使用相同锚点，仅滚轮方向相反。
    // 一百步输入足以越过两端上限，结果必须由公共常量钳制。
    const auto maximum = MMM::UI::ProjectAudioToolLayout::zoomCameraAtPointer(
        1.0F, 100.0F, 1.0F, 0.0F, 0.0F, 50.0F, 50.0F);
    const auto minimum = MMM::UI::ProjectAudioToolLayout::zoomCameraAtPointer(
        1.0F, -100.0F, 1.0F, 0.0F, 0.0F, 50.0F, 50.0F);
    return near(maximum.zoom,
                MMM::UI::ProjectAudioToolLayout::MAXIMUM_CAMERA_ZOOM) &&
           near(minimum.zoom,
                MMM::UI::ProjectAudioToolLayout::MINIMUM_CAMERA_ZOOM) &&
           near(minimum.scrollX, 0.0F) && near(minimum.scrollY, 0.0F);
}

/// @brief 验证缩放滑条指定倍率时以可见画布中心保持相机锚点。
///
/// 与滚轮增量测试不同，本例直接指定 2.5 倍目标，验证滑条入口复用相同锚点保持
/// 公式并准确达到请求倍率。
bool testSliderCameraZoom()
{
    // DPI 为一时逻辑锚点公式可直接由滚动与指针坐标相加验证。
    constexpr float POINTER_X = 240.0F;
    constexpr float POINTER_Y = 160.0F;
    constexpr float SCROLL_X  = 120.0F;
    constexpr float SCROLL_Y  = 80.0F;
    // 指针代表调用方选择的可见画布中心。
    const auto result = MMM::UI::ProjectAudioToolLayout::zoomCameraToPointer(
        1.0F, 2.5F, 1.0F, SCROLL_X, SCROLL_Y, POINTER_X, POINTER_Y);
    const float anchorBeforeX = SCROLL_X + POINTER_X;
    const float anchorBeforeY = SCROLL_Y + POINTER_Y;
    // 缩放后的滚动补偿应使两个轴的逻辑中心都保持不变。
    const float anchorAfterX = (result.scrollX + POINTER_X) / result.zoom;
    const float anchorAfterY = (result.scrollY + POINTER_Y) / result.zoom;
    return near(result.zoom, 2.5F) && near(anchorBeforeX, anchorAfterX) &&
           near(anchorBeforeY, anchorAfterY);
}

}  // namespace

/// @brief 运行项目音频工具几何布局测试。
/// @return 全部通过返回零，否则返回首个失败案例的稳定编号。
int main()
{
    // 按功能从尺寸、吸附、可见率到相机变换依次执行，遇到首错立即返回。
    // 1 至 2 对应尺寸计算。
    if ( !testDefaultWidth() ) return 1;
    if ( !testControlDrivenMinimumSize() ) return 2;
    // 3 至 8 对应移动、堆叠与缩放吸附。
    if ( !testEdgeAndCenterSnapping() ) return 3;
    if ( !testStackingSnapAndHysteresis() ) return 4;
    if ( !testStackingSnapLevels() ) return 5;
    if ( !testStackingSnapLevelsCanCombine() ) return 6;
    if ( !testStackingSnapUsesTopmostLayer() ) return 7;
    if ( !testResizeSnapping() ) return 8;
    // 9 至 15 对应单块、增量、预处理与批量可见率约束。
    if ( !testMinimumVisibleRatio() ) return 9;
    if ( !testVisibleLabelCell() ) return 10;
    if ( !testIncrementalVisibleLabelCell() ) return 11;
    if ( !testResizeVisibilityConstraint() ) return 12;
    if ( !testPreparedVisibilityConstraint() ) return 13;
    if ( !testExistingVisibilityDeficitIsBaseline() ) return 14;
    if ( !testBatchMoveVisibilityConstraint() ) return 15;
    // 16 至 18 对应滚轮、倍率边界与滑条相机缩放。
    if ( !testMouseAnchoredCameraZoom() ) return 16;
    if ( !testCameraZoomLimits() ) return 17;
    if ( !testSliderCameraZoom() ) return 18;
    // 所有场景均通过时返回标准成功码。
    return 0;
}
