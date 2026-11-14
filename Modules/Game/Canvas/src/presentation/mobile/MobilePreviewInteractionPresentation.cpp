#include "MobilePreviewGesture.h"
#include "canvas/PreviewDensityInteraction.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/VisualConfig.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "logic/EditorEngine.h"
#include "presentation/PreviewInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace MMM::Canvas
{
namespace
{
/// @brief 预览专用触控解释器，主画布和时间线不共享本对象状态。
/// @details 内容导航复用播放命令，密度轴直接映射全谱时间。
class MobilePreviewInteractionPresentation final
    : public PreviewInteractionPresentation
{
public:
    /// @brief 纯鼠标或经典方案返回原路径；触控始终吞掉合成鼠标输入。
    /// @warning 每帧固定数学和标量读取，不取得会话锁或复制完整配置。
    bool update(const PreviewInteractionFrame& frame) override
    {
        if ( !UI::Interaction::activeProfile().touchGestures() ) {
            suspend();
            return false;
        }
        const auto& raw = Graphic::mobileTouchSnapshot();
        if ( !m_railRunning ) m_densityTarget.reset();
        // 显示快照只借用到本次 update 返回，成员只记录其进程内实例令牌。
        // 同一路径重新加载也会换令牌，旧的密度轴目标不能落到新谱面。
        // 方案代数与谱面令牌独立；切回同名方案也不能恢复半次输入。
        // 中断帧检测覆盖隐藏后重开，不依赖平台一定补发 Up。
        // 每帧没有待处理队列，连续反馈随实际事件立即向逻辑层发布。
        const auto instance =
            frame.m_snapshot ? frame.m_snapshot->beatmapInstanceId : 0;
        const auto generation = UI::Interaction::profileGeneration();
        // 换谱、换方案和中断帧都只结束旧导航，不接续新的对象或补发轻点。
        if ( (m_instance && instance != m_instance) ||
             (m_generation && generation != m_generation) ||
             (m_frame && ImGui::GetFrameCount() > m_frame + 1) )
            suspend();
        m_generation = generation;
        m_frame      = ImGui::GetFrameCount();
        m_instance   = instance;
        // 缺失快照仍推进分类器，让已开始的导航走同一个结束边沿。
        // 没有合法实例时既不能开始轻点，也不能继续上一谱面的密度拖动。
        // 窗口遮挡和触点数由不同层处理，不能用鼠标焦点替代原始多指状态。
        // 内容几何包含边距后的实际渲染大小，标题和工具栏不参与时间反投影。
        // UI 的像素坐标与逻辑的秒时间只在明确命令边界转换。
        PreviewInteractionFrame input = frame;
        input.m_available &= instance && frame.m_snapshot->hasBeatmap;
        const auto& io = ImGui::GetIO();
        // 预览范围是轻量标量，读取不复制完整编辑器配置。
        // 原始触点数量有固定上限，不随谱面物件或时间线段数增长。
        // 颜色、文字、离屏对象和主画布选择都不在这个输入解释器里维护。
        // 快照为空时不能用之前一帧的 duration 继续计算目标。
        // 默认返回触控接管，避免经典分支在等待输入时误消费鼠标 Up。
        // 切换到经典方案的那一帧则明确返回 false，恢复原鼠标路径。
        // 这里没有定时器或阻塞等待，防抖只决定当前帧是否有导航增量。
        // 最终落点不在 UI 搜索全量滚速段，由播放控制器复用已有索引。
        // 同一位置多次停留不复制或排序任何谱面数据。
        auto&      visual = Config::AppConfig::instance().getVisualConfig();
        const auto delta =
            m_gesture.update(raw,
                             input,
                             io.DisplaySize.x,
                             io.DisplaySize.y,
                             visual.previewConfig.areaRatio,
                             std::max(8.0f, ImGui::GetFontSize() * .5f));
        // 密度轴的时间范围来自快照全谱时长，与预览可见范围倍率无关。
        // 点击只提交一次，拖动则先预览再在释放或取消时提交最后有效值。
        // 第二指加入不读取新指位置，因此不会从密度轴跳到内容区。
        // 命令显式绑定实例，在排队期间发生切谱也由播放控制器拒绝。
        // 本地游标只是显示覆盖，不写回密度缓存或谱面对象。
        if ( delta.m_rail && frame.m_snapshot ) {
            const auto target = previewDensityTimeAtY(
                delta.m_y,
                frame.m_railMin.y,
                frame.m_railMax.y,
                frame.m_snapshot->previewDensity.duration);
            if ( target ) {
                // 提示游标立即采用本地目标；命令时间扣除视觉偏移。
                // 轻点一次提交，拖动连续预览；取消不读取取消位置。
                m_densityTarget = *target;
                m_lastSeek      = *target - visual.getEffectiveVisualOffset();
                UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                    Logic::CmdSeek{ .time              = m_lastSeek,
                                    .isScrubbing       = !delta.m_tap,
                                    .beatmapInstanceId = m_instance }));
                m_railRunning = !delta.m_tap;
            }
        } else if ( (delta.m_tap || delta.m_pan) && frame.m_snapshot ) {
            m_ratio = delta.m_ratio;
            // 专用字段才启用 Preview 导航，经典主画布平移载荷不受影响。
            // 轻点使用当前显示帧时间与已应用的 Y 补偿，不读滞后的 Hover。
            // 拖动使用像素增量，逻辑端依序积分，不把旧快照时间反复写回。
            // 缩放锚点属于增量发生前的中点，便于同时保持锚点与应用平移。
            // 横向位移不进入命令，不能意外改变主画布轨道偏移。
            // 这个分支只发布值载荷，不借用会话、注册表或对象观察指针。
            // 纯点击不创建连续 Scrub，关闭窗口也无需补发一次结束定位。
            Logic::CmdPanCanvas command{
                .cameraId       = "Preview",
                .deltaY         = delta.m_dy,
                .viewportWidth  = frame.m_size.x,
                .viewportHeight = frame.m_size.y,
                .renderScaleY   = frame.m_snapshot->renderScaleY,
                .anchorY = delta.m_tap
                               ? delta.m_y - frame.m_origin.y - frame.m_yOffset
                               : delta.m_anchor - frame.m_origin.y,
                .previewAreaRatio  = m_ratio,
                .previewTap        = delta.m_tap,
                .previewInstanceId = instance,
                .previewVisualTime = frame.m_snapshot->currentTime
            };
            UI::Interaction::dispatchCommand(Event::LogicCommandEvent(command));
            m_navigationRunning |= delta.m_pan;
            // 连续帧只改 UI 标量；最终配置在释放边沿通过短配置锁发布。
            if ( delta.m_pan ) visual.previewConfig.areaRatio = m_ratio;
        }
        // 先处理本帧最终有效增量，再交付释放边沿。
        // Down 与 Up 合在一个 UI 帧时也遵守相同顺序。
        // 结束之后的空闲帧不再触发保存或发布最终命令。
        // 取消导航不等于撤销用户上一笔编辑，也不能清空原来的重做栈。
        if ( delta.m_finished ) finish();
        return true;
    }
    /// @brief 结束已经发布的最后有效浏览，绝不使用残留 Up 作为新落点。
    void suspend() override
    {
        finish();
        m_gesture.cancel(Graphic::mobileTouchSnapshot().m_epoch);
        m_densityTarget.reset();
    }
    /// @brief 方案守卫只读取本地门闩，不等待逻辑配置回流。
    bool hasGesture() const override
    {
        return m_gesture.active() || m_navigationRunning || m_railRunning;
    }
    /// @brief 与内容分开的密度轴游标，仅导航期间覆盖快照时间。
    std::optional<double> densityTarget() const override
    {
        return m_densityTarget;
    }

private:
    /// @brief 一个导航只提交一次最终状态；窗口隐藏与正常释放共用收尾。
    /// @warning 释放边沿允许低频布局保存，不在持续拖动帧执行文件操作。
    void finish()
    {
        if ( m_navigationRunning ) {
            // 发布最终范围先于结束命令；渲染覆盖保留到最终配置被逻辑采纳。
            auto& app = Config::AppConfig::instance();
            app.getVisualConfig().previewConfig.areaRatio = m_ratio;
            Logic::EditorEngine::instance().updatePreviewAreaRatio(m_ratio);
            UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                Logic::CmdPanCanvas{ .cameraId          = "Preview",
                                     .finishNavigation  = true,
                                     .previewAreaRatio  = m_ratio,
                                     .previewInstanceId = m_instance }));
            app.saveLayoutChanges();
            m_navigationRunning = false;
        }
        // 已发布过密度预览才需要最终提交，普通待定轻点取消没有副作用。
        // 收尾继续携带原实例，切谱后不能污染新的活动会话。
        // 最后目标在有效输入帧已经完成有限值与范围检查。
        // 不根据当前取消位置重新计算，以免系统手势带来额外跳转。
        // 结束时释放游标覆盖，让后续播放重新由正式快照驱动。
        if ( m_railRunning ) {
            // 密度拖动的最后值已经通过有效坐标校验，取消时不重新取手指位置。
            UI::Interaction::dispatchCommand(
                Event::LogicCommandEvent(Logic::CmdSeek{
                    .time = m_lastSeek, .beatmapInstanceId = m_instance }));
            m_railRunning = false;
        }
        m_densityTarget.reset();
    }
    /// @brief 原始触点分类器只有本窗口一个所有者。
    MobilePreviewGesture m_gesture;
    /// @brief 实例与方案代数隔离切谱和输入方案切换。
    std::uintptr_t m_instance{};
    std::uint64_t  m_generation{};
    /// @brief 中断帧检测不使用墙钟等待。
    int m_frame{};
    /// @brief 未经异步快照回流覆盖的本地最终导航值。
    float                 m_ratio{ 1 };
    double                m_lastSeek{};
    std::optional<double> m_densityTarget;
    /// @brief 连续预览与最终提交分别记录，防止隐藏后重复收尾。
    bool m_navigationRunning{}, m_railRunning{};
};
}  // namespace
/// @brief 移动构建和 CPU 回归共用生产解释器，不替换业务命令。
std::unique_ptr<PreviewInteractionPresentation>
createMobilePreviewInteractionPresentation()
{
    return std::make_unique<MobilePreviewInteractionPresentation>();
}
}  // namespace MMM::Canvas
