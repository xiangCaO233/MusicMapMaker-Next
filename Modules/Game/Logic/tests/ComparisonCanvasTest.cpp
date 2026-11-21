/// @file ComparisonCanvasTest.cpp
/// @brief 同一谱面的多相机映射、独立快照和编辑事务回归。
/// @details 使用真实命令处理和同步缓冲，不要求 Vulkan、窗口焦点或音频设备。
/// 时刻和轨道由两种视觉映射换算；确定的 2 倍 SV 让映射误用可被精确观察。
/// 比较结束后独立关闭一个视口，另外两份状态仍可读取。
#include "common/ComparisonCanvas.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/TimingEffect.h"

#include <cmath>
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
/// 因而换算结果的差异只能由变速映射产生，不能归因于画布比例或 DPI。
/// 对照相机同时覆盖共用主映射与使用备用映射，测试分别检查这两种复用关系。
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
    ctx.currentBeatmap->m_baseMapMetadata.track_count = 4;
    ctx.trackCount                                    = 4;
    ctx.currentTime                                   = 1.0;
    ctx.animateTime                                   = 1.0;
    ctx.currentTool                                   = EditTool::Draw;
    auto config                                       = ctx.lastConfig;
    config.visual.trackLayout.left                    = 0.1F;
    config.visual.trackLayout.right                   = 0.5F;
    config.visual.judgeline_pos                       = 0.5F;
    config.visual.enableLinearScrollMapping           = false;
    config.visual.canvasThicknessCompensation         = 50.0F;
    config.settings.objectPlacementSnap               = false;
    // 固定缩放保证变速差来自时间线事件。关闭放置吸附使两种映射
    // 即使落在同一分拍网格附近也不会被吸到相同的时间点。
    config.visual.timelineZoom = 1.0F;
    const std::string main     = "Basic2DCanvas";
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
    auto linearVisual =
        std::make_shared<MMM::Config::VisualConfig>(config.visual);
    linearVisual->trackLayout.left  = .55F;
    linearVisual->trackLayout.right = .95F;
    linearVisual->judgeline_pos     = .75F;
    auto tiltedVisual =
        std::make_shared<MMM::Config::VisualConfig>(config.visual);
    tiltedVisual->debugCanvasCameraAngleDegrees = 35.0F;
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
    // 所以同为变速的两个相机应复用一套时间分段和位置索引。
    const auto* mainCache   = SessionUtils::scrollCacheForCamera(ctx, main);
    const auto* linearCache = SessionUtils::scrollCacheForCamera(ctx, linear);
    const auto* tiltedCache = SessionUtils::scrollCacheForCamera(ctx, tilted);
    if ( !mainCache || !linearCache || mainCache == linearCache ||
         mainCache != tiltedCache ||
         near(mainCache->getAbsY(1.5), linearCache->getAbsY(1.5)) ) {
        XERROR("Comparison views did not share the correct scroll caches");
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

    // 单独选择仅右移后的对比轨道才包含的 X，以排除主布局误命中的情况。
    // 即使画笔进入函数，旧全局左右边界也会拒绝这个位置，导致物件数不符。
    // 同步检查判定线的 Y 偏移，可以捕捉命中正确但时间映射采用旧值的回归。
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

    // 主设置改为线性，已有 3D 视图仍使用变速，且未关闭的标签继续共用数据。
    // 交换主缓存和备用缓存的职责，检查已建立视口不会随全局开关改值。
    // 切换后两套缓存继续复用稳定地址，避免渲染位置索引逐帧重建。
    config.visual.enableLinearScrollMapping = true;
    session.pushCommand(CmdUpdateEditorConfig{ config });
    session.update(0.0, config, true);
    // 比较稳定地址同时检验了复用路径，确保主映射切换没有为每个标签
    // 单独分配对应的时间线缓存。
    if ( SessionUtils::scrollCacheForCamera(ctx, linear) != mainCache ||
         SessionUtils::scrollCacheForCamera(ctx, tilted) != linearCache ||
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
