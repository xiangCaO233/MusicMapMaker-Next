#include "logic/BeatmapSession.h"
#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/tool/CutTool.h"
#include "logic/session/tool/DrawTool.h"
#include "logic/session/tool/GrabTool.h"
#include "logic/session/tool/MarqueeTool.h"
#include "mmm/beatmap/BeatMap.h"
#include <chrono>

// 独立函数，用于 entt 信号回调，标记 ScrollCache 为脏状态
static void markScrollCacheDirty(entt::registry& reg, entt::entity)
{
    if ( auto* cache = reg.ctx().find<MMM::Logic::System::ScrollCache>() ) {
        cache->isDirty = true;
    }
}

namespace MMM::Logic
{

BeatmapSession::BeatmapSession()
{
    m_tools[EditTool::Move]    = std::make_unique<GrabTool>();
    m_tools[EditTool::Marquee] = std::make_unique<MarqueeTool>();
    m_tools[EditTool::Draw]    = std::make_unique<DrawTool>();
    m_tools[EditTool::Cut]     = std::make_unique<CutTool>();

    // 初始化时注册 TimelineComponent 的增删改信号，并注入 ScrollCache 上下文
    m_timelineRegistry.ctx().emplace<System::ScrollCache>();
    m_timelineRegistry.on_construct<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    m_timelineRegistry.on_update<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    m_timelineRegistry.on_destroy<TimelineComponent>()
        .connect<&markScrollCacheDirty>();

    // 订阅音频事件
    m_audioFinishedToken =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::AudioFinishedEvent>(
                [this](const MMM::Event::AudioFinishedEvent& e) {
                    if ( !e.isLooping ) {
                        m_isPlaying = false;
                        // 确保 AudioManager 状态同步
                        Audio::AudioManager::instance().pause();
                    }
                });

    m_audioPositionToken =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::AudioPositionEvent>(
                [this](const MMM::Event::AudioPositionEvent& e) {
                    if ( m_isPlaying ) {
                        // 仅缓存位置和系统时间戳
                        m_lastAudioPos     = e.positionSeconds;
                        m_lastAudioSysTime = e.systemTimeSeconds;
                    }
                });
}

void BeatmapSession::pushCommand(LogicCommand&& cmd)
{
    m_commandQueue.enqueue(std::move(cmd));
}

void BeatmapSession::update(double dt, const Config::EditorConfig& config)
{
    m_lastConfig = config;

    // 处理来自 UI 的指令
    processCommands();

    // --- 边缘自动滚动处理 ---
    if ( std::abs(m_previewEdgeScrollVelocity) > 0.0001 ) {
        double delta     = m_previewEdgeScrollVelocity * dt;
        double totalTime = Audio::AudioManager::instance().getTotalTime();
        m_currentTime    = std::clamp(m_currentTime + delta, 0.0, totalTime);

        // 由于手动改变了时间，需要同步时钟和打击索引
        m_syncClock.reset(m_currentTime);
        if ( m_isPlaying ) {
            Audio::AudioManager::instance().seek(m_currentTime);
        }
        syncHitIndex();
    }

    double prevVisualTime = m_visualTime;

    // 更新播放时间
    if ( m_isPlaying ) {
        float speed = Audio::AudioManager::instance().getPlaybackSpeed();

        // 1. 平时：直接累加正常 ECS 周期自己的 dt，保证视觉流畅
        m_currentTime += static_cast<double>(dt * speed);
        m_syncClock.advance(dt, speed);

        // 2. 周期性：通过同步计时器修正可能的累积偏移
        m_syncTimer += dt;
        if ( m_syncTimer >= config.settings.syncConfig.syncInterval ) {
            // 使用事件回调缓存的最新硬件位置进行推演，消除事件传递延迟抖动
            double currentSysTime =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            double audioTime =
                (m_lastAudioPos > 0.0)
                    ? (m_lastAudioPos +
                       (currentSysTime - m_lastAudioSysTime) * speed)
                    : Audio::AudioManager::instance().getCurrentTime();

            // 修正视觉时间偏移 (根据配置的同步模式: Integral 或 WaterTank)
            m_syncClock.sync(audioTime, config.settings.syncConfig);

            // 将逻辑时间对齐至硬件时间，防止长期累积误差
            m_currentTime = audioTime;
            m_syncTimer   = 0.0;
        }

        // 3. 计算最终视觉时间 (包含配置的视觉偏移)
        m_visualTime = m_syncClock.getVisualTime() + config.visual.visualOffset;

        // --- 打击音效与特效触发逻辑 ---
        std::vector<System::HitFXSystem::HitEvent> triggeredEvents;

        // 检测时间跳跃（往前大跳、往回跳、或者刚刚从暂停状态恢复）
        bool isJump = (std::abs(m_visualTime - prevVisualTime) > 0.2) ||
                      (m_visualTime < prevVisualTime);

        if ( isJump ) {
            m_hitFXSystem.clearActiveEffects();
            syncHitIndex();
        } else {
            // 正常的帧推进

            // A. 音频预测触发 (提前 200ms)
            double predictWindow = 0.2;
            while ( m_nextPredictHitIndex < m_hitEvents.size() &&
                    m_hitEvents[m_nextPredictHitIndex].timestamp <=
                        (m_visualTime + predictWindow) ) {
                const auto& ev = m_hitEvents[m_nextPredictHitIndex];
                // 仅对未来且未预测过的事件进行预测
                if ( ev.timestamp > (prevVisualTime + predictWindow) ) {
                    m_hitFXSystem.triggerAudio(ev, config);
                }
                m_nextPredictHitIndex++;
            }

            // B. 视觉特效触发 (精确到帧)
            while ( m_nextHitIndex < m_hitEvents.size() &&
                    m_hitEvents[m_nextHitIndex].timestamp <= m_visualTime ) {
                const auto& ev = m_hitEvents[m_nextHitIndex];
                if ( ev.timestamp > prevVisualTime ) {
                    triggeredEvents.push_back(ev);
                }
                m_nextHitIndex++;
            }
        }
        m_hitFXSystem.update(m_visualTime, triggeredEvents, config);
    } else {
        m_visualTime = m_currentTime + config.visual.visualOffset;
        m_syncTimer  = 0.0;

        // 暂停状态下虽然不播放音效，但如果用户在拖拽进度条，我们需要实时同步
        // m_nextHitIndex，这样在重新播放的一瞬间索引就是正确的。
        if ( std::abs(m_visualTime - prevVisualTime) > 0.0001 ) {
            syncHitIndex();
        }
    }

    // 执行逻辑计算和生成渲染快照
    updateECSAndRender(config);
}

}  // namespace MMM::Logic
