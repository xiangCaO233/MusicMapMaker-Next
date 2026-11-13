#include "PreviewNavigationProjection.h"
#include "common/LogicCommands.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace MMM::Logic
{
/// @brief 绑定实际显示谱面的预览导航；不借用鼠标悬浮或当前选中物件。
/// @details 轻点以快照时间和实际显示比例反投影，拖动以当前逻辑位置积分。
/// 捏合保持旧中点下的滚动位置，横向位移不会改变主画布轨道偏移。
/// @warning 高频输入仅查询已有 ScrollCache，禁止全量重建、锁或固定时长等待。
void PlaybackController::handlePreviewNavigation(const CmdPanCanvas& cmd)
{
    // 命令可在切谱后才被队列消费；实例不符时连收尾也不能碰新谱面。
    if ( !m_ctx.currentBeatmap ||
         cmd.previewInstanceId !=
             reinterpret_cast<std::uintptr_t>(m_ctx.currentBeatmap.get()) )
        return;
    // 收尾不要求旧窗口几何仍存在，但必须绑定同一谱面实例。
    // 零宽、零高或坏倍率不能阻止已经有效开启的 Scrub 结束。
    // 完成标记只影响预览覆盖，不清理主画布的指针缩放状态。
    // 这条命令不会创建物件历史，也不调用任何编辑工具。
    if ( cmd.finishNavigation ) {
        // 已显示的浏览位置保留；取消只阻止额外落点，不用 Undo 回滚导航。
        m_ctx.m_pointerPreviewFinished = true;
        if ( m_ctx.isSeekScrubbing )
            handleCommand(CmdSeek{ .time = m_ctx.currentTime });
        return;
    }
    if ( m_ctx.composeLessonInputMode == ComposeLessonInputMode::AutoPlayback ||
         !std::isfinite(cmd.previewAreaRatio) || cmd.previewAreaRatio < 1 ||
         cmd.previewAreaRatio > 50 || !std::isfinite(cmd.viewportHeight) ||
         cmd.viewportHeight <= 0 || !std::isfinite(cmd.viewportWidth) ||
         cmd.viewportWidth <= 0 || !std::isfinite(cmd.deltaY) ||
         !std::isfinite(cmd.anchorY) )
        return;
    // 范围只影响预览的渲染压缩比例，ScrollCache 的目标缩放不变。
    // 全局视觉偏移在进入滚动距离前加回，在发布音频时间前减去。
    // 以实际预览高度和主画布有效高度建立比例，不能用固定像素每秒猜测。
    // 同一会话的连续命令按队列顺序处理，所以旧快照不会覆盖上一段平移。
    // 相机横向偏移和物件选区在整个处理过程中都不写入。
    // 自动演示、坏坐标与退化视口均在第一次索引查询前拒绝。
    // 缺少滚速缓存只拒绝本次输入，不在热路径临时重建全谱积分。
    const auto& config   = m_ctx.lastConfig;
    const float oldRatio = m_ctx.m_pointerPreviewAreaRatio.value_or(
        config.visual.previewConfig.areaRatio);
    const float oldScale =
        previewNavigationScale(m_ctx, config, cmd.viewportHeight, oldRatio);
    const float newScale = previewNavigationScale(
        m_ctx, config, cmd.viewportHeight, cmd.previewAreaRatio);
    // 滚动缓存未就绪时不猜测时间；下一笔真实输入可在缓存可用后正常开始。
    auto* cache = m_ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache || oldScale <= 0 || newScale <= 0 ) return;
    const double offset   = config.visual.getEffectiveVisualOffset();
    const double judgment = cmd.viewportHeight * config.visual.judgeline_pos;
    double       absolute{};
    if ( cmd.previewTap ) {
        // 单击与长期悬浮解耦；视觉补偿已经由 UI 从实际局部坐标扣除。
        // 仍通过正式滚速索引逆映射，保留变速、跳跃与边界语义。
        if ( !std::isfinite(cmd.previewVisualTime) ||
             !std::isfinite(cmd.renderScaleY) || cmd.renderScaleY <= 0 )
            return;
        absolute = cache->getAbsY(cmd.previewVisualTime) +
                   (judgment - cmd.anchorY) / cmd.renderScaleY;
    } else {
        // 缩放前后中点对应同一个谱面位置，平移使用新比例还原像素距离。
        // 连续命令依序更新会话，不能用滞后 UI 快照覆盖上一帧导航结果。
        absolute =
            cache->getAbsY(m_ctx.currentTime + offset) +
            (judgment - cmd.anchorY) * (1.0 / oldScale - 1.0 / newScale) +
            cmd.deltaY / newScale;
    }
    // 反投影由既有滚速索引决定，正负变速和 Jump 的歧义规则沿用原实现。
    // 参数有限不代表反算结果必然有限，因此在发布定位前还需最后校验。
    // Seek 继续拥有曲尾夹取、播放暂停偏好、音频和联机提交的语义。
    // 这里不直接调用音频设备，也不复制 Seek 的跨模块生命周期。
    // 动画时间只在成功定位之后对齐，以钳制后的真实当前时间为准。
    const double target = cache->getTime(absolute);
    if ( !std::isfinite(target) ) return;
    if ( !cmd.previewTap ) {
        // 只覆盖预览呈现，不触发全谱变换或写入主画布时间线倍率。
        m_ctx.m_pointerPreviewAreaRatio = cmd.previewAreaRatio;
        m_ctx.m_pointerPreviewFinished  = false;
    }
    handleCommand(
        CmdSeek{ .time = target - offset, .isScrubbing = !cmd.previewTap });
    // 浏览直接跟随手指；不叠加暂停态滚动动画，也不借此改变用户动画偏好。
    m_ctx.animateTime                = m_ctx.currentTime + offset;
    m_ctx.animateTimeTarget          = m_ctx.animateTime;
    m_ctx.animateTimeAnimationActive = false;
}
}  // namespace MMM::Logic
