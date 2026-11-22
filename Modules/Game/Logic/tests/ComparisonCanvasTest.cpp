/// @file ComparisonCanvasTest.cpp
/// @brief 同一谱面的多相机缩放、映射、独立快照和编辑事务回归。
/// @details 使用真实命令处理和同步缓冲，不要求 Vulkan、窗口焦点或音频设备。
/// 时刻和轨道由两种视觉映射换算；确定的 2 倍 SV 让映射误用可被精确观察。
/// 比较结束后独立关闭一个视口，另外两份状态仍可读取。
#include "common/ComparisonCanvas.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "common/render/TextureID.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/metadata/MapMetadata.h"
#include "mmm/timing/TimingEffect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace
{
/// @brief 对时间和像素坐标使用统一的误差上限。
bool near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

/// @brief 同一会话并行发布主画布、线性对比和 3D 变速对比。
/// @details
/// 覆盖绘制坐标与撤销共享、窗口关闭，以及主设置变化后对比仍保有自身选项。
///
/// 初始化只建一个 ECS 和一个动作栈。相机使用相同尺寸、不同标识，
/// 因而换算结果的差异由变速映射及缩放产生，不能归因于 DPI。
/// 对照相机分别借用主积分和备用积分，以独立倍率投影渲染及拾取坐标。
/// 按相同时间锚点分别比较前向位置和逆向编辑落点，覆盖仅修改显示刻度、
/// 逻辑仍沿用主缓存而导致的错位。
///
/// 每个视口都须拿到自己的同步缓冲。验证透视厚度参数可以捕捉误用
/// 全局相机配置的回归，也可以捕捉 UI 声明有 3D、逻辑却只生成二维几何的情况。
///
/// 输入阶段走完整队列，映射选择必须贯穿命令消费、画笔落点和历史提交。
/// 发生误命中时，可能对象创建成功但时间错误，所以验证轨道坐标换算的产物，
/// 再从主视口撤销，证明窗口没有各自维护一份数据或历史。
///
/// 把主视图从变速切到线性会交换两套缓存的职责。对比配置在交换前后应固定；
/// 新的主视口和线性对比继续共用，3D 变速视图应转去使用备用映射。
///
/// 关闭与 Resize 故意连续入队。UI 释放纹理前可能已发出 Resize，验证乱序尾部
/// 不会恢复一个无窗口的相机，也不会释放原视口或另一对比仍在使用的同步缓冲。
/// 本测试不检查色彩、系统窗口事件或实际 GPU 像素，这些由 UI 验收覆盖。
/// @return 所有跨视口契约成立时返回零。
int runComparisonCanvasTest()
{
    using namespace MMM::Logic;
    BeatmapSession session;
    auto&          ctx = session.getContextMutable();
    // 只建立一个实际谱面，不为对比窗口单独创建 Session 或音频环境。
    // 固定轨道与判定线几何，输入时按目标视口缓存换算像素。
    ctx.currentBeatmap = std::make_shared<MMM::BeatMap>();
    // 锁定 osu! 滑条倍率，否则变速积分还会叠加格式默认的 1.4 倍。
    ctx.currentBeatmap->m_metadata
        .map_properties[MMM::MapMetadataType::OSU]
                       ["Difficulty::SliderMultiplier"] = "1";
    ctx.currentBeatmap->m_baseMapMetadata.track_count   = 4;
    ctx.trackCount                                      = 4;
    ctx.currentTime                                     = 1.0;
    ctx.animateTime                                     = 1.0;
    ctx.currentTool                                     = EditTool::Draw;
    auto config                                         = ctx.lastConfig;
    config.visual.trackLayout.left                      = 0.1F;
    config.visual.trackLayout.right                     = 0.5F;
    // 垂直留白使旧底板越出布局的情况也能被捕获。
    // 顶部不取零，防止错误复用旧高度时碰巧通过上沿检查。
    config.visual.trackLayout.top             = 0.05F;
    config.visual.trackLayout.bottom          = 0.95F;
    config.visual.judgeline_pos               = 0.5F;
    config.visual.enableLinearScrollMapping   = false;
    config.visual.canvasThicknessCompensation = 50.0F;
    config.settings.objectPlacementSnap       = false;
    // 主视口以单位倍率为参照；关闭放置吸附以便精确核对对比视口的逆映射。
    config.visual.timelineZoom            = 1.0F;
    config.visual.scrollAnimationDuration = 0.0F;
    const std::string main                = "Basic2DCanvas";
    const std::string prefix =
        main + std::string(MMM::Common::COMPARISON_CANVAS_SUFFIX);
    const auto linear = prefix + "1";
    const auto tilted = prefix + "2";

    // 同时间点保留两倍 SV，使相同像素向上绘制得到不同的谱面时刻。
    ctx.timelineRegistry.emplace<TimelineComponent>(
        ctx.timelineRegistry.create(),
        TimelineComponent{ .m_timestamp = 0.0,
                           .m_effect    = MMM::TimingEffect::SCROLL,
                           .m_value     = 2.0 });
    // 对比不仅使用不同映射，还验证轨道起点和判定线也能独立设置。
    // 以 50% 和 150% 分别覆盖比基准小及比基准大的比例。
    // 两者的视口宽高相同，从而区分纵向时间间距与按窗口高缩放的行为。
    // 每次配置发布后都保留同一份只读视觉对象，以覆盖 UI 的实际命令协议。
    auto linearVisual =
        std::make_shared<MMM::Config::VisualConfig>(config.visual);
    linearVisual->trackLayout.left  = .55F;
    linearVisual->trackLayout.right = .95F;
    linearVisual->judgeline_pos     = .75F;
    linearVisual->timelineZoom      = .5F;
    auto tiltedVisual =
        std::make_shared<MMM::Config::VisualConfig>(config.visual);
    tiltedVisual->debugCanvasCameraAngleDegrees = 35.0F;
    tiltedVisual->timelineZoom                  = 1.5F;
    // 使用隔离的底板 UV 区域，使检查只统计白色轨道线使用的平铺几何。
    // 无需实际皮肤资源，其他轮廓和拍线仍走普通快照生成，但不落在此 UV 内。
    // 两个视图使用同一图集条目，避免不同纹理坐标提前阻止错误复用。
    // 通过真实图集发布接口注入数据，保留快照的相机图集同步过程。
    auto&      engine = EditorEngine::instance();
    const auto trackTexture =
        static_cast<std::uint32_t>(MMM::Common::Render::TextureID::Track);
    engine.setAtlasUVMap(main,
                         { { trackTexture, { .25F, .5F, .25F, .025F } } });
    engine.setAtlasUVMap(tilted,
                         { { trackTexture, { .25F, .5F, .25F, .025F } } });
    // 经真实队列注册三种画布，验证同步缓冲使用各自相机 ID。
    // 主画布的命令不带覆盖参数，保持普通入口的注册语义。
    session.pushCommand(CmdUpdateViewport{ main, 1000.0F, 600.0F });
    session.pushCommand(CmdUpdateViewport{
        linear,
        1000.0F,
        600.0F,
        MMM::Common::ComparisonCanvasSettings{ .linearMapping = true,
                                               .visual = linearVisual } });
    session.pushCommand(CmdUpdateViewport{
        tilted,
        1000.0F,
        600.0F,
        MMM::Common::ComparisonCanvasSettings{ .visual = tiltedVisual } });
    session.update(0.0, config, true);

    // 线性视图的像素时间必须与变速图不同。3D 只改变观察矩阵，
    // 所以同为变速的两个相机复用时间分段和位置索引，各自投影为不同间距。
    // 这里同时检查源缓存身份，不允许为了独立缩放为每个标签重复做时间积分。
    // 线性对比同时关闭两倍 SV 并使用一半缩放，预期位置为主画布的四分之一。
    const auto* mainCache   = SessionUtils::scrollCacheForCamera(ctx, main);
    const auto* linearCache = SessionUtils::scrollCacheForCamera(ctx, linear);
    const auto* tiltedCache = SessionUtils::scrollCacheForCamera(ctx, tilted);
    if ( !mainCache || !linearCache || !tiltedCache ||
         &linearCache->integralSource() == mainCache ||
         &tiltedCache->integralSource() != mainCache ||
         !near(tiltedCache->getAbsY(1.5), mainCache->getAbsY(1.5) * 1.5) ||
         !near(linearCache->getAbsY(1.5), mainCache->getAbsY(1.5) * .25) ) {
        XERROR(
            "Comparison views did not use independent zoom over shared "
            "integrals");
        return 1;
    }

    // 发布独立投影视锥；三份快照的谱面与时间来自同一个会话。
    // 缩放和厚度都由独立快照生产；检查真实透视分支，而非仅检查设置值。
    // 主设置的补偿强度本身不会让二维对比视图进入透视路径。
    // 三份快照独立借用，只读当前轮结果。在后续 update 后不再访问这些指针，
    // 因为生产者可重新使用交换出去的槽位存储下一帧。
    const auto* mainSnap   = ctx.syncBuffers.at(main)->pullLatestSnapshot();
    const auto* linearSnap = ctx.syncBuffers.at(linear)->pullLatestSnapshot();
    const auto* tiltedSnap = ctx.syncBuffers.at(tilted)->pullLatestSnapshot();
    if ( !mainSnap || !linearSnap || !tiltedSnap || !mainSnap->hasBeatmap ||
         !linearSnap->hasBeatmap || !tiltedSnap->hasBeatmap ||
         mainSnap->thicknessCompensation.m_amount != 0.0F ||
         linearSnap->thicknessCompensation.m_amount != 0.0F ||
         tiltedSnap->thicknessCompensation.m_amount <= 0.0F ) {
        XERROR("Comparison view perspective leaked into another viewport");
        return 1;
    }

    /// @brief 通过底板的 UV 区域单独检查轨道线上下端。
    /// @details 底板向上多铺半像素用于遮缝，最后一段必须到达当前轨道上沿。
    /// 下沿保持原位，不能随缩放或透视延长一起移动。
    /// 只比较图元数无法捕捉整片底板错位，需要检查实际坐标。
    /// @return 找到底板且两端与指定视口的逻辑坐标一致时返回真。
    const auto trackFillsLayout =
        [](const auto& snapshot, const auto& visual, float height) {
            float top    = std::numeric_limits<float>::infinity();
            float bottom = -top;
            for ( const auto& vertex : snapshot.vertices ) {
                // 半像素内缩使轨道底板顶点落在已登记 UV 矩形的内部。
                if ( vertex.uv.u <= .25F || vertex.uv.u >= .5F ||
                     vertex.uv.v <= .5F || vertex.uv.v >= .525F )
                    continue;
                top    = std::min(top, vertex.pos.y);
                bottom = std::max(bottom, vertex.pos.y);
            }
            const auto layout = visual.trackLayoutForKeyCount(4);
            // 没有底板时极值保持无穷，范围检查也会失败。
            // 斜轨远端延长只改变上沿，CPU 逻辑坐标无需 GPU 投影即可验证。
            return std::abs(top - (height * (layout.top -
                                             visual.canvasCameraExtension()) -
                                   0.5F)) < 0.01F &&
                   std::abs(bottom - height * layout.bottom) < 0.01F;
        };
    if ( !trackFillsLayout(*mainSnap, config.visual, 600.0F) ||
         !trackFillsLayout(*tiltedSnap, *tiltedVisual, 600.0F) ) {
        XERROR("Track geometry did not cover the initial viewport");
        return 1;
    }
    // 模拟纵向拆分 Dock。只改高度，水平位置、单轨宽、颜色和皮肤保持相同。
    // 旧缓存若只匹配横向键，会把 600px 高的底板带进缩小后的平面或斜轨视图。
    // 两个视图使用不同的新高度，不能以一份重建结果覆盖另一个视图。
    // 两次命令在同一轮更新处理，覆盖两个视图在同一生产线程交替访问缓存的情况。
    session.pushCommand(CmdUpdateViewport{ main, 1000.0F, 360.0F });
    session.pushCommand(CmdUpdateViewport{ tilted, 1000.0F, 420.0F });
    session.update(0.0, config, true);
    // 从各自的同步缓冲消费新快照，不借用测试前一轮的顶点指针。
    const auto* resizedMain = ctx.syncBuffers.at(main)->pullLatestSnapshot();
    const auto* resizedTilted =
        ctx.syncBuffers.at(tilted)->pullLatestSnapshot();
    // 即使只回退一扇窗口的底板，也要使整个多视图用例失败。
    if ( !resizedMain || !resizedTilted ||
         !trackFillsLayout(*resizedMain, config.visual, 360.0F) ||
         !trackFillsLayout(*resizedTilted, *tiltedVisual, 420.0F) ) {
        XERROR(
            "Resizing a viewport reused track geometry at the previous height");
        return 1;
    }

    // 单独选择仅右移后的对比轨道才包含的 X，以排除主布局误命中的情况。
    // 即使画笔进入函数，旧全局左右边界也会拒绝这个位置，导致物件数不符。
    // 同步检查判定线的 Y 偏移，可以捕捉命中正确但时间映射采用旧值的回归。
    // 输入使用一个有明显垂直间距的时间点。仅比较判定线会让错误倍率恰好通过。
    // 从线性视图起笔；时间由该视图的逆映射计算，动作进入同一历史。
    const double expectedTime = ctx.animateTime + 0.25;
    // 判定线在对比视口的 450 px。目标时间直接由线性积分向上投影，
    // 同一个屏幕位置若错误走变速积分，只会前进一半的时间。
    const float mouseY =
        450.0F - static_cast<float>(linearCache->getAbsY(expectedTime) -
                                    linearCache->getAbsY(ctx.animateTime));
    session.pushCommand(CmdStartBrush{
        .cameraId = linear, .mouseX = 600.0F, .mouseY = mouseY });
    session.pushCommand(CmdEndBrush{ .cameraId = linear });
    session.update(0.0, config, true);
    // 在唯一的谱面 Registry 检查，不能用对比局部快照保有的旧物件作证据。
    const auto notes = ctx.noteRegistry.view<NoteComponent>();
    if ( notes.size() != 1 ||
         !near(notes.get<NoteComponent>(*notes.begin()).m_timestamp,
               expectedTime) ) {
        XERROR("Comparison view brush did not use its own time projection");
        return 1;
    }
    // 一次 Undo
    // 必须能从主相机撤销对比落下的物件，排除投影正确却分裂了历史的情况。
    session.pushCommand(CmdUndo{ .cameraId = main });
    session.update(0.0, config, true);
    if ( notes.size() != 0 ) {
        XERROR("The main editor did not undo an edit from its comparison");
        return 1;
    }

    // 只改线性对比的缩放，验证主视口及其余对比保持原有可见范围。
    // 模拟滑块只发布一条零尺寸 CmdUpdateViewport，保持相机高度和水平位置。
    // 复制旧配置是为保留布局字段；不修改仍被相机借用的上一次发布对象。
    // 将线性倍率调到 200%，抵消主视口两倍 SV 的差异，便于精确比较绝对坐标。
    auto zoomedVisual =
        std::make_shared<MMM::Config::VisualConfig>(*linearVisual);
    zoomedVisual->timelineZoom = 2.0F;
    session.pushCommand(CmdUpdateViewport{
        .cameraId   = linear,
        .comparison = MMM::Common::ComparisonCanvasSettings{
            .linearMapping = true, .visual = zoomedVisual } });
    session.update(0.0, config, true);
    const double mainPosition   = mainCache->getAbsY(1.5);
    const double linearPosition = linearCache->getAbsY(1.5);
    const double tiltedPosition = tiltedCache->getAbsY(1.5);
    // 同时检查共享动画状态，排除一帧内先改主缩放再覆盖缓存的实现。
    if ( !near(linearPosition, mainPosition) ||
         !near(tiltedPosition, mainPosition * 1.5) ||
         !near(ctx.animatedTimelineZoom, 1.0) ) {
        XERROR("Changing one comparison zoom leaked into another view");
        return 1;
    }

    // 切换主映射和基准缩放，验证对比仍保持各自映射及绝对间距。
    // 此次切换会触发积分重建；对比倍率应重新除以主缓存的新基础值。
    // 测试禁用了主视口缩放过渡，用零时间推进即可得到目标倍率。
    // 两个旧视口分别重新绑定到原来相反的积分槽，继续持有自己的绝对倍率。
    const auto* alternate                   = &linearCache->integralSource();
    config.visual.enableLinearScrollMapping = true;
    config.visual.timelineZoom              = 3.0F;
    session.pushCommand(CmdUpdateEditorConfig{ config });
    session.update(0.0, config, true);
    // 相机投影保留稳定地址，积分源可以随主映射设置交换。
    // 单独验证主视口确实到达 300%，避免测试在缩放操作被忽略时假阳性。
    // 3D 视角沿用原视觉快照，切换时间映射也不应触发布局恢复。
    if ( &linearCache->integralSource() != mainCache ||
         &tiltedCache->integralSource() != alternate ||
         !near(linearCache->getAbsY(1.5), linearPosition) ||
         !near(tiltedCache->getAbsY(1.5), tiltedPosition) ||
         !near(mainCache->getAbsY(1.5), linearPosition * 1.5) ||
         ctx.cameras.at(tilted)
                 .comparison->visual->debugCanvasCameraAngleDegrees != 35.0F ) {
        XERROR("Main editor mapping override changed comparison view settings");
        return 1;
    }

    // 先关闭再投递一次迟到的 Resize，验证不能唤醒已释放的对比视口。
    session.pushCommand(
        CmdUpdateViewport{ .cameraId = linear, .closeComparison = true });
    session.pushCommand(CmdUpdateViewport{ linear, 100.0F, 100.0F });
    session.update(0.0, config, true);
    // 相机和同步缓冲都须移除，后者仍存在会导致窗口反复开关时积累资源。
    // 关闭发生在重新绑定以后，可同时检查一个投影销毁不会释放共享积分。
    if ( !ctx.cameras.contains(main) || !ctx.cameras.contains(tilted) ||
         ctx.cameras.contains(linear) || ctx.syncBuffers.contains(linear) ) {
        XERROR("Closing a comparison did not isolate its camera lifetime");
        return 1;
    }
    return 0;
}

}  // namespace

/// @brief 执行对比相机回归，不启动应用窗口或访问真实音频设备。
int main()
{
    return runComparisonCanvasTest();
}
