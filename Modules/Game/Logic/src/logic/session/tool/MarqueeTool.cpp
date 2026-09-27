#include "logic/session/tool/MarqueeTool.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/SelectionState.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"

namespace MMM::Logic
{

/// @brief 开始一次框选，并以输入相机的坐标建立新的选择框。
/// @param ctx 保存相机、框选集合与选择状态的会话。
/// @param cmd 起点像素坐标、相机 ID 和追加选择修饰键。
/// @pre 视口、轨道宽度与预览缩放参数已由布局流程设置为有效值。
/// @note 框以时间和连续轨道坐标存储，而不是保存易随缩放失效的屏幕像素。
/// @details 此入口只建立框，不直接计算覆盖的物件集合。
/// @warning 开始手势可能清空已有选择并扩展框容器，仅在按下事件调用。
void MarqueeTool::handleStartMarquee(SessionContext&        ctx,
                                     const CmdStartMarquee& cmd)
{
    ctx.isSelecting         = true;
    ctx.hasMarqueeSelection = true;
    ctx.marqueeIsAdditive   = cmd.isCtrlDown;
    // 追加策略在按下时确定，移动过程不会因修饰键变化重置选择基线。

    // 普通框选替换旧结果；追加模式保留旧框与已有实体选择。
    if ( !ctx.marqueeIsAdditive ) {
        ctx.marqueeBoxes.clear();
        ctx.isMarqueeSelectionDirty = false;
        // 旧框已全部移除，旧的待计算标记也不能继续驱动选择更新。
        clearChartObjectSelection(ctx);
    }

    // 使用现有滚速缓存反查时间，不在输入处理时临时构建滚速数据。
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        auto it = ctx.cameras.find(cmd.cameraId);
        if ( it != ctx.cameras.end() ) {
            // 只有缓存和目标相机同时可用才能构造有意义的框选起点。
            MarqueeBox newBox;
            newBox.cameraId = cmd.cameraId;
            // 记录发起相机，拖动更新不能因鼠标经过其他画布而改变坐标解释。

            float judgmentLineY =
                it->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
            // 判定线位置由当前视口比例计算，不沿用主画布的像素坐标。

            float renderScaleY = 1.0f;
            if ( cmd.cameraId == "Preview" ) {
                // 预览区纵轴压缩取决于主画布有效高度和预览覆盖比例。
                const auto* mainCamera =
                    SessionUtils::findMainCanvasCamera(ctx.cameras);
                float mainViewportHeight = mainCamera
                                               ? mainCamera->viewportHeight
                                               : it->second.viewportHeight;
                // 主画布尚未注册时借用当前视口高度，保持坐标换算入口可用。

                float mainEffectiveH =
                    // 使用实际轨道布局高度，而不是把整个主窗口都当作绘图区。
                    (ctx.lastConfig.visual.trackLayout.bottom -
                     ctx.lastConfig.visual.trackLayout.top) *
                    mainViewportHeight;
                float ty = ctx.lastConfig.visual.previewConfig.margin.top;
                float by = it->second.viewportHeight -
                           ctx.lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;
                // 预览上下边距不属于可绘制高度，必须在计算压缩比例前扣除。

                renderScaleY = previewDrawH /
                               (mainEffectiveH *
                                ctx.lastConfig.visual.previewConfig.areaRatio);
                // areaRatio 表示预览覆盖的主画布高度倍数，放在分母中压缩显示。
            } else {
                // 主画布使用滚速缓存的原始纵向尺度，不套用预览压缩。
                renderScaleY = 1.0f;
            }

            double currentAbsY = cache->getAbsY(ctx.animateTime);
            // 锚点使用当前画面动画时间，框的起点应与用户实际看到的位置一致。
            double targetAbsY =
                currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
            // 屏幕向下而时间轴按判定线反向投影，先消除缩放再反查逻辑时间。
            newBox.startTime = cache->getTime(targetAbsY);
            // 使用滚速反向映射处理变速段，不能把像素差当成固定秒数。
            newBox.endTime = newBox.startTime;
            // 起始框没有面积，后续移动只改终点，保留首次按下的位置。

            const auto projection = calculatePlayerTrackProjection(
                // 横轴复用轨道投影，保证布局边距与画面拾取一致。
                it->second.viewportWidth,
                ctx.trackCount,
                ctx.lastConfig.visual.trackLayout.left,
                ctx.lastConfig.visual.trackLayout.right,
                SessionUtils::isMainCanvasCameraId(cmd.cameraId)
                    ? it->second.horizontalOffsetX
                    : 0.0F);
            // 起点不钳制在玩家区内，拖动框可以从轨道区外延伸进来。
            newBox.startTrack =
                // 保留小数轨道坐标，框选边缘不吸附到整条轨道。
                (cmd.mouseX - projection.leftX) / projection.singleTrackWidth;
            newBox.endTrack = newBox.startTrack;
            // 横纵两个终点均初始化为起点，尚无移动时可由结束入口识别为点击。

            ctx.marqueeBoxes.push_back(std::move(newBox));
            // 新框追加到末尾，更新和结束事件只处理这一活动框。
        }
    }
}

/// @brief 更新当前框的终点并请求重新计算选择结果。
/// @param ctx 当前框选所属会话。
/// @param cmd 拖动位置，按活动框记录的相机解释。
/// @pre 开始框选时的有效布局约束仍成立，尤其不能使用零轨宽或零纵向尺度。
/// @note 本方法不创建选择框，也不改变当前追加策略。
/// @warning 拖动期间可每个 update 调用；只更新框与脏标记，不在此遍历谱面物件。
void MarqueeTool::handleUpdateMarquee(SessionContext&         ctx,
                                      const CmdUpdateMarquee& cmd)
{
    // 开始阶段可能因缺少相机或缓存未能建立框，空集合不能作为有效拖动处理。
    if ( !ctx.isSelecting || ctx.marqueeBoxes.empty() ) return;
    // 松开之后即使仍有已完成的框，迟到的移动事件也不得改写它们。
    auto& currentBox = ctx.marqueeBoxes.back();
    // 先前完成的追加框保持不变，只有末尾框跟随当前指针。

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        auto it = ctx.cameras.find(currentBox.cameraId);
        // 相机消失时保留最后有效终点，不以另一画布的投影覆盖历史坐标。
        if ( it != ctx.cameras.end() ) {
            float judgmentLineY =
                it->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;

            float renderScaleY = 1.0f;
            if ( currentBox.cameraId == "Preview" ) {
                // 每次更新使用最新视口高度，拖动期间布局变化仍按当前画面换算。
                const auto* mainCamera =
                    SessionUtils::findMainCanvasCamera(ctx.cameras);
                float mainViewportHeight = mainCamera
                                               ? mainCamera->viewportHeight
                                               : it->second.viewportHeight;

                float mainEffectiveH =
                    (ctx.lastConfig.visual.trackLayout.bottom -
                     ctx.lastConfig.visual.trackLayout.top) *
                    mainViewportHeight;
                float ty = ctx.lastConfig.visual.previewConfig.margin.top;
                float by = it->second.viewportHeight -
                           ctx.lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;

                renderScaleY = previewDrawH /
                               (mainEffectiveH *
                                ctx.lastConfig.visual.previewConfig.areaRatio);
            } else {
                renderScaleY = 1.0f;
            }

            double currentAbsY = cache->getAbsY(ctx.animateTime);
            // 播放或滚动可推进画面锚点，终点必须随当前动画时间重新反查。
            double targetAbsY =
                currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
            currentBox.endTime = cache->getTime(targetAbsY);
            // 不交换起终点，保留手势方向；命中测试时再取最小/最大边界。

            const auto projection = calculatePlayerTrackProjection(
                // 仅主画布应用自己的横向平移，预览不继承该相机偏移。
                it->second.viewportWidth,
                ctx.trackCount,
                ctx.lastConfig.visual.trackLayout.left,
                ctx.lastConfig.visual.trackLayout.right,
                SessionUtils::isMainCanvasCameraId(currentBox.cameraId)
                    ? it->second.horizontalOffsetX
                    : 0.0F);
            currentBox.endTrack =
                (cmd.mouseX - projection.leftX) / projection.singleTrackWidth;
            // 横向同样保留浮点边界，不因跨越轨道中心而突然扩大选择范围。
            ctx.isMarqueeSelectionDirty = true;
            // 将多次移动合并到最新框数据，由选择更新流程消费失效标记。
        }
    }
}

/// @brief 结束拖动，丢弃近似点击的零面积框或确认有效框。
/// @param ctx 当前框选所属会话。
/// @param cmd 结束通知；终点使用最后一次更新保存的状态。
/// @note 判定阈值分别使用秒和轨道宽度单位，不依赖当前像素缩放。
/// @details 结束通知不再做坐标反查，因此必须先提交最后一次位置更新。
/// @note 这里只处理框选状态，不生成谱面编辑的撤销记录。
void MarqueeTool::handleEndMarquee(SessionContext&      ctx,
                                   const CmdEndMarquee& cmd)
{
    ctx.isSelecting = false;
    // 先关闭拖动状态，再处理框的有效性，避免后续输入继续修改已结束的框。
    if ( !ctx.marqueeBoxes.empty() ) {
        auto& lastBox = ctx.marqueeBoxes.back();
        // 两个方向都很小时才视为点击，单方向细长框仍允许保留。
        if ( std::abs(lastBox.endTime - lastBox.startTime) < 0.001 &&
             std::abs(lastBox.endTrack - lastBox.startTrack) < 0.1 ) {
            // 绝对差值使正向与反向拖动使用同一点击容差。
            ctx.marqueeBoxes.pop_back();
            // 只移除这次手势创建的末尾框，不丢弃先前的追加选择框。
            if ( ctx.marqueeBoxes.empty() ) {
                // 没有剩余框时停止框选计算，避免空集合触发无意义刷新。
                ctx.hasMarqueeSelection     = false;
                ctx.isMarqueeSelectionDirty = false;
            } else {
                // 仍有其他框时重新计算它们的并集，不沿用刚移除框贡献的选择。
                ctx.isMarqueeSelectionDirty = true;
            }
        } else {
            // 确认有效框时再标脏，确保最终选择结果覆盖最后一个手势状态。
            ctx.isMarqueeSelectionDirty = true;
        }
    }
}

/// @brief 删除指定点击位置下最后创建的同相机选择框。
/// @param ctx 保存所有选择框和实体选择集合的会话。
/// @param cmd 点击所在相机与像素坐标。
/// @pre 调用方提供当前相机局部像素坐标，而非整个应用窗口的屏幕坐标。
/// @note 未命中时不清理实体选择；只有成功移除框才改变选择结果。
/// @warning 用户移除操作会遍历选择框并清空选择，不应作为每帧悬浮测试调用。
void MarqueeTool::handleRemoveMarqueeAt(SessionContext&           ctx,
                                        const CmdRemoveMarqueeAt& cmd)
{
    // 没有可用投影时不能可靠命中框，保持原有选择状态。
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;
    // 框存储的是逻辑时间，没有滚速映射就无法从像素可靠恢复点击位置。

    auto it = ctx.cameras.find(cmd.cameraId);
    // 只从命令指定的相机取布局，不能以任意可用相机作为替代。
    if ( it == ctx.cameras.end() ) return;

    // 点击需转换到框保存的时间/轨道空间，不直接比较跨缩放的像素矩形。
    float judgmentLineY =
        it->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;

    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" ) {
        // 删除命中使用与创建、更新一致的预览尺度，避免可见框与命中区错位。
        const auto* mainCamera =
            SessionUtils::findMainCanvasCamera(ctx.cameras);
        float mainViewportHeight =
            mainCamera ? mainCamera->viewportHeight : it->second.viewportHeight;
        float mainEffectiveH = (ctx.lastConfig.visual.trackLayout.bottom -
                                ctx.lastConfig.visual.trackLayout.top) *
                               mainViewportHeight;
        // 高度只用于确定尺度，后续位置仍以当前视口自己的判定线为锚点。
        float ty = ctx.lastConfig.visual.previewConfig.margin.top;
        float by = it->second.viewportHeight -
                   ctx.lastConfig.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;
        // 可绘制高度排除预览边距，与绘制时压缩的有效范围保持一致。
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    } else {
        renderScaleY = 1.0f;
    }

    double currentAbsY = cache->getAbsY(ctx.animateTime);
    // 当前画面锚点抵消播放/滚动的位移，逻辑框本身无需随画面整体移动。
    double targetAbsY =
        currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
    double clickTime = cache->getTime(targetAbsY);
    // 保留完整时间精度，不先吸附到节拍线，否则窄框可能无法命中。

    const auto projection = calculatePlayerTrackProjection(
        // 主画布横移必须参与逆投影，否则点击会落到错误的逻辑轨道。
        it->second.viewportWidth,
        ctx.trackCount,
        ctx.lastConfig.visual.trackLayout.left,
        ctx.lastConfig.visual.trackLayout.right,
        SessionUtils::isMainCanvasCameraId(cmd.cameraId)
            ? it->second.horizontalOffsetX
            : 0.0F);
    const float clickTrack =
        (cmd.mouseX - projection.leftX) / projection.singleTrackWidth;
    // 点击轨道可以是小数，命中判断使用与框边界一致的连续坐标。

    // 后创建的框优先，重叠区域一次点击只移除最上层的一个框。
    for ( int i = static_cast<int>(ctx.marqueeBoxes.size()) - 1; i >= 0; --i ) {
        auto& box = ctx.marqueeBoxes[i];
        if ( box.cameraId != cmd.cameraId ) continue;
        // 不跨相机删除逻辑范围碰巧重叠的框，保留独立视口的框选归属。

        double minTime = std::min(box.startTime, box.endTime);
        // 起终点可能来自任意拖动方向，比较前将两个轴规范为闭区间。
        double maxTime  = std::max(box.startTime, box.endTime);
        float  minTrack = std::min(box.startTrack, box.endTrack);
        float  maxTrack = std::max(box.startTrack, box.endTrack);
        // 时间轴与轨道轴分别判定，只有同时位于范围内才命中矩形。

        if ( clickTime >= minTime && clickTime <= maxTime &&
             clickTrack >= minTrack && clickTrack <= maxTrack ) {
            // 边缘也算命中，避免细框只能从内部极窄区域移除。
            ctx.marqueeBoxes.erase(ctx.marqueeBoxes.begin() + i);
            // 仅删除命中的框，其他框的相对顺序保持不变，继续决定重叠时的优先级。

            // 不能只撤销该框曾选中的实体：多个框可能共同覆盖同一物件。
            // 清空旧结果后按剩余框重建，才能正确处理重叠与追加选择。
            if ( ctx.marqueeBoxes.empty() ) {
                // 最后一个框移除后退出追加模式，后续手势重新建立选择基线。
                ctx.hasMarqueeSelection     = false;
                ctx.marqueeIsAdditive       = false;
                ctx.isMarqueeSelectionDirty = false;
                // 无剩余框可以重建选择，直接清空实体索引而不是仅设置脏标记。
                clearChartObjectSelection(ctx);
            } else {
                // 重置追加模式，确保后续选择集合完整重建。
                // 若保留追加语义，已被移除框单独覆盖的物件会错误地继续选中。
                ctx.marqueeIsAdditive = false;
                clearChartObjectSelection(ctx);
                ctx.hasMarqueeSelection = true;
                // 保留剩余框，交给统一的选择更新流程重新投影所有物件类型。
                ctx.isMarqueeSelectionDirty = true;
            }
            // 删除会改变容器索引，命中后立即返回，不继续使用旧遍历位置。
            return;
        }
    }
    // 没命中任何同相机框时保持原状态，普通空白点击不隐式取消框选。
}

}  // namespace MMM::Logic
