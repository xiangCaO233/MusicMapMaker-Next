#include "logic/BeatmapSyncBuffer.h"

#include "config/EditorConfig.h"
#include "config/VisualConfig.h"
#include "config/visual/InteractionHitboxConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/InteractionController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>

namespace
{
/// @brief 使用小容差比较包围盒浮点坐标。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
/// @note 使用绝对坐标容差；实体标识、部位和子节点索引仍要求精确相等。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 验证横纵缩放围绕包围盒中心展开且保留拾取元数据。
/// @return 几何与元数据均符合预期时返回 true。
bool testCenteredScale()
{
    // 矩形故意不是正方形，横纵倍率也不同，以检测宽高或倍率混用。
    // 实体仅作为元数据哨兵，不需要创建注册表或真实音符。
    const MMM::Logic::Hitbox source{
        static_cast<entt::entity>(42),
        MMM::Logic::HoverPart::HoldBody,
        3,
        100.0F,
        200.0F,
        10.0F,
        20.0F,
        MMM::Logic::ChartObjectKind::PlayerNote,
    };
    // 原中心为 (105,210)，放大后左上角必须向左上移动而非固定不变。
    const auto scaled = MMM::Logic::scaleInteractionHitbox(source, 3.0F, 2.0F);
    // 宽增加 20、高增加 20，左上角各减 10 才能保持中心不变。
    // 同一断言同时检查拾取身份，避免几何正确却把长条体误识别成头部。
    if ( !near(scaled.x, 90.0F) || !near(scaled.y, 190.0F) ||
         !near(scaled.w, 30.0F) || !near(scaled.h, 40.0F) ||
         scaled.entity != source.entity || scaled.part != source.part ||
         scaled.subIndex != source.subIndex || scaled.kind != source.kind ) {
        XERROR("Interaction hitbox did not scale around its center");
        return false;
    }
    return true;
}

/// @brief 验证无效缩放不会破坏原始拾取区域。
/// @return 非法横纵缩放均回退为 1 时返回 true。
/// @note 只覆盖负值和 NaN，不将本例当作所有非法浮点输入的完整测试。
bool testInvalidScaleFallback()
{
    // 使用不同于几何用例的起点尺寸，避免实现中的固定默认框恰好通过。
    const MMM::Logic::Hitbox source{
        static_cast<entt::entity>(7),
        MMM::Logic::HoverPart::Head,
        -1,
        12.0F,
        24.0F,
        30.0F,
        40.0F,
        MMM::Logic::ChartObjectKind::PlayerNote,
    };
    // 横向给负倍率，纵向给 NaN，分别覆盖数值范围错误和非有限值。
    // 回退应是单位倍率，而不是缩成零面积或翻转命中范围。
    const auto scaled = MMM::Logic::scaleInteractionHitbox(
        source, -2.0F, std::numeric_limits<float>::quiet_NaN());
    // 位置与大小全部保持，单独检查宽高会漏掉回退时意外偏移中心的问题。
    if ( !near(scaled.x, source.x) || !near(scaled.y, source.y) ||
         !near(scaled.w, source.w) || !near(scaled.h, source.h) ) {
        XERROR("Invalid interaction hitbox scale did not fall back to one");
        return false;
    }
    return true;
}
/// @brief 验证所有部件分别读取自己的倍率，原始几何和身份保持不变。
/// @return 各部件独立生效、全局倍率叠加且未知部件回退正确时返回真。
/// @note 非方形框和不同轴倍率用于捕捉索引串位，不模拟真实渲染器。
/// @pre 部件数组默认使用单位倍率，不依赖个人视觉配置或当前皮肤。
/// @post 只操作栈上值，没有创建实体、修改注册表或写入配置文件。
bool testComponentScales()
{
    using namespace MMM;
    Config::InteractionHitboxScales scales{};
    // 输入仍使用命令协议枚举，不能假定它与配置数组下标相同。
    // 映射错误会在对应样本的几何比较中暴露。
    const std::array parts{
        Logic::HoverPart::Head,         Logic::HoverPart::HoldBody,
        Logic::HoverPart::HoldEnd,      Logic::HoverPart::FlickArrow,
        Logic::HoverPart::PolylineNode, Logic::HoverPart::SampleAnchor,
        Logic::HoverPart::SampleOffset
    };
    // 每次只调整一类，其他部件仍须保留全局基础范围。
    // 同号实体的玩家／采样领域由元数据保留，不得在缩放时重新推断。
    for ( std::size_t selected = 0; selected < parts.size(); ++selected ) {
        // 清空上一组覆盖，避免多个部件同时放大掩盖映射错误。
        // 横向放大而纵向缩小，统一倍率无法通过本组断言。
        scales           = {};
        scales[selected] = { 2.0F, 0.5F };
        for ( std::size_t i = 0; i < parts.size(); ++i ) {
            // 非零坐标区分围绕原点缩放与围绕中心缩放。
            // 子索引、领域和触控令牌均是命令目标的一部分。
            const Logic::Hitbox source{ static_cast<entt::entity>(42),
                                        parts[i],
                                        3,
                                        100,
                                        200,
                                        20,
                                        40,
                                        Logic::ChartObjectKind::DraftNote,
                                        Logic::HoverPart::HoldStart,
                                        1234 };
            const auto          scaled =
                Logic::scaleInteractionHitbox(source, 1.5F, 2.0F, scales);
            // 全局倍率对所有部件生效，附加倍率只影响当前目标。
            // 期望宽高独立于生产 helper，避免测试复述实现公式。
            const float width  = i == selected ? 60 : 30;
            const float height = i == selected ? 40 : 80;
            // 只检查当前框不够：本轮未选中的六类也必须没有附加变化。
            // 这能发现误把某一组件设置传播成全局倍率的实现。
            // 中心守恒和完整身份同时检查，防止只改变可视化而漏改真正拾取。
            if ( !near(scaled.w, width) || !near(scaled.h, height) ||
                 !near(scaled.x + scaled.w / 2, 110) ||
                 !near(scaled.y + scaled.h / 2, 220) ||
                 scaled.entity != source.entity || scaled.part != source.part ||
                 scaled.subIndex != 3 || scaled.kind != source.kind ||
                 scaled.touchHandle != source.touchHandle ||
                 scaled.touchNodeToken != 1234 )
                return false;
        }
    }
    // 触屏专用起点复用头部设置；非法附加倍率不能污染另一轴。
    // 起点不能遗漏映射，也不能误用长条尾部设置。
    // 无穷与负数分别覆盖有限性回退和范围限幅两个入口。
    scales    = {};
    scales[0] = { std::numeric_limits<float>::infinity(), -2 };
    const Logic::Hitbox start{ static_cast<entt::entity>(7),
                               Logic::HoverPart::HoldStart,
                               -1,
                               0,
                               0,
                               20,
                               40 };
    const auto clamped = Logic::scaleInteractionHitbox(start, 2, 1, scales);
    // 未知协议部件不能成为数组索引，也不能借用头部的非法倍率。
    // 这项回退兼容比当前客户端更新的快照生产者。
    auto unknown = start;
    unknown.part = static_cast<Logic::HoverPart>(255);
    // 协议值可能来自更新版本，回退时仍需保留原样的身份与基础范围。
    // 未知值不允许触发按数组序号访问造成越界。
    const auto fallback = Logic::scaleInteractionHitbox(unknown, 2, 1, scales);
    // 回退后 X 保留全局二倍；负 Y 夹到四分之一，不能影响 X。
    // 真实 UI 边缘拾取另由输入帧测试验收，不以框存在作为通过标准。
    return near(clamped.w, 40) && near(clamped.h, 10) && near(fallback.w, 40) &&
           near(fallback.h, 40);
}
/// @brief 通过正式选择控制器验证捕捉倍率同时影响相交和严格框选。
/// @return 边缘相交、严格包含、全局叠加与长条体扩展均符合有效包围框时为真。
/// @note 直接设置框端点，鼠标输入分派另由画布表面回归覆盖。
/// @pre 使用固定线性滚动、无外部图集与四轨布局，不读取个人配置。
/// @post 只修改测试上下文，未写入谱面文件或撤销历史。
bool testMarqueeHitboxScales()
{
    // 该用例由 CTest 隔离进程执行，图集缺省回退不依赖其他测试的加载顺序。
    using namespace MMM;
    Logic::SessionContext ctx;
    ctx.currentBeatmap = std::make_shared<BeatMap>();
    ctx.currentBeatmap->m_baseMapMetadata.track_count = 4;
    ctx.trackCount                                    = 4;
    ctx.currentTime = ctx.animateTime = 1;
    // 相机与框选使用同一动画时刻，排除播放头滞后造成的几何误差。
    // 本例创建真实根组件，不用假命中记录代替领域对象。
    // 每轨宽一百像素；头部宽六十像素，中心位于第一轨的一百五十像素。
    // 刻意留出明显边缘空白，避免浮点容差让阴性样本也命中。
    auto& visual                             = ctx.lastConfig.visual;
    visual.trackLayout.left                  = .1F;
    visual.trackLayout.right                 = .5F;
    visual.judgeline_pos                     = .5F;
    visual.noteScaleX                        = .6F;
    visual.noteScaleY                        = .1F;
    ctx.lastConfig.settings.professionalMode = true;
    ctx.cameras.emplace("Basic2DCanvas",
                        // 固定视口把比例布局转换成确定的像素区域。
                        // 与框端点的 cameraId 一致，避免走无效相机早退。
                        Logic::CameraInfo{ "Basic2DCanvas", 1000, 600 });
    auto* cachePtr =
        ctx.timelineRegistry.ctx().find<Logic::System::ScrollCache>();
    if ( !cachePtr )
        cachePtr =
            &ctx.timelineRegistry.ctx().emplace<Logic::System::ScrollCache>();
    auto& cache = *cachePtr;
    // 不用手写像素速度替代缓存，保留默认 BPM 和时间单位换算。
    // 缓存创建方式不是本例约束，始终借用会话中实际安装的实例。
    cache.rebuild(
        ctx.timelineRegistry, ctx.lastConfig, ctx.currentBeatmap.get());
    const auto entity = ctx.noteRegistry.create();
    // 只创建一个根与交互标记，防止其他候选偶然命中使选择集合非空。
    // 后续检查具体实体，不仅检查选中数量。
    ctx.noteRegistry.emplace<Logic::NoteComponent>(
        entity, Logic::NoteComponent{ .m_timestamp = 1, .m_trackIndex = 0 });
    ctx.noteRegistry.emplace<Logic::InteractionComponent>(entity);
    // 同时提供排序实体和前缀终点，走生产时间索引而不是全量兜底。
    ctx.sortedNoteEntities     = { entity };
    ctx.sortedNoteMaxEndPrefix = { 1 };
    Logic::InteractionController controller(ctx);
    // 控制器跨样本复用，前一次选择必须经过正式重算清理。
    // 不替换注册表，也不直接修改选中标记模拟框选成功。
    /// @brief 按源平面像素定义选框，再通过正式时间和轨道坐标保存。
    /// @warning 仅测试构造，正式框选精筛仍由控制器和滚动缓存完成。
    const auto select = [&](float                 left,
                            float                 right,
                            float                 top,
                            float                 bottom,
                            Config::SelectionMode mode) {
        // 使用缓存反向换算，不能在测试中复制固定 BPM 的每秒像素公式。
        const double anchor = cache.getVisualAnchorAbsY(ctx.animateTime);
        ctx.marqueeBoxes    = { Logic::MarqueeBox{
            .startTime  = cache.getTime(anchor + 300 - top),
            .endTime    = cache.getTime(anchor + 300 - bottom),
            .startTrack = (left - 100) / 100,
            .endTrack   = (right - 100) / 100,
            .cameraId   = "Basic2DCanvas" } };
        ctx.lastConfig.settings.selectionMode = mode;
        // 强制同步覆盖从已有选择清空再命中的过程。
        // 两种模式共享候选，只允许几何谓词改变命中结果。
        ctx.isMarqueeSelectionDirty = true;
        controller.updateMarqueeSelection(true);
        return ctx.selectedNoteEntities.contains(entity);
    };
    // 原框为 [120,180]；外侧小框只能与扩大后的 [90,210] 相交。
    // 严格模式则相反：覆盖原框的区域不足以覆盖放大后的完整范围。
    // 同一组同时验证放大后的拒绝路径，不能只是让所有候选更容易选中。
    for ( const bool expanded : { false, true } ) {
        // 第一项验证边缘相交，第二项验证严格包含随范围扩大的反转。
        // 最后一个大框证明严格模式不能一律拒绝放大后的对象。
        // 连续三次查询不得借用上一项遗留的选择状态。
        visual.interactionHitboxComponents[0].x = expanded ? 2 : 1;
        if ( select(95, 105, 200, 400, Config::SelectionMode::Intersection) !=
                 expanded ||
             select(115, 185, 200, 400, Config::SelectionMode::Strict) ==
                 expanded ||
             !select(85, 215, 200, 400, Config::SelectionMode::Strict) ) {
            XERROR("Marquee did not follow component capture bounds");
            return false;
        }
    }
    // 单独恢复部件倍率并使用全局二倍，结果应与前面的部件二倍完全一致。
    visual.interactionHitboxComponents = {};
    visual.interactionHitboxScaleX     = 2;
    // 旧全局倍率和新部件倍率需要得到相同的有效几何。
    // 数组已恢复默认，模拟旧配置尚未自定义任何部件的情况。
    if ( !select(95, 105, 200, 400, Config::SelectionMode::Intersection) )
        return false;
    // 长条体纵向放大可延伸到原始终点之外，候选粗筛也必须同步扩大。
    // 一百秒载体配远端选框，能暴露沿用旧两秒索引余量造成的漏选。
    auto& hold      = ctx.noteRegistry.get<Logic::NoteComponent>(entity);
    hold.m_type     = NoteType::HOLD;
    hold.m_duration = 100;
    // 共享尾部 HS 排除独立尾部入场门禁对远端样本的干扰。
    // 这里定位候选区间的漏选，不把复杂 HS 生命周期当作本例通过条件。
    hold.m_metadata
        .note_properties[NoteMetadataType::MMM]["hold_independent_end_hs"] =
        "false";
    ctx.sortedNoteMaxEndPrefix   = { 101 };
    ctx.maxSelectionNoteDuration = 100;
    // 提供与生产索引一致的保守时长，查询时不扫描注册表填这个值。
    // 终点前缀仍是原持续时间，扩大的只能是查询窗口与交互矩形。
    visual.interactionHitboxScaleX = 1;
    const double anchor            = cache.getVisualAnchorAbsY(ctx.animateTime);
    const float  remoteY =
        300 - static_cast<float>(cache.getDisplayDelta(130, anchor, 1));
    // 单位倍率作为阴性对照；扩展后的载体才允许进入远端框。
    visual.interactionHitboxComponents[1].y = 1;
    if ( select(145,
                155,
                remoteY - 5,
                remoteY + 5,
                Config::SelectionMode::Intersection) )
        return false;
    visual.interactionHitboxComponents[1].y = 4;
    // 原谱面终点仍在一百零一秒，扩展后的交互体才进入一百三十秒。
    // 时间粗筛放行后仍须通过部件矩形精筛，不能直接将候选全部选中。
    return select(145,
                  155,
                  remoteY - 5,
                  remoteY + 5,
                  Config::SelectionMode::Intersection);
}
}  // namespace

/// @brief 覆盖交互拾取包围盒缩放的中心保持与非法值回退。
/// @return 全部断言通过时返回 0。
/// @note 这是包围盒变换测试，不运行鼠标拾取或真实图形绘制。
int main()
{
    // 两个用例独立构造只读输入，不依赖皮肤、相机状态或外部资源。
    // 先验收合法缩放，再检查非法倍率回退，保持现有短路失败顺序。
    // 非零退出码用于回归运行器，具体几何错误由用例日志定位。
    return testCenteredScale() && testInvalidScaleFallback() &&
                   testComponentScales() && testMarqueeHitboxScales()
               ? 0
               : 1;
}
