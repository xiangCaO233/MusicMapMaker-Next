#include "canvas/ObjectDragAutoPan.h"

#include <cmath>

namespace
{

/// @brief 使用小容差比较自动平移逻辑像素。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
///
/// 自动平移由多次浮点投影组合，测试不要求逐位相等。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 验证默认边缘自动平移速度已降低且仍保留渐进响应。
/// @return 边缘速度与中心静止行为符合预期时返回 true。
///
/// 用固定帧间隔和灵敏度隔离基础速度常量，左右端点还验证方向约定。
/// 中心采样同时确认热区之外不会产生残余位移。
bool testReducedEdgeSpeed()
{
    // 1000 像素视口得到 64 像素上限热区，便于覆盖完整边缘强度。
    constexpr float extent = 1000.0F;
    // sensitivity=1 保持基础速度，不掺入用户倍率。
    const float leftDelta = MMM::Canvas::objectDragAutoPanAxisDelta(
        // 起始边缘产生正向内容位移。
        0.0F,
        extent,
        1.0F / 60.0F,
        1.0F);
    const float rightDelta = MMM::Canvas::objectDragAutoPanAxisDelta(
        // 结束边缘产生等幅负向内容位移。
        extent,
        extent,
        1.0F / 60.0F,
        1.0F);
    const float centerDelta = MMM::Canvas::objectDragAutoPanAxisDelta(
        extent * 0.5F, extent, 1.0F / 60.0F, 1.0F);
    // 60 FPS 下两端速度应对称为 9 像素，中心位于热区外应静止。
    return near(leftDelta, 9.0F) && near(rightDelta, -9.0F) &&
           near(centerDelta, 0.0F);
}

/// @brief 验证横向自动平移止于草稿区和 BGM 区最外侧轨道边缘。
/// @return 两侧边缘均保留固定留白，已完整可见方向不再移动时返回 true。
///
/// 本用例先检查居中投影，再应用裁剪结果重建边界投影，覆盖计算闭环。
/// 两侧使用不同剩余距离，避免对称输入掩盖 BGM 边界计算错误。
/// 默认 48 像素冗余作为公开交互常量参与期望值计算。
bool testHorizontalTrackAreaBounds()
{
    // 投影同时启用草稿区和 BGM 区，覆盖左右两侧不同的外边界来源。
    const auto centered = MMM::Logic::calculateCanvasLaneProjection(
        // 四条玩家轨、两个 BGM 源最终形成可验证的三条 BGM 轨投影。
        1000.0F,
        4,
        2,
        0.3F,
        0.7F,
        0.0F,
        true,
        true,
        true);
    if ( !centered.valid || !near(centered.player.singleTrackWidth, 100.0F) ||
         centered.draftLaneCount != 4U || centered.bgmLaneCount != 3U ||
         !near(centered.draftLeftX, -100.0F) ||
         !near(centered.bgmRightX, 1026.0F) ) {
        // 先验证测试夹具投影，避免后续限制断言建立在错误几何上。
        return false;
    }

    // 从居中状态分别请求远大于剩余空间的左右位移，期望被精确裁剪。
    const float towardDraft =
        // 正位移将左侧草稿区向右带入视口。
        MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
            200.0F, 1000.0F, centered);
    const float towardBgm = MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
        // 负位移将右侧 BGM 区向左带入视口。
        -200.0F,
        1000.0F,
        centered);
    if ( !near(towardDraft, 148.0F) || !near(towardBgm, -74.0F) ) {
        return false;
    }

    // 把裁剪后的位移重新应用到相机，构造两侧恰好到达冗余边界的投影。
    const auto draftAtBoundary = MMM::Logic::calculateCanvasLaneProjection(
        // 应用 +148 后草稿区左边界恰好落在 48 像素安全线。
        1000.0F,
        4,
        2,
        0.3F,
        0.7F,
        148.0F,
        true,
        true,
        true);
    const auto bgmAtBoundary = MMM::Logic::calculateCanvasLaneProjection(
        // 应用 -74 后 BGM 右边界恰好落在 952 像素安全线。
        1000.0F,
        4,
        2,
        0.3F,
        0.7F,
        -74.0F,
        true,
        true,
        true);
    // 到达左边界后继续向左应为零；到达右边界后继续向右同样应为零。
    return near(draftAtBoundary.draftLeftX, 48.0F) &&
           // 已到草稿边界时拒绝继续正向位移。
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    20.0F, 1000.0F, draftAtBoundary),
                0.0F) &&
           near(bgmAtBoundary.bgmRightX, 952.0F) &&
           // 已到 BGM 边界时拒绝继续负向位移。
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    -20.0F, 1000.0F, bgmAtBoundary),
                0.0F);
}

/// @brief 验证任一侧轨道已完整可见时不会继续滚入空白区。
/// @return 两侧继续越界均被阻止，返回轨道区方向仍可移动时返回 true。
///
/// 两个相机偏移分别覆盖左侧与右侧已经越过安全线的镜像场景。
/// 每个场景同时验证禁止越界和允许返回两个方向。
/// 请求量保持在边界允许范围内，用于区分裁剪和完整保留。
bool testAlreadyVisibleSideDoesNotOverscroll()
{
    // 两个投影分别让草稿区和 BGM 区超过各自的安全可见边界。
    const auto draftVisible = MMM::Logic::calculateCanvasLaneProjection(
        // 正相机偏移让草稿区进入视口并超过左侧留白。
        1000.0F,
        4,
        2,
        0.3F,
        0.7F,
        200.0F,
        true,
        true,
        true);
    const auto bgmVisible = MMM::Logic::calculateCanvasLaneProjection(
        // 负相机偏移让 BGM 区进入视口并超过右侧留白。
        1000.0F,
        4,
        2,
        0.3F,
        0.7F,
        -200.0F,
        true,
        true,
        true);
    // 已完全可见的一侧禁止继续滚入空白，反方向仍保留原请求量以便返回。
    return draftVisible.valid && draftVisible.draftLeftX > 48.0F &&
           // 草稿区已充分可见时继续正移会增加左侧空白，应归零。
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    20.0F, 1000.0F, draftVisible),
                0.0F) &&
           // 反向负移会把内容拉回视口，保持完整请求。
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    -20.0F, 1000.0F, draftVisible),
                -20.0F) &&
           bgmVisible.valid && bgmVisible.bgmRightX < 952.0F &&
           // BGM 区已充分可见时继续负移会增加右侧空白，应归零。
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    -20.0F, 1000.0F, bgmVisible),
                0.0F) &&
           // 反向正移仍允许把轨道内容拉回。
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    20.0F, 1000.0F, bgmVisible),
                20.0F);
}

/// @brief 验证隐藏侧边轨道时以玩家区最外侧轨道作为边界。
/// @return 无草稿和 BGM 轨道时不会把玩家轨道自动滚入更大空白区。
///
/// 该用例防止边界逻辑在侧区关闭后仍使用无效的 draftLeftX 或 bgmRightX。
/// 玩家区已经完整可见，因此任一方向新增空白都应被阻止。
/// 显隐参数由投影输入直接控制，不依赖编辑器配置单例。
bool testHiddenSideAreasFallBackToPlayerEdges()
{
    // 显式隐藏两侧区域后，投影中不应残留草稿或 BGM 轨道数量。
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 0, 0.3F, 0.7F, 0.0F, true, false, false);
    // 玩家区已完整位于视口内，两个方向都没有可继续自动平移的轨道内容。
    return projection.valid && projection.draftLaneCount == 0U &&
           projection.bgmLaneCount == 0U &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    20.0F, 1000.0F, projection),
                0.0F) &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    -20.0F, 1000.0F, projection),
                0.0F);
}

}  // namespace

/// @brief 运行物件拖拽边缘自动平移回归测试。
/// @return 全部测试通过时返回 0。
///
/// 用例只固定公开几何契约，不绑定画布内部相机存储方式。
/// 失败时使用进程退出码交给 CTest 报告。
int main()
{
    // 速度曲线、双侧边界、返回方向与隐藏区域回退分别独立覆盖。
    // 所有测试使用纯投影结果，不需要创建画布或输入设备。
    return testReducedEdgeSpeed() && testHorizontalTrackAreaBounds() &&
                   testAlreadyVisibleSideDoesNotOverscroll() &&
                   testHiddenSideAreasFallBackToPlayerEdges()
               ? 0
               : 1;
}
