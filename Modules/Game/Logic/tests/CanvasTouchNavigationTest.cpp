#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <cmath>
#include <memory>

namespace
{
/// @brief 归一化触点及滚动积分允许极小浮点误差，不能使用整数像素取整掩盖跳变。
bool near(double a, double b)
{
    return std::abs(a - b) < 1e-4;
}
/// @brief 建立无音频设备的四轨会话，保持真实滚速缓存与命令解释。
/// @details 非活动会话只推进模型，测试不能改变本机音频输出或用户工程。
/// 总时长在测试中显式设置，避免边界钳制吞掉错误位移。
/// 固定四轨且显式给出 BGM 轨数，使每次 Session 更新采用相同领域布局。
/// 谱面对象只服务坐标上下文，不加载或保存任何外部资源。
/// 现有缓存由测试继续使用，不能通过换掉缓存对象掩盖基准不一致。
/// 相机标识采用生产主画布格式，让命令进入真实主画布分支。
/// 相机宽高固定为逻辑像素，不依赖主机 DPI 或显示器大小。
void configureContext(MMM::Logic::SessionContext& context)
{
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count     = 4;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 1;
    context.trackCount                                        = 4;
    context.draftTrackCount                                   = 4;
    context.bgmTrackCount                                     = 1;
    // 固定布局使纵坐标只由缩放与平移决定，不混入改键布局恢复。
    context.lastConfig.visual.trackLayout.left  = .1f;
    context.lastConfig.visual.trackLayout.right = .5f;
    context.lastConfig.visual.judgeline_pos     = .8f;
    context.cameras.emplace(
        "Basic2DCanvas",
        MMM::Logic::CameraInfo{ "Basic2DCanvas", 1000, 600, 0 });
    auto* cache =
        context.timelineRegistry.ctx().find<MMM::Logic::System::ScrollCache>();
    // 与 Session 默认构造兼容，重复安装上下文会掩盖生产缓存生命周期。
    if ( !cache )
        cache = &context.timelineRegistry.ctx()
                     .emplace<MMM::Logic::System::ScrollCache>();
    cache->rebuild(context.timelineRegistry,
                   context.lastConfig,
                   context.currentBeatmap.get());
}
/// @brief 真实控制器验证双指锚点、连续定位和最终配置切换没有回跳。
/// @details 显示坐标从生产滚速缓存计算，不在测试中复制捏合求解公式。
/// 测试先张开再缩回，并让旧配置继续运行一轮来复现异步发布时序。
/// 随后采用最终配置，要求临时覆盖和积分基准在同一显示尺度下交接。
/// 不初始化音频设备，非活动会话只测试时间和几何，不驱动系统播放。
/// 两次缩放都在曲目中段进行，避免零秒钳制掩盖错误的锚点换算。
/// @return 锚点像素、历史与缓存生命周期全部符合契约时返回 true。
/// 第一轮放大同时平移，能发现先缩放后平移造成的双重位移。
/// 第二轮保持中点缩小，检查绝对倍率而非对上帧结果再叠乘。
/// 释放不立刻丢弃直接显示值，因为逻辑调度仍可能使用旧配置。
/// 旧配置与新配置分别执行一轮，模拟真实发布而不使用固定等待时长。
/// 显示锚点由一个固定逻辑时间代表，不重新拾取屏幕位置。
/// 缓存实际基准与目标倍率分别断言，防止临时比例被重复应用。
/// 全程检查连续定位结束状态，音频提交不应停留在拖动模式。
/// 本测试只验证视图导航，不允许向编辑撤销栈追加动作。
bool testDirectZoomKeepsCentroidAnchor()
{
    using namespace MMM::Logic;
    BeatmapSession session;
    auto&          context = session.getContextMutable();
    configureContext(context);
    // 描述符在夹具中已准备；不要让空音轨重建替换明确的可定位时长。
    context.isAudioTimelineDescriptorDirty = false;
    context.audioTimelineTotalTime         = 100;
    context.currentTime = context.animateTime = 10;
    context.lastConfig.visual.judgeline_pos   = .8f;
    context.isTransformDirty                  = false;
    auto  config                              = context.lastConfig;
    auto& cache = context.timelineRegistry.ctx().get<System::ScrollCache>();
    // 取中点下真实时间作为观察对象，后续只验证它显示在哪个像素位置。
    const double judgmentY = 600 * config.visual.judgeline_pos;
    const double anchorTime =
        cache.getTime(cache.getVisualAnchorAbsY(10) + judgmentY - 200);
    // 闭包每次读取当前缓存，不能捕获一份旧积分使所有断言固定不变。
    // 观察时间保持不变，正是手指中点应锁住的那个谱面位置。
    const auto anchorPixel = [&] {
        return judgmentY - (cache.getAbsY(anchorTime) -
                            cache.getVisualAnchorAbsY(context.currentTime));
    };
    // 直接调用正式控制器，命令处理的时间钳制与动画状态均保留。
    // 随后交给真实 Session 更新，覆盖调用顺序带来的缓存基准变化。
    PlaybackController controller(context);
    controller.handleCommand(CmdPanCanvas{ .cameraId       = "Basic2DCanvas",
                                           .deltaX         = 20,
                                           .deltaY         = 60,
                                           .viewportWidth  = 1000,
                                           .viewportHeight = 600,
                                           .timelineZoom   = 2,
                                           .anchorY        = 200 });
    // 张开两倍同时下移 60 像素，旧中点下的时间应落到新中点 260。
    // 导航中不置全谱面变换脏标记，也不重建已有积分。
    if ( !near(anchorPixel(), 260) || context.isTransformDirty ||
         cache.isDirty || !near(cache.getBaseZoom(), 1) ||
         !context.isSeekScrubbing ||
         !near(context.cameras.at("Basic2DCanvas").horizontalOffsetX, 20) ) {
        XERROR(
            "Direct pinch lost centroid or rebuilt the whole transform cache");
        return false;
    }
    controller.handleCommand(CmdPanCanvas{ .cameraId       = "Basic2DCanvas",
                                           .viewportWidth  = 1000,
                                           .viewportHeight = 600,
                                           .timelineZoom   = .5f,
                                           .anchorY        = 260 });
    // 缩回时仍使用同一中点，不能把两次绝对倍率相乘到已有缓存上。
    if ( !near(anchorPixel(), 260) || !near(context.animatedTimelineZoom, .5) )
        return false;
    controller.handleCommand(
        CmdPanCanvas{ .cameraId = "Basic2DCanvas", .finishNavigation = true });
    // 释放必须提交连续定位最终状态，但配置尚未采纳时不能清掉显示覆盖。
    if ( context.isSeekScrubbing || !context.m_pointerTimelineZoom )
        return false;
    // 零 dt 刻意不推进缓动，任何视觉一致性都必须来自正确的直接倍率。
    // 旧配置轮次允许继续显示，但不能提前把局部覆盖当作已经持久化的目标。
    // 后一轮只改变目标倍率，其他软件设置与谱面组件保持相同。
    // 两轮调用都不等待墙钟时间，发布时序由测试直接控制。
    session.update(0, config, false);
    // 即使当前轮仍拿着旧配置，本地视觉继续保持最后的直接操作结果。
    if ( !context.m_pointerTimelineZoom ||
         !near(context.animatedTimelineZoom, .5) )
        return false;
    // 最终配置只有一个字段改变，不用完整配置广播替代手势的局部发布。
    config.visual.timelineZoom = .5f;
    session.update(0, config, false);
    // 新配置可见后只重建一次积分并移除覆盖，释放前后锚点不能跳动。
    // 这一步使用真正 Session::update，能发现仅在控制器单测中看不到的回跳。
    // 只有配置采纳后覆盖才消失，导航本身不属于可撤销的谱面数据修改。
    const bool passed =
        !context.m_pointerTimelineZoom && !context.m_pointerZoomFinished &&
        near(cache.getBaseZoom(), .5) &&
        near(context.animatedTimelineZoom, .5) && near(anchorPixel(), 260) &&
        context.actionStack.getUndoStackSize() == 0;
    if ( !passed )
        XERROR("Final pinch config adoption changed displayed anchor: {}",
               anchorPixel());
    // 所有对象随局部会话销毁，不向后续测试留下活动谱面或全局设置。
    return passed;
}


}  // namespace
/// @brief 退出码由真实控制器及 Session 更新的最终投影契约决定。
int main()
{
    return testDirectZoomKeepsCentroidAnchor() ? 0 : 1;
}
