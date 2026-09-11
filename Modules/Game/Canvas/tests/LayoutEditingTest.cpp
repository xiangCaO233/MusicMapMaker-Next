#include "canvas/TrackLayoutEditing.h"
#include "common/CanvasComponentLayout.h"

#include <array>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace
{

/// @brief 判断两个布局浮点值是否足够接近。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 差值小于测试容差时返回 true。
///
/// 布局计算经过归一化和像素往返，使用容差避免二进制浮点尾差干扰断言。
/// 该 helper 只用于预期有限的结果，非有限值自然不会通过比较。
bool near(float lhs, float rhs)
{
    // 容差远小于最小布局跨度，不会掩盖可见的边界偏差。
    return std::abs(lhs - rhs) < 1e-6f;
}

/// @brief 判断布局是否满足轨道边界的全部不变量。
/// @param layout 待检查布局。
/// @return 四边范围、顺序和最小跨度均合法时返回 true。
///
/// 公共检查器供多个操作用例复用，保证每种编辑方式都维持同一合法域。
/// 它不检查具体默认位置，从而聚焦边界与跨度不变量。
bool isLegal(const MMM::Config::TrackLayout& layout)
{
    // 前半部分约束画布范围，后半部分约束横纵最小可编辑跨度。
    return layout.left >= 0.0f && layout.top >= 0.0f && layout.right <= 1.0f &&
           layout.bottom <= 1.0f &&
           layout.right - layout.left >=
               MMM::Canvas::TRACK_LAYOUT_MIN_SPAN - 1e-6f &&
           layout.bottom - layout.top >=
               MMM::Canvas::TRACK_LAYOUT_MIN_SPAN - 1e-6f;
}

/// @brief 验证越界和非有限配置会被规整为合法布局。
/// @return 规整结果合法时返回 true。
///
/// 四条边分别使用负越界、NaN、反向边界和正越界，覆盖逐字段回退。
/// 测试不固定未损坏字段的具体默认数值，只要求最终结果满足公共合法域，
/// 从而允许默认皮肤调整轨道初始位置而不削弱异常输入覆盖。
/// NaN 专门放在 top，确认字段级回退不会使整份布局恢复默认。
bool testSanitizeInvalidLayout()
{
    // 从默认布局开始只为取得完整值对象，随后覆盖每个待测字段。
    MMM::Config::TrackLayout layout;
    layout.left   = -2.0f;
    layout.top    = std::numeric_limits<float>::quiet_NaN();
    layout.right  = -1.0f;
    layout.bottom = 4.0f;
    // 本用例关注不变量；具体默认坐标由配置类型自己的默认值负责。
    return isLegal(MMM::Canvas::sanitizeTrackLayout(layout));
}

/// @brief 验证四条边在越过相邻边界或画布边缘时均会被限制。
/// @return 所有边界拖动结果保持合法时返回 true。
///
/// 每个句柄都被拖过对边或画布边缘，预期收敛到精确最小跨度。
/// 四次 resize 均从相同不可变起点执行，避免前一次边界变化影响下一句柄；
/// 横纵结果分别检查，确保限制没有错误应用到另一轴。
/// 每个结果还经 isLegal 复核，兼顾精确边界和整体不变量。
bool testBoundaryConstraints()
{
    // 默认布局提供稳定的对边位置，四次操作彼此独立从同一起点计算。
    const MMM::Config::TrackLayout start;
    const auto                     left = MMM::Canvas::resizeTrackLayout(
        start, MMM::Canvas::TrackLayoutDragHandle::Left, 0.95f);
    const auto top = MMM::Canvas::resizeTrackLayout(
        start, MMM::Canvas::TrackLayoutDragHandle::Top, 2.0f);
    const auto right = MMM::Canvas::resizeTrackLayout(
        start, MMM::Canvas::TrackLayoutDragHandle::Right, -1.0f);
    const auto bottom = MMM::Canvas::resizeTrackLayout(
        start, MMM::Canvas::TrackLayoutDragHandle::Bottom, -1.0f);
    // 先统一验证合法域，再逐轴确认不是任意合法尺寸而是最小跨度。
    return isLegal(left) && isLegal(top) && isLegal(right) && isLegal(bottom) &&
           near(left.right - left.left, MMM::Canvas::TRACK_LAYOUT_MIN_SPAN) &&
           near(top.bottom - top.top, MMM::Canvas::TRACK_LAYOUT_MIN_SPAN) &&
           near(right.right - right.left, MMM::Canvas::TRACK_LAYOUT_MIN_SPAN) &&
           near(bottom.bottom - bottom.top, MMM::Canvas::TRACK_LAYOUT_MIN_SPAN);
}

/// @brief 验证整体平移在触及四周时仍保持原始宽高。
/// @return 平移结果合法且尺寸不变时返回 true。
///
/// 两次超大位移分别把矩形推向左上和右下，覆盖四条画布边缘。
/// 输入位移远超可用范围，使结果必须由边界钳制决定；同时保留非方形尺寸，
/// 可识别实现是否把宽高交换或独立裁短。
/// 两次移动分别从 start 计算，不测试累积拖动误差。
bool testMovePreservesSize()
{
    // 起始宽 0.5、高 0.7，刻意使用非方形矩形以分辨横纵尺寸。
    MMM::Config::TrackLayout start;
    start.left              = 0.2f;
    start.top               = 0.1f;
    start.right             = 0.7f;
    start.bottom            = 0.8f;
    const auto movedTopLeft = MMM::Canvas::moveTrackLayout(start, -3.0f, -2.0f);
    const auto movedBottomRight =
        MMM::Canvas::moveTrackLayout(start, 4.0f, 5.0f);

    // 边缘位置与原始宽高同时断言，防止实现通过裁剪矩形“合法化”。
    return isLegal(movedTopLeft) && isLegal(movedBottomRight) &&
           near(movedTopLeft.left, 0.0f) && near(movedTopLeft.top, 0.0f) &&
           near(movedBottomRight.right, 1.0f) &&
           near(movedBottomRight.bottom, 1.0f) &&
           near(movedTopLeft.right - movedTopLeft.left, 0.5f) &&
           near(movedTopLeft.bottom - movedTopLeft.top, 0.7f) &&
           near(movedBottomRight.right - movedBottomRight.left, 0.5f) &&
           near(movedBottomRight.bottom - movedBottomRight.top, 0.7f);
}

/// @brief 验证整个轨道布局可按其边缘和中心吸附到像素目标线。
/// @return 横向边缘与纵向中心吸附后仍保持轨道宽高时返回 true。
///
/// 吸附器输出的是组件中心，本用例再通过轨道专用移动函数应用结果。
/// 横向选择候选右边缘，纵向选择候选中心，故一个操作同时覆盖两种目标部位；
/// 应用后的轨道仍必须完整位于归一化画布内。
/// 六像素与三像素偏差都位于八像素吸附阈值内。
bool testTrackLayoutSnapping()
{
    // 候选范围映射为 x=[200,600]、y=[100,400] 的像素矩形。
    MMM::Config::TrackLayout candidate;
    candidate.left                                         = 0.2f;
    candidate.top                                          = 0.2f;
    candidate.right                                        = 0.6f;
    candidate.bottom                                       = 0.8f;
    constexpr float                         viewportWidth  = 1000.0f;
    constexpr float                         viewportHeight = 500.0f;
    const MMM::Logic::CanvasComponentBounds candidateBounds{
        candidate.left * viewportWidth,
        candidate.top * viewportHeight,
        candidate.right * viewportWidth,
        candidate.bottom * viewportHeight,
    };
    // X 目标靠近右边缘，Y 目标靠近中心，分别触发边缘和中心吸附。
    const std::array<float, 1> targetX{ 606.0f };
    const std::array<float, 1> targetY{ 247.0f };
    const auto                 snap = MMM::Logic::snapCanvasComponentBounds(
        candidateBounds, targetX, targetY, 8.0f);
    const auto snapped = MMM::Canvas::moveTrackLayoutToPixelCenter(
        candidate, snap.center.x, snap.center.y, viewportWidth, viewportHeight);

    // 应用吸附只平移矩形，因此宽 0.4、高 0.6 必须保持不变。
    return snap.snappedX && snap.snappedY && near(snapped.right, 0.606f) &&
           near((snapped.top + snapped.bottom) * 0.5f, 0.494f) &&
           near(snapped.right - snapped.left, 0.4f) &&
           near(snapped.bottom - snapped.top, 0.6f);
}

/// @brief 验证宽度句柄只在阈值内吸附到最近的组件边缘。
/// @return 主轨道区与辅助区域都采用吸附坐标且越界目标被忽略时返回 true。
///
/// 用例先独立验证一维吸附，再把结果同时应用到主轨道和辅助区右边界。
/// 这样可确认吸附选择与两类布局的最小宽度约束职责没有混淆。
/// 阈值外样本和 NaN 目标还覆盖未吸附时保持原指针的规则。
bool testHorizontalResizeEdgeSnapping()
{
    // 固定 1000 像素视口，便于把像素目标精确换算为归一化边界。
    constexpr float viewportWidth = 1000.0F;
    // 同时放入远端、近端和非有限值，覆盖过滤与距离选择。
    // 200 与 700 确保远端不会抢占，402 与 405 用于比较最近距离。
    // NaN 必须被跳过，不能污染 bestDistance 或返回坐标。
    const std::array targets{
        200.0F, 402.0F, 405.0F, 700.0F, std::numeric_limits<float>::quiet_NaN(),
    };
    // 398 距 402 四像素且距 405 七像素，应选择更近的 402。
    // 393 距最近目标九像素，应处于八像素阈值之外并保持原坐标。
    const auto snapped =
        MMM::Canvas::snapHorizontalResizeEdge(398.0F, targets, 8.0F);
    const auto outside =
        MMM::Canvas::snapHorizontalResizeEdge(393.0F, targets, 8.0F);
    // 命中必须同时报告替换坐标和目标线，未命中不能改写指针。
    if ( !snapped.snapped || !near(snapped.position, 402.0F) ||
         !near(snapped.target, 402.0F) || outside.snapped ||
         !near(outside.position, 393.0F) ) {
        return false;
    }

    // 主轨道右边和辅助区右边共用吸附结果，再分别交给各自的合法化函数。
    // 吸附器仅选择像素坐标，不得绕过两类布局原有的最小宽度约束。
    // 玩家区左边固定在 0.2，右句柄吸附后应精确落到 0.402。
    MMM::Config::TrackLayout trackStart;
    trackStart.left  = 0.2F;
    trackStart.right = 0.6F;
    const auto track = MMM::Canvas::resizeTrackLayout(
        trackStart,
        MMM::Canvas::TrackLayoutDragHandle::Right,
        snapped.position / viewportWidth);
    // 辅助区保存 left+width，吸附后右边界应与玩家区完全相同。
    const MMM::Canvas::HorizontalRegionBounds regionStart{ 0.1F, 0.3F };
    const auto region = MMM::Canvas::resizeHorizontalRegion(
        regionStart,
        MMM::Canvas::HorizontalRegionDragHandle::Right,
        snapped.position / viewportWidth);
    // 两种缩放都必须保持对侧左边界，防止吸附退化为整体移动。
    return near(track.right, 0.402F) && near(region.right(), 0.402F) &&
           near(track.left, trackStart.left) &&
           near(region.left, regionStart.left);
}

/// @brief 验证判定线位置会被限制在画布范围内。
/// @return 有限值、越界值和非有限值均得到合法结果时返回 true。
///
/// 正常值应原样保留，负值与正越界分别贴边，NaN 回到编辑器默认高度。
/// 用例直接测试纯规范化函数，不掺入像素高度或轨道布局，因此失败能定位到
/// 判定线配置边界而非命中或渲染换算。
/// 默认回退值 0.85 与实际初始判定线位置保持一致。
bool testJudgmentLineConstraints()
{
    // 四项输入分别覆盖保持、下限、上限和非有限回退分支。
    return near(MMM::Canvas::sanitizeJudgmentLinePosition(0.42f), 0.42f) &&
           near(MMM::Canvas::sanitizeJudgmentLinePosition(-2.0f), 0.0f) &&
           near(MMM::Canvas::sanitizeJudgmentLinePosition(3.0f), 1.0f) &&
           near(MMM::Canvas::sanitizeJudgmentLinePosition(
                    std::numeric_limits<float>::quiet_NaN()),
                0.85f);
}

/// @brief 验证中心把手优先于边界且四边和判定线把手均可命中。
/// @return 命中结果符合预期时返回 true。
///
/// 固定 100 像素视口使默认归一化布局可直接换算成整数命中坐标。
/// 最后一项位于所有句柄半径外，验证不会返回最近但未命中的边。
/// 四条边的采样点避开角点，判定线采样点位于右边界专用位置，确保每个结果
/// 来自对应句柄分支而非优先级碰撞。
/// 命中半径分别使用边缘 4 像素与移动把手 8 像素。
bool testHandleHitTesting()
{
    // 默认布局和判定线位置与实际编辑器初始状态一致。
    const MMM::Config::TrackLayout layout;
    using Handle = MMM::Canvas::TrackLayoutDragHandle;
    // 中心点同时可能靠近窄矩形边缘，Move 必须具有最高优先级。
    return MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 50.0f, 50.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Move &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 20.0f, 30.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Left &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 40.0f, 5.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Top &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 80.0f, 70.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Right &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 60.0f, 95.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Bottom &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 80.0f, 85.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::JudgmentLine &&
           // 左上画布角不在默认轨道矩形的任何命中延长范围内。
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 2.0f, 2.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::None;
}

/// @brief 验证辅助区域只能横向移动和调整宽度。
/// @return 规整、左右缩放、移动及命中结果均符合预期时返回 true。
///
/// 辅助区允许位于视口之外，但宽度始终为正；纵向边界只参与命中测试。
/// 规整、缩放、移动和命中四个阶段使用同一初始边界，分别验证数据约束与
/// 像素交互，不要求辅助区拥有不存在的 top/bottom 配置字段。
/// 左右边缘在窄区域中优先于中心移动把手，防止缩放入口丢失。
bool testHorizontalRegionEditing()
{
    // 起点右边界为 0.6，左右缩放可以分别验证固定对边。
    using Bounds = MMM::Canvas::HorizontalRegionBounds;
    using Handle = MMM::Canvas::HorizontalRegionDragHandle;
    const Bounds start{ 0.2F, 0.4F };
    // 左右缩放各自固定对侧边界，移动只改变 X 且保持总宽度。
    const auto resizedLeft =
        MMM::Canvas::resizeHorizontalRegion(start, Handle::Left, 0.1F);
    const auto resizedRight =
        MMM::Canvas::resizeHorizontalRegion(start, Handle::Right, 0.9F);
    const auto moved   = MMM::Canvas::moveHorizontalRegion(start, -0.35F);
    const auto invalid = MMM::Canvas::sanitizeHorizontalRegionBounds(
        { std::numeric_limits<float>::quiet_NaN(), -2.0F });
    // 首组断言覆盖几何变换与非法值恢复，不依赖命中测试结果。
    if ( !near(resizedLeft.left, 0.1F) ||
         !near(resizedLeft.right(), start.right()) ||
         !near(resizedRight.left, start.left) ||
         !near(resizedRight.right(), 0.9F) || !near(moved.left, -0.15F) ||
         !near(moved.width, start.width) || !std::isfinite(invalid.left) ||
         invalid.width < 0.005F ) {
        return false;
    }

    // 纵向范围只用于命中，区域类型本身没有 Y、Top 或 Bottom 可写字段。
    // 三个有效点分别位于左边、右边和中心，最后一点越过顶部范围。
    return MMM::Canvas::hitTestHorizontalRegion(
               start, 20.0F, 80.0F, 20.0F, 50.0F, 100.0F, 3.0F, 6.0F) ==
               Handle::Left &&
           MMM::Canvas::hitTestHorizontalRegion(
               start, 20.0F, 80.0F, 60.0F, 50.0F, 100.0F, 3.0F, 6.0F) ==
               Handle::Right &&
           MMM::Canvas::hitTestHorizontalRegion(
               start, 20.0F, 80.0F, 40.0F, 50.0F, 100.0F, 3.0F, 6.0F) ==
               Handle::Move &&
           MMM::Canvas::hitTestHorizontalRegion(
               start, 20.0F, 80.0F, 40.0F, 5.0F, 100.0F, 3.0F, 6.0F) ==
               Handle::None;
}

/// @brief 验证组件锚点规整与边缘拖动会保持组件完整可见。
/// @return 锚点、边界和移动结果均合法时返回 true。
///
/// 第一阶段验证配置字段规整，第二阶段验证带实际文本尺寸的像素边界裁剪。
/// 移动输入故意落在画布外，最终组件应以自身测量宽高贴边，而不是仅把锚点
/// 夹到 `[0,1]` 后仍让文字的一半越出视口。
/// 颜色四通道同时覆盖 NaN、下越界、上越界和有效保持。
bool testCanvasComponentPlacement()
{
    // NaN 锚点回到中心，越界锚点和颜色通道分别夹到合法范围。
    MMM::Config::CanvasComponentPlacement invalid;
    invalid.anchorX = std::numeric_limits<float>::quiet_NaN();
    invalid.anchorY = 4.0f;
    invalid.color   = {
        std::numeric_limits<float>::quiet_NaN(), -1.0f, 2.0f, 0.5f
    };
    const auto sanitized =
        MMM::Logic::sanitizeCanvasComponentPlacement(invalid);
    // 先确认逐字段规整结果，避免后续移动断言掩盖输入修复回归。
    if ( !near(sanitized.anchorX, 0.5f) || !near(sanitized.anchorY, 1.0f) ||
         !near(sanitized.color[0], 1.0f) || !near(sanitized.color[1], 0.0f) ||
         !near(sanitized.color[2], 1.0f) || !near(sanitized.color[3], 0.5f) ) {
        return false;
    }

    MMM::Config::CanvasComponentPlacement placement;
    placement.visible = true;
    // 目标位于画布左下外侧，组件应以自身尺寸贴住两条边界。
    const auto moved = MMM::Logic::moveCanvasComponent(
        placement, -100.0f, 1000.0f, 800.0f, 600.0f, 140.0f, 27.0f);
    const auto bounds =
        MMM::Logic::canvasComponentBounds(moved, 800.0f, 600.0f, 140.0f, 27.0f);
    // contains 检查闭区间边界，确保贴边点仍属于组件命中范围。
    return near(bounds.left, 0.0f) && near(bounds.bottom, 600.0f) &&
           bounds.right <= 800.0f && bounds.top >= 0.0f &&
           bounds.contains(bounds.left, bounds.top);
}

/// @brief 验证组件边缘与中心会在阈值内独立吸附到最近横纵目标线。
/// @return 边缘、中心、阈值外和非有限目标均按预期处理时返回 true。
///
/// 四组夹具分别覆盖边缘吸附、中心吸附、同步组包围框和阈值外保持。
/// 横纵轴独立选择目标，任一轴没有候选时不应影响另一轴。
/// 候选数组还包含 NaN，确认冻结吸附目标来自多个组件时，单个无效测量值
/// 不会污染最近距离或另一轴的有效选择。
/// 同步组场景使用联合包围框，验证目标线相对组边缘计算。
bool testCanvasComponentSnapping()
{
    // 基础矩形中心为 (140,100)，四条边用于计算候选移动距离。
    const MMM::Logic::CanvasComponentBounds bounds{
        100.0f, 80.0f, 180.0f, 120.0f
    };
    const std::array<float, 3> edgeTargetsX{
        94.0f, 184.0f, std::numeric_limits<float>::quiet_NaN()
    };
    // NaN 目标必须跳过，184 与右边缘仅差 4 像素。
    const std::array<float, 2> edgeTargetsY{ 74.0f, 125.0f };
    const auto                 edgeSnap = MMM::Logic::snapCanvasComponentBounds(
        bounds, edgeTargetsX, edgeTargetsY, 8.0f);
    if ( !edgeSnap.snappedX || !edgeSnap.snappedY ||
         !near(edgeSnap.center.x, 144.0f) || !near(edgeSnap.center.y, 105.0f) ||
         !near(edgeSnap.targetX, 184.0f) || !near(edgeSnap.targetY, 125.0f) ) {
        return false;
    }

    // 中心目标直接替换中心，不改变原矩形宽高。
    const std::array<float, 1> centerTargetX{ 146.0f };
    const std::array<float, 1> centerTargetY{ 93.0f };
    const auto centerSnap = MMM::Logic::snapCanvasComponentBounds(
        bounds, centerTargetX, centerTargetY, 8.0f);
    if ( !centerSnap.snappedX || !centerSnap.snappedY ||
         !near(centerSnap.center.x, 146.0f) ||
         !near(centerSnap.center.y, 93.0f) ) {
        return false;
    }

    // 同步组件组使用联合包围框，只提供 X 目标以验证轴向独立性。
    const MMM::Logic::CanvasComponentBounds synchronizedGroupBounds{
        20.0f, 30.0f, 220.0f, 130.0f
    };
    const std::array<float, 1> groupTargetX{ 225.0f };
    const std::array<float, 0> noTargets;
    const auto groupSnap = MMM::Logic::snapCanvasComponentBounds(
        synchronizedGroupBounds, groupTargetX, noTargets, 8.0f);
    if ( !groupSnap.snappedX || groupSnap.snappedY ||
         !near(groupSnap.center.x, 125.0f) ||
         !near(groupSnap.center.y, 80.0f) ||
         !near(groupSnap.targetX, 225.0f) ) {
        return false;
    }

    // 两个目标均超出八像素阈值，结果必须保持原中心。
    const std::array<float, 1> distantTargetX{ 189.0f };
    const std::array<float, 1> distantTargetY{ 129.0f };
    const auto distantSnap = MMM::Logic::snapCanvasComponentBounds(
        bounds, distantTargetX, distantTargetY, 8.0f);
    return !distantSnap.snappedX && !distantSnap.snappedY &&
           near(distantSnap.center.x, 140.0f) &&
           near(distantSnap.center.y, 100.0f);
}

/// @brief 验证四角包围框会等比调整字号并保持对角点。
/// @return 命中、字号和移动后的中心均符合预期时返回 true。
///
/// 初始组件位于画布中央，测试先命中右下角，再把该角拖到相对固定左上角
/// 两倍宽高的位置。字号比例应翻倍，同时左上角保持不动，锚点随新包围框
/// 中心移动。这样可同时覆盖命中、字号计算和固定对角点三项职责。
/// 期望锚点使用归一化画布坐标，能够发现实现误把像素中心直接写入配置。
/// 句柄测试与缩放测试串联，确保动作使用真实命中语义。
bool testCanvasComponentResize()
{
    // 初始字号比例与 200x24 像素测量结果共同定义原始包围框。
    MMM::Config::CanvasComponentPlacement placement;
    placement.visible       = true;
    placement.anchorX       = 0.5f;
    placement.anchorY       = 0.5f;
    placement.fontSizeRatio = 0.04f;
    const auto bounds       = MMM::Logic::canvasComponentBounds(
        placement, 800.0f, 600.0f, 200.0f, 24.0f);
    // 操作前先确认右下角落在预期句柄命中半径内。
    if ( MMM::Logic::hitTestCanvasComponent(
             bounds, bounds.right, bounds.bottom, 6.0f) !=
         MMM::Logic::CanvasComponentDragHandle::BottomRight ) {
        return false;
    }

    // 指针相对左上固定点移动到原宽高两倍处，目标字号也应翻倍。
    const auto resized = MMM::Logic::resizeCanvasComponent(
        placement,
        MMM::Logic::CanvasComponentDragHandle::BottomRight,
        bounds,
        bounds.left + bounds.width() * 2.0f,
        bounds.top + bounds.height() * 2.0f,
        800.0f,
        600.0f);
    // 新锚点对应放大后包围框中心，而不是继续停留在画布中心。
    return near(resized.fontSizeRatio, 0.08f) &&
           near(resized.anchorX, 0.625f) && near(resized.anchorY, 0.52f);
}

/// @brief 验证物件包围框四角会以中心为固定点独立调整横纵缩放。
/// @return 横纵缩放、反向角点与上下限均符合预期时返回 true。
///
/// 音符渲染缩放不同于文本字号：X/Y 可以独立变化，并始终以包围框中心为
/// 固定点。用例分别拖动右下与左上角，再以极端距离触发最大和最小限制，
/// 防止某一轴错误复用另一轴比例或越过公开范围。
/// 四个结果均从相同 start 计算，避免连续缩放的累积误差影响边界断言。
/// 上下限直接引用公开常量，配置范围调整时测试仍保持契约一致。
bool testNoteRenderScaleResize()
{
    using Handle = MMM::Logic::CanvasComponentDragHandle;
    // 100x40 包围框中心明确，便于分别计算横纵拖动比例。
    const MMM::Logic::CanvasComponentBounds bounds{
        100.0f, 80.0f, 200.0f, 120.0f
    };
    const MMM::Logic::NoteRenderScale start{ 1.2f, 1.2f };
    // 右下角同时远离中心，两轴应从 1.2 一致放大到 1.8。
    const auto resized = MMM::Logic::resizeNoteRenderScale(
        start, Handle::BottomRight, bounds, 225.0f, 130.0f);
    if ( !near(resized.x, 1.8f) || !near(resized.y, 1.8f) ) {
        return false;
    }

    // 左上角横向远离、纵向靠近，验证两轴允许独立变化。
    const auto independent = MMM::Logic::resizeNoteRenderScale(
        start, Handle::TopLeft, bounds, 75.0f, 90.0f);
    if ( !near(independent.x, 1.8f) || !near(independent.y, 0.6f) ) {
        return false;
    }

    // 极端远点与中心附近点分别触发公开上下限。
    const auto clampedMaximum = MMM::Logic::resizeNoteRenderScale(
        start, Handle::BottomRight, bounds, 1000.0f, 1000.0f);
    const auto clampedMinimum = MMM::Logic::resizeNoteRenderScale(
        start, Handle::TopLeft, bounds, 149.0f, 99.0f);
    return near(clampedMaximum.x, MMM::Logic::NOTE_RENDER_MAX_SCALE) &&
           near(clampedMaximum.y, MMM::Logic::NOTE_RENDER_MAX_SCALE) &&
           near(clampedMinimum.x, MMM::Logic::NOTE_RENDER_MIN_SCALE) &&
           near(clampedMinimum.y, MMM::Logic::NOTE_RENDER_MIN_SCALE);
}

/// @brief 验证同步字号缩放会让其他组件沿用当前把手的固定对角点。
/// @return 其他组件字号变化后左上角保持不动且中心随尺寸移动时返回 true。
///
/// 同步组中的组件具有各自文本尺寸，不能直接复制当前组件中心。本用例以
/// BottomRight 句柄把字号翻倍，重新计算从属组件包围框，要求其左上固定、
/// 中心按新尺寸移动，从而验证同步传播保留的是句柄语义。
/// 区域坐标非全局画布状态，确保 helper 尊重传入区域原点和尺寸。
/// 重建包围框使用翻倍后的文本测量值，与目标字号变化保持一致。
bool testSynchronizedCanvasComponentResize()
{
    // 区域宽高和文本像素尺寸使用二次幂比例，减少期望值舍入。
    const MMM::Logic::CanvasComponentBounds region{
        0.0f, 0.0f, 1024.0f, 512.0f
    };
    MMM::Config::CanvasComponentPlacement placement;
    placement.visible       = true;
    placement.anchorX       = 0.375f;
    placement.anchorY       = 0.5f;
    placement.fontSizeRatio = 0.03125f;
    // 初始包围框是同步调整前固定对角点的基准。
    const auto startBounds = MMM::Logic::canvasComponentBoundsInRegion(
        placement, region, 160.0f, 32.0f);
    const auto resized = MMM::Logic::resizeCanvasComponentToFontSizeInRegion(
        placement,
        MMM::Logic::CanvasComponentDragHandle::BottomRight,
        startBounds,
        0.0625f,
        region);
    // 新像素尺寸与字号同步翻倍，用于重建真实结果包围框。
    const auto resizedBounds = MMM::Logic::canvasComponentBoundsInRegion(
        resized, region, 320.0f, 64.0f);
    const float startCenterX = (startBounds.left + startBounds.right) * 0.5f;
    const float resizedCenterX =
        (resizedBounds.left + resizedBounds.right) * 0.5f;
    // 左上两边不变且中心改变，才能证明实现没有仅修改字号字段。
    return near(resized.fontSizeRatio, 0.0625f) &&
           near(resizedBounds.left, startBounds.left) &&
           near(resizedBounds.top, startBounds.top) &&
           !near(resizedCenterX, startCenterX);
}

/// @brief 验证同步移动会给其他组件应用相同位移并保留相对间距。
/// @return 两个组件移动后的中心差与移动前一致时返回 true。
///
/// 两个组件使用不同宽度与不同横向锚点，但共享同一编辑区域。对二者应用
/// 相同像素偏移后，分别检查归一化锚点增量以及像素中心间距，确保同步移动
/// 不会因为组件尺寸不同而逐渐改变相对布局。
/// 纵向也检查相同增量，防止同步实现只处理常见的横向 KPS 排列。
/// 中心间距比锚点差更能反映不同组件宽度下的视觉相对位置。
bool testSynchronizedCanvasComponentMove()
{
    // 两个组件共享区域与 Y 锚点，但宽度和 X 锚点不同。
    const MMM::Logic::CanvasComponentBounds region{
        0.0f, 0.0f, 1024.0f, 512.0f
    };
    MMM::Config::CanvasComponentPlacement first;
    first.visible                                = true;
    first.anchorX                                = 0.25f;
    first.anchorY                                = 0.5f;
    MMM::Config::CanvasComponentPlacement second = first;
    second.anchorX                               = 0.625f;
    const auto firstBounds =
        MMM::Logic::canvasComponentBoundsInRegion(first, region, 160.0f, 32.0f);
    const auto secondBounds = MMM::Logic::canvasComponentBoundsInRegion(
        second, region, 240.0f, 32.0f);

    // 64x32 像素分别对应区域宽高的 1/16，预期锚点增量明确。
    constexpr float offsetX = 64.0f;
    constexpr float offsetY = 32.0f;
    const auto movedFirst   = MMM::Logic::moveCanvasComponentByOffsetInRegion(
        first, firstBounds, offsetX, offsetY, region);
    const auto movedSecond = MMM::Logic::moveCanvasComponentByOffsetInRegion(
        second, secondBounds, offsetX, offsetY, region);
    // 移动前后都按各自文本尺寸重建包围框，再比较中心间距。
    const auto movedFirstBounds = MMM::Logic::canvasComponentBoundsInRegion(
        movedFirst, region, 160.0f, 32.0f);
    const auto movedSecondBounds = MMM::Logic::canvasComponentBoundsInRegion(
        movedSecond, region, 240.0f, 32.0f);
    const float startCenterDistance =
        (secondBounds.left + secondBounds.right) * 0.5f -
        (firstBounds.left + firstBounds.right) * 0.5f;
    const float movedCenterDistance =
        (movedSecondBounds.left + movedSecondBounds.right) * 0.5f -
        (movedFirstBounds.left + movedFirstBounds.right) * 0.5f;
    // 两个锚点增加相同归一化量，中心距离应保持原值。
    return near(movedFirst.anchorX, 0.3125f) &&
           near(movedFirst.anchorY, 0.5625f) &&
           near(movedSecond.anchorX, 0.6875f) &&
           near(movedSecond.anchorY, 0.5625f) &&
           near(movedCenterDistance, startCenterDistance);
}

/// @brief 验证拍内组件移动和缩放不会越过所属整拍的垂直边界。
/// @return 垂直范围受整拍限制且横向仍可使用完整画布时返回 true。
///
/// 拍内文字的编辑区域只占画布中部纵向范围，但横向覆盖完整画布。用例先
/// 把组件移到区域右下外侧，再从正常状态极端放大；两种操作的最终包围框
/// 都必须被区域四边约束，而不是错误使用整幅画布高度。
/// 移动和缩放分别从原始 placement 计算，保证两种边界策略独立接受检验。
/// 最终断言使用实际包围框四边，而不是只检查锚点范围。
bool testBeatRelativeComponentConstraints()
{
    // 拍区域横跨画布宽度，但纵向只允许 y=[100,300]。
    const MMM::Logic::CanvasComponentBounds beatRegion{
        0.0f, 100.0f, 800.0f, 300.0f
    };
    MMM::Config::CanvasComponentPlacement placement =
        MMM::Config::DEFAULT_BEAT_NUMBER_PLACEMENT;
    placement.visible       = true;
    placement.anchorX       = 0.5f;
    placement.anchorY       = 0.5f;
    placement.fontSizeRatio = 0.1f;

    // 移动目标远超右下边界，组件应带着自身尺寸贴边。
    const auto moved = MMM::Logic::moveCanvasComponentInRegion(
        placement, 900.0f, 500.0f, beatRegion, 120.0f, 40.0f);
    const auto movedBounds = MMM::Logic::canvasComponentBoundsInRegion(
        moved, beatRegion, 120.0f, 40.0f);
    if ( !near(movedBounds.right, 800.0f) ||
         !near(movedBounds.bottom, 300.0f) ||
         movedBounds.left < beatRegion.left ||
         movedBounds.top < beatRegion.top ) {
        return false;
    }

    // 缩放目标同样远超区域，最大可用字号由四边共同限制。
    const auto startBounds = MMM::Logic::canvasComponentBoundsInRegion(
        placement, beatRegion, 200.0f, 20.0f);
    const auto resized = MMM::Logic::resizeCanvasComponentInRegion(
        placement,
        MMM::Logic::CanvasComponentDragHandle::BottomRight,
        startBounds,
        1200.0f,
        900.0f,
        beatRegion);
    const auto resizedBounds = MMM::Logic::canvasComponentBoundsInRegion(
        resized,
        beatRegion,
        startBounds.width() * (resized.fontSizeRatio / placement.fontSizeRatio),
        startBounds.height() *
            (resized.fontSizeRatio / placement.fontSizeRatio));
    // 使用最终字号推导像素尺寸，再对实际包围框逐边验证。
    return resizedBounds.left >= beatRegion.left &&
           resizedBounds.right <= beatRegion.right &&
           resizedBounds.top >= beatRegion.top &&
           resizedBounds.bottom <= beatRegion.bottom;
}

/// @brief 验证所有画布组件均可仅复位位置和尺寸。
/// @return 默认几何恢复、显示属性保留且 KPS 逐轨覆盖被清除时返回 true。
///
/// 复位只负责几何字段，不能清除显隐和用户颜色。测试通过统一 lambda 覆盖
/// 判定线时间、拍号、分拍线时间、KPS 与背景频谱；KPS 还预先创建逐轨覆盖
/// 和同步设置，确认复位会清理派生几何但保留用户选择的同步模式。
/// lambda 每次重新写入非默认值，使前一组件的复位结果不会降低后一项覆盖。
/// KPS 字号缓存归零也单独断言，避免残留同步值覆盖新默认几何。
bool testCanvasComponentPlacementReset()
{
    // 单一配置对象依次复位所有类型，验证操作不会遗留跨类型状态。
    MMM::Config::CanvasComponentLayoutConfig config;
    const auto                               verifyReset =
        [&](MMM::Config::CanvasComponentType             type,
            const MMM::Config::CanvasComponentPlacement& expected) {
            auto& placement = config.placement(type);
            // 为每类组件写入同一组非默认几何和用户显示属性。
            placement.visible       = true;
            placement.anchorX       = 0.91f;
            placement.anchorY       = 0.83f;
            placement.fontSizeRatio = 0.21f;
            placement.color         = { 0.2f, 0.3f, 0.4f, 0.5f };

            if ( type == MMM::Config::CanvasComponentType::Kps ) {
                // KPS 额外建立单轨覆盖与两个同步开关，覆盖其派生状态清理。
                auto& trackPlacement =
                    config.editablePlacement(type, 2, 4, 0.2f, 0.8f);
                trackPlacement.anchorX   = 0.72f;
                config.syncKpsTrackSizes = true;
                config.setSyncKpsTrackRelativePositions(true);
                config.synchronizeKpsTrackFontSize(0.08f);
            }

            // 复位后几何应匹配类型默认值，显隐与颜色仍保持用户设置。
            config.resetPlacementToDefault(type);
            const auto& reset = config.placement(type);
            return reset.visible && near(reset.anchorX, expected.anchorX) &&
                   near(reset.anchorY, expected.anchorY) &&
                   near(reset.fontSizeRatio, expected.fontSizeRatio) &&
                   near(reset.color[0], 0.2f) && near(reset.color[3], 0.5f);
        };

    // 五种可编辑组件均通过相同契约检查，减少遗漏新增类型的风险。
    const bool allPlacementsReset =
        verifyReset(MMM::Config::CanvasComponentType::JudgmentLineTime,
                    MMM::Config::DEFAULT_JUDGMENT_LINE_TIME_PLACEMENT) &&
        verifyReset(MMM::Config::CanvasComponentType::BeatNumber,
                    MMM::Config::DEFAULT_BEAT_NUMBER_PLACEMENT) &&
        verifyReset(MMM::Config::CanvasComponentType::BeatLineTime,
                    MMM::Config::DEFAULT_BEAT_LINE_TIME_PLACEMENT) &&
        verifyReset(MMM::Config::CanvasComponentType::Kps,
                    MMM::Config::DEFAULT_KPS_TOTAL_PLACEMENT) &&
        verifyReset(MMM::Config::CanvasComponentType::BackgroundSpectrum,
                    MMM::Config::DEFAULT_BACKGROUND_SPECTRUM_PLACEMENT);
    return allPlacementsReset && config.kpsTracks.empty() &&
           // KPS 单轨几何被清理，但同步模式仍保持调用前的选择。
           near(config.kpsTrackFontSizeRatio, 0.0f) &&
           config.syncKpsTrackSizes && config.syncKpsTrackRelativePositions &&
           !config.syncAllKpsComponentPositions;
}

/// @brief 验证两种 KPS 位置同步模式始终互斥。
/// @return 设置接口与冲突配置读取后均只保留一个模式时返回 true。
///
/// 先通过两个公开 setter 验证后启用者覆盖前者，再构造两个字段同时为 true
/// 的冲突 JSON。反序列化必须按稳定优先级只保留全组件同步，避免运行时出现
/// 两套位置传播规则同时修改同一逐轨 KPS 项。
/// 这同时覆盖正常 API 使用和需要兼容的外部/旧版配置两条输入路径。
/// 两次 setter 的顺序固定，明确验证“后启用者生效”的交互约定。
bool testKpsPositionSyncModeMutualExclusion()
{
    // 首先启用相对轨道同步，要求全组件同步自动关闭。
    MMM::Config::CanvasComponentLayoutConfig config;
    config.setSyncKpsTrackRelativePositions(true);
    if ( !config.syncKpsTrackRelativePositions ||
         config.syncAllKpsComponentPositions ) {
        return false;
    }

    // 再启用全组件同步，互斥关系应反向切换。
    config.setSyncAllKpsComponentPositions(true);
    if ( config.syncKpsTrackRelativePositions ||
         !config.syncAllKpsComponentPositions ) {
        return false;
    }

    // 手工构造旧版或外部工具可能写出的冲突持久化配置。
    nlohmann::json encoded                   = config;
    encoded["syncKpsTrackRelativePositions"] = true;
    encoded["syncAllKpsComponentPositions"]  = true;
    const auto decoded =
        encoded.get<MMM::Config::CanvasComponentLayoutConfig>();
    // 读取冲突时稳定选择全组件同步，避免结果依赖 JSON 字段遍历顺序。
    return !decoded.syncKpsTrackRelativePositions &&
           decoded.syncAllKpsComponentPositions;
}

/// @brief 验证画布组件布局配置可独立完成 JSON 往返。
/// @return 显隐、锚点、字号与颜色均保持时返回 true。
///
/// 源配置为三类文字、总 KPS 和单轨 KPS 设置互不相同的字段，然后序列化并
/// 重新解析。断言覆盖颜色、锚点、字号、同步模式、逐轨覆盖及未覆盖轨道的
/// 默认推导；最后关闭字号同步，确认持久化的逐轨值仍可独立读取。
/// `fontSizeUsesCanvasHeight` 版本标志也参与断言，防止新配置被下次读取误迁移。
/// 单轨 KPS 同时检查显式轨和默认轨，覆盖解析与派生两种来源。
bool testCanvasComponentConfigRoundTrip()
{
    // 为各组件选择显著不同的字段值，防止序列化键串线仍偶然通过。
    MMM::Config::CanvasComponentLayoutConfig source;
    auto& placement         = source.judgmentLineTime;
    placement.visible       = true;
    placement.anchorX       = 0.23f;
    placement.anchorY       = 0.76f;
    placement.fontSizeRatio = 0.08f;
    placement.color         = { 0.1f, 0.3f, 0.7f, 0.8f };
    // 拍号配置使用另一组锚点、字号和 RGBA。
    auto& beatNumber         = source.beatNumber;
    beatNumber.visible       = true;
    beatNumber.anchorX       = 0.91f;
    beatNumber.anchorY       = 0.34f;
    beatNumber.fontSizeRatio = 0.16f;
    beatNumber.color         = { 0.8f, 0.2f, 0.3f, 0.7f };
    // 分拍线时间作为第三类重复文字，必须拥有独立持久化字段。
    auto& beatLineTime         = source.beatLineTime;
    beatLineTime.visible       = true;
    beatLineTime.anchorX       = 0.42f;
    beatLineTime.anchorY       = 0.67f;
    beatLineTime.fontSizeRatio = 0.19f;
    beatLineTime.color         = { 0.2f, 0.9f, 0.6f, 0.75f };
    // 总 KPS 配置与单轨覆盖同时存在，覆盖基础值和派生值的合并。
    source.kps.visible       = true;
    source.kps.anchorX       = 0.61f;
    source.kps.anchorY       = 0.09f;
    source.kps.fontSizeRatio = 0.06f;
    source.kps.color         = { 0.9f, 0.8f, 0.2f, 0.85f };
    // 只覆盖第三轨，另一轨道稍后用于验证默认位置推导。
    auto& kpsTrack = source.editablePlacement(
        MMM::Config::CanvasComponentType::Kps, 2, 4, 0.2f, 0.8f);
    kpsTrack.anchorX         = 0.73f;
    kpsTrack.anchorY         = 0.24f;
    kpsTrack.fontSizeRatio   = 0.045f;
    source.syncKpsTrackSizes = true;
    source.setSyncKpsTrackRelativePositions(true);
    source.synchronizeKpsTrackFontSize(0.064f);

    // JSON 往返是被测边界，不直接比较内存布局或私有字段。
    const nlohmann::json encoded = source;
    const auto           decoded =
        encoded.get<MMM::Config::CanvasComponentLayoutConfig>();
    const auto& restoredTime         = decoded.judgmentLineTime;
    const auto& restoredBeat         = decoded.beatNumber;
    const auto& restoredBeatLineTime = decoded.beatLineTime;
    const auto  restoredKpsTrack     = decoded.resolvedPlacement(
        MMM::Config::CanvasComponentType::Kps, 2, 4, 0.2f, 0.8f);
    // 未覆盖的第二轨应从总 KPS 和轨道区间计算默认锚点。
    const auto defaultKpsTrack = decoded.resolvedPlacement(
        MMM::Config::CanvasComponentType::Kps, 1, 4, 0.2f, 0.8f);
    auto independentlyEditable = decoded;
    // 关闭字号同步后再次解析，确认显式保存的轨道字号没有被丢弃。
    independentlyEditable.syncKpsTrackSizes = false;
    const auto independentStoredKpsTrack =
        independentlyEditable.resolvedPlacement(
            MMM::Config::CanvasComponentType::Kps, 2, 4, 0.2f, 0.8f);
    const auto independentDefaultKpsTrack =
        independentlyEditable.resolvedPlacement(
            MMM::Config::CanvasComponentType::Kps, 1, 4, 0.2f, 0.8f);
    // 长断言按基础组件、KPS 覆盖、同步标志和独立解析四组依次检查。
    // 首项确认编码端写出新字号参照标志，后续读取才不会再次执行旧版迁移。
    return encoded.value("fontSizeUsesCanvasHeight", false) &&
           // 判定线时间完整检查显隐、锚点、字号和 RGBA 四通道。
           restoredTime.visible && near(restoredTime.anchorX, 0.23f) &&
           near(restoredTime.anchorY, 0.76f) &&
           near(restoredTime.fontSizeRatio, 0.08f) &&
           near(restoredTime.color[0], 0.1f) &&
           near(restoredTime.color[1], 0.3f) &&
           near(restoredTime.color[2], 0.7f) &&
           near(restoredTime.color[3], 0.8f) && restoredBeat.visible &&
           // 拍号字段使用独立期望值，确认没有错误复用判定线时间键。
           near(restoredBeat.anchorX, 0.91f) &&
           near(restoredBeat.anchorY, 0.34f) &&
           near(restoredBeat.fontSizeRatio, 0.16f) &&
           near(restoredBeat.color[0], 0.8f) &&
           near(restoredBeat.color[1], 0.2f) &&
           near(restoredBeat.color[2], 0.3f) &&
           near(restoredBeat.color[3], 0.7f) && restoredBeatLineTime.visible &&
           // 分拍线时间覆盖第三套重复文字配置及其透明度。
           near(restoredBeatLineTime.anchorX, 0.42f) &&
           near(restoredBeatLineTime.anchorY, 0.67f) &&
           near(restoredBeatLineTime.fontSizeRatio, 0.19f) &&
           near(restoredBeatLineTime.color[0], 0.2f) &&
           near(restoredBeatLineTime.color[1], 0.9f) &&
           near(restoredBeatLineTime.color[2], 0.6f) &&
           near(restoredBeatLineTime.color[3], 0.75f) && decoded.kps.visible &&
           // 总 KPS 基础配置在解析逐轨覆盖后仍应保持原值。
           near(decoded.kps.anchorX, 0.61f) &&
           near(decoded.kps.anchorY, 0.09f) &&
           near(decoded.kps.fontSizeRatio, 0.06f) &&
           near(decoded.kps.color[0], 0.9f) &&
           near(decoded.kps.color[3], 0.85f) &&
           // 源配置只建立一个显式轨道项，往返后数量不能膨胀。
           decoded.kpsTracks.size() == 1U &&
           // 第三轨保留显式锚点，并按同步字号采用统一 0.064 比例。
           near(restoredKpsTrack.anchorX, 0.73f) &&
           near(restoredKpsTrack.anchorY, 0.24f) &&
           near(restoredKpsTrack.fontSizeRatio, 0.064f) &&
           // 未覆盖轨道从四轨区间推导中心 0.425，而非复制第三轨位置。
           near(defaultKpsTrack.anchorX, 0.425f) &&
           near(defaultKpsTrack.fontSizeRatio, 0.064f) &&
           decoded.syncKpsTrackSizes && decoded.syncKpsTrackRelativePositions &&
           // 互斥模式往返后只保留逐轨相对位置同步。
           !decoded.syncAllKpsComponentPositions &&
           near(decoded.kpsTrackFontSizeRatio, 0.064f) &&
           // 关闭同步仅改变解析策略，显式轨道存储的字号仍保持。
           near(independentStoredKpsTrack.fontSizeRatio, 0.064f) &&
           // 默认轨道也继续继承持久化的统一字号基准。
           near(independentDefaultKpsTrack.fontSizeRatio, 0.064f);
}

/// @brief 验证旧版拍内字号比例迁移为固定画布参考比例。
/// @return 默认旧字号得到新默认尺寸且位置比例保持不变时返回 true。
///
/// 旧配置缺少 fontSizeUsesCanvasHeight 标志，且拍号与分拍线时间都使用旧版
/// 0.18 比例。迁移应分别映射到两类组件的新默认字号，同时原始 anchorX/Y
/// 必须原样保留，避免升级配置后文字位置漂移。
/// 两种组件共享旧值但得到不同新值，能够验证迁移按类型而非统一倍率处理。
/// 迁移测试不写入新版本标志，保持输入与真实旧配置一致。
bool testLegacyRepeatedTextSizeMigration()
{
    // 缺少新版本标志使反序列化进入旧字号比例迁移路径。
    const nlohmann::json legacy{
        // 两类重复文字使用相同旧比例，但新默认目标尺寸不同。
        { "beatNumber",
          { { "anchorX", 0.31f },
            { "anchorY", 0.44f },
            { "fontSizeRatio", 0.18f } } },
        { "beatLineTime",
          { { "anchorX", 0.27f },
            { "anchorY", 0.63f },
            { "fontSizeRatio", 0.18f } } },
    };
    // 解码后锚点必须保留，只有字号比例按组件语义迁移。
    const auto decoded = legacy.get<MMM::Config::CanvasComponentLayoutConfig>();
    // 拍号的新默认比例为 0.075，分拍线时间的新默认比例为 0.025。
    return near(decoded.beatNumber.anchorX, 0.31f) &&
           // 拍号横纵锚点逐项对照旧 JSON，防止迁移误重置位置。
           near(decoded.beatNumber.anchorY, 0.44f) &&
           near(decoded.beatNumber.fontSizeRatio, 0.075f) &&
           near(decoded.beatLineTime.anchorX, 0.27f) &&
           // 分拍线时间同样保留位置，仅替换旧字号解释。
           near(decoded.beatLineTime.anchorY, 0.63f) &&
           near(decoded.beatLineTime.fontSizeRatio, 0.025f);
}

}  // namespace

/// @brief 覆盖轨道、判定线与可选画布组件的布局编辑。
/// @return 所有检查通过时返回 0。
///
/// main 为每项职责分配稳定退出码，失败时无需日志即可定位具体用例。测试
/// 顺序先覆盖主轨道和辅助区，再覆盖组件编辑、同步、持久化与兼容迁移；
/// 吸附及音符缩放在末尾单独编号，避免与基础几何失败混淆。
/// 返回码属于测试诊断契约，不复用布尔短路导致的统一失败码。
/// 全部用例通过时才返回零，便于 CTest 精确报告目标成功。
/// 测试使用纯值对象和 JSON 内存往返，不读取用户配置或写入资源目录。
/// 因此可在隔离配置环境中稳定重复执行。
/// 新增布局职责时应分配新的唯一退出码并在此保持顺序。
int main()
{
    // 1：异常轨道布局规范化。
    if ( !testSanitizeInvalidLayout() ) {
        return 1;
    }
    // 2：四边缩放的最小跨度。
    if ( !testBoundaryConstraints() ) {
        return 2;
    }
    // 3：整体移动保持宽高。
    if ( !testMovePreservesSize() ) {
        return 3;
    }
    // 4：判定线归一化边界。
    if ( !testJudgmentLineConstraints() ) {
        return 4;
    }
    // 5：主轨道六类句柄命中。
    if ( !testHandleHitTesting() ) {
        return 5;
    }
    // 6：辅助区横向编辑与命中。
    if ( !testHorizontalRegionEditing() ) {
        return 6;
    }
    // 7：组件规整与基础缩放共享历史诊断码。
    if ( !testCanvasComponentPlacement() ) {
        return 7;
    }
    if ( !testCanvasComponentResize() ) {
        return 7;
    }
    // 8：跨组件同步字号缩放。
    if ( !testSynchronizedCanvasComponentResize() ) {
        return 8;
    }
    // 9：跨组件同步像素移动。
    if ( !testSynchronizedCanvasComponentMove() ) {
        return 9;
    }
    // 10：拍内组件区域约束。
    if ( !testBeatRelativeComponentConstraints() ) {
        return 10;
    }
    // 11：全部组件几何复位。
    if ( !testCanvasComponentPlacementReset() ) {
        return 11;
    }
    // 12：KPS 位置同步模式互斥。
    if ( !testKpsPositionSyncModeMutualExclusion() ) {
        return 12;
    }
    // 13：当前版本布局 JSON 往返。
    if ( !testCanvasComponentConfigRoundTrip() ) {
        return 13;
    }
    // 14：旧版重复文字字号迁移。
    if ( !testLegacyRepeatedTextSizeMigration() ) {
        return 14;
    }
    // 15：组件边缘与中心吸附。
    if ( !testCanvasComponentSnapping() ) {
        return 15;
    }
    // 16：主轨道整体吸附。
    if ( !testTrackLayoutSnapping() ) {
        return 16;
    }
    // 17：横向缩放句柄吸附。
    if ( !testHorizontalResizeEdgeSnapping() ) {
        return 17;
    }
    // 18：音符渲染双轴缩放。
    if ( !testNoteRenderScaleResize() ) {
        return 18;
    }
    return 0;
}
