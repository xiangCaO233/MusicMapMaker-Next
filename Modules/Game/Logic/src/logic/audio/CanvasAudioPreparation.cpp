/// @file CanvasAudioPreparation.cpp
/// @brief 分离画布切换中的后台资源准备与逻辑线程的时间线安装。
/// @details 准备任务只消费描述符中的事件；实际安装仍使用当前谱面的 STOP、
/// 内容末尾与光标。一次只允许一个后台任务，避免快速切换积压解码和 PCM。
#include "logic/audio/CanvasAudioPreparation.h"

#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/audio/AudioTimelineDescriptor.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <utility>

namespace MMM::Logic
{

/// @brief 取消旧活动目标的准备意图，后台只需自然完成已接收的任务。
/// @param previous 离开的会话，可为空；调用者已提取需要继承的续播意图。
/// @warning 目标变更路径，持有会话锁；同步调用不触碰逻辑线程的任务槽。
void CanvasAudioPreparation::invalidate(SessionContext* previous)
{
    // 即使很快切回同一画布，第二次进入也需要重新核对描述符和续播意图。
    ++m_generation;
    // 新建和关闭不一定走常规焦点切换。撤回应随旧会话保留，
    // 不能依赖下一轮仍能按旧索引找到它。
    if ( previous ) {
        previous->m_waitingForCanvasAudio    = false;
        previous->m_playWhenCanvasAudioReady = false;
    }
}

/// @brief 延迟切换时的资源解码，使谱面在声音就绪前仍可编辑。
/// @param context 已被设为活动项的谱面。
/// @param shouldResume 切换决策给出的初始播放意图。
/// @pre 调用者已持有会话锁，并已使前一活动目标的准备代次失效。
void CanvasAudioPreparation::defer(SessionContext& context, bool shouldResume)
{
    context.m_waitingForCanvasAudio    = true;
    context.m_playWhenCanvasAudioReady = shouldResume;
    // 保留已知音频尾端，同时给从未激活过的谱面提供无音频的导航范围。
    context.audioTimelineTotalTime =
        std::max(context.audioTimelineTotalTime,
                 context.audioTimelineDescriptor.m_chartEndSeconds);
    // 这里只登记意图。已有任务时由其归还槽位，不能再保留一份事件数组。
    m_pendingGeneration = m_generation;
}

/// @brief 检查是否能开始下一项工作，或取回已就绪的结果。
/// @details 失效期间也保留就绪通知，确保逻辑可以回收旧 PCM。
/// @return 当前轮次需要获取会话锁处理准备状态时返回 true。
/// @warning 每轮逻辑调用；零时长轮询，不等待工作者。
bool CanvasAudioPreparation::needsAdvance() const
{
    if ( !m_job.result.valid() ) return m_pendingGeneration.has_value();
    // 有任务时不排队第二份事件。当前任务返回后再以最新意图启动准备。
    return m_job.result.wait_for(std::chrono::seconds(0)) ==
           std::future_status::ready;
}

/// @brief 仅将当前会话及描述符仍匹配的准备结果安装为全局时间线。
/// @pre 会话锁覆盖目标读取、指纹检查和安装。后台任务只能持有事件值。
/// @return 安装成功并对齐当前会话播放时钟时返回 true。
/// @warning 逻辑低频状态推进。只消费已就绪结果，文件解码及旧源轨回收交给后台。
bool CanvasAudioPreparation::advance(SessionContext*  context,
                                     const Project*   project,
                                     ice::ThreadPool* pool)
{
    // 焦点消费后也可能在同轮调用本函数，因此入口自己检查状态。
    // 未就绪的 future 必须保留，避免其 get 或析构阻塞逻辑线程。
    if ( !needsAdvance() ) return false;

    // 先回收就绪的任务。哪怕当前为占位画布，也不让过期资源留在 future 中。
    auto completed = std::exchange(m_job, {});
    // 同步切换只推进代次，逻辑侧在这里消费失效标记。没有原目标时，
    // 不得清除新会话自己的异步状态，也不能从已取消结果恢复播放。
    if ( m_pendingGeneration != m_generation || !context ||
         !context->m_waitingForCanvasAudio || !context->currentBeatmap ) {
        if ( context && m_pendingGeneration == m_generation ) {
            context->m_waitingForCanvasAudio    = false;
            context->m_playWhenCanvasAudioReady = false;
        }
        m_pendingGeneration.reset();
        return false;
    }

    // 准备期间可能收到资源改名、编辑或协作更新，复核当前描述符而非缓存指纹。
    auto& ctx = *context;
    if ( ctx.isAudioTimelineDescriptorDirty ) {
        SessionUtils::rebuildAudioTimelineDescriptor(
            ctx,
            ctx.collaborationProject ? ctx.collaborationProject.get()
                                     : project);
    }
    const auto& descriptor = ctx.audioTimelineDescriptor;
    auto&       audio      = Audio::AudioManager::instance();
    // 即使两个谱面的音频事件完全相同，也不能从先前切换恢复续播意图。
    // 等代次匹配后再检查指纹，这样同一谱面换音频也会丢弃原任务。
    if ( completed.result.valid() && completed.generation == m_generation &&
         completed.fingerprint == descriptor.m_fingerprint ) {
        if ( auto prepared = completed.result.get() ) {
            auto result =
                audio.installAudioTimeline(*prepared,
                                           descriptor.m_chartEndSeconds,
                                           descriptor.m_fingerprint,
                                           descriptor.m_stops);
            // 成功和失败都消费本次请求，不能在下一轮重新进入同步加载。
            ctx.m_waitingForCanvasAudio          = false;
            ctx.isAudioTimelineActivationPending = false;
            ctx.missingAudioTimelineClipCount    = result.missingClipCount;
            m_pendingGeneration.reset();
            // 缺失片段可与安装成功共存，先记录诊断，再判断是否可恢复传输。
            for ( const auto& diagnostic : result.diagnostics ) {
                XWARN("Audio timeline load: {}", diagnostic.message);
            }

            // 取提交时的光标和播放意图，后台完成前的定位或暂停必须仍然生效。
            const bool shouldPlay          = ctx.m_playWhenCanvasAudioReady;
            ctx.m_playWhenCanvasAudioReady = false;
            if ( result.success ) {
                // 安装后活动指纹已匹配，激活只定位、播放或暂停并校准视觉时钟。
                ctx.isPlaying =
                    SessionUtils::activateAudioTimeline(ctx, shouldPlay) &&
                    shouldPlay;
                return true;
            }
            // 安装失败时续播意图已消费。仍可按谱面内容时长定位和编辑。
            ctx.audioTimelineTotalTime = descriptor.m_chartEndSeconds;
            return false;
        }
    }

    // 未匹配时只重新准备当前描述符，不能沿用旧 PCM 后再更新指纹。
    // 若池已关闭，走下面的收尾分支，保证准备状态有明确结束。
    if ( pool ) {
        // 取消和指纹变化最多丢弃一次旧结果。后台只持有事件副本和稳定服务，
        // 不观察 SessionContext、曲目选择或界面焦点。
        m_job.generation  = m_generation;
        m_job.fingerprint = descriptor.m_fingerprint;
        m_job.result = pool->enqueue([&audio, events = descriptor.m_events]() {
            // 回收缓存会释放解码后的源轨，放在同一个后台任务中完成。
            static_cast<void>(audio.releaseUnusedTrackCache());
            return audio.prepareAudioTimeline(events);
        });
        return false;
    }

    // 运行时池不存在时保留可编辑范围，不能让活动会话永久停在准备状态。
    ctx.m_waitingForCanvasAudio    = false;
    ctx.m_playWhenCanvasAudioReady = false;
    // 清除自动激活标记；之后用户可显式重试，但不能在逻辑轮询里同步解码。
    ctx.isAudioTimelineActivationPending = false;
    m_pendingGeneration.reset();
    return false;
}

/// @brief 收回后台任务，保证音频系统可以安全关闭。
/// @warning 调用前必须已停止逻辑线程。这里是唯一允许等待工作者的入口。
void CanvasAudioPreparation::finishWorker()
{
    if ( m_job.result.valid() ) {
        // 先等待完成再销毁 future，使关闭音频服务时没有工作者仍借用它。
        // 不清除当前意图，重新启动时允许按最新描述符再提交一次任务。
        m_job.result.wait();
        m_job = {};
    }
}
}  // namespace MMM::Logic
