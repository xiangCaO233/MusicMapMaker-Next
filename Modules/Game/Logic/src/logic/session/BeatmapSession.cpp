#include "logic/BeatmapSession.h"
#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/NoteTransformSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "mmm/beatmap/BeatMap.h"

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
    // 初始化时注册 TimelineComponent 的增删改信号，并注入 ScrollCache 上下文
    m_timelineRegistry.ctx().emplace<System::ScrollCache>();
    m_timelineRegistry.on_construct<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    m_timelineRegistry.on_update<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    m_timelineRegistry.on_destroy<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
}

void BeatmapSession::pushCommand(LogicCommand&& cmd)
{
    m_commandQueue.enqueue(std::move(cmd));
}

void BeatmapSession::update(double dt, const Common::EditorConfig& config)
{
    m_lastConfig = config;

    // 处理来自 UI 的指令
    processCommands();

    double prevTime       = m_currentTime;
    double prevVisualTime = m_visualTime;

    // 更新播放时间
    if ( m_isPlaying ) {
        float speed = Audio::AudioManager::instance().getPlaybackSpeed();

        // 1. 平时：直接累加正常 ECS 周期自己的 dt，保证视觉流畅
        m_currentTime += static_cast<double>(dt * speed);
        m_syncClock.advance(dt, speed);

        // 2. 周期性：通过同步计时器修正可能的累积偏移
        m_syncTimer += dt;
        if ( m_syncTimer >= config.syncConfig.syncInterval ) {
            double audioTime = Audio::AudioManager::instance().getCurrentTime();
            // 修正视觉时间偏移 (根据配置的同步模式: Integral 或 WaterTank)
            m_syncClock.sync(audioTime, config.syncConfig);
            // 将逻辑时间对齐至硬件时间，防止长期累积误差
            m_currentTime = audioTime;
            m_syncTimer   = 0.0;
        }

        m_visualTime = m_syncClock.getVisualTime() + config.visualOffset;

        // --- 打击音效与特效触发逻辑 ---
        std::vector<System::HitFXSystem::HitEvent> triggeredEvents;

        // 检测时间跳跃（无论是往前跳还是往回跳）
        if ( std::abs(m_visualTime - prevVisualTime) > 0.2 ) {
            // 发生了时间跳转（比如用户拖拽进度条或点击跳转），不发出这一大段的音效
            auto it =
                std::lower_bound(m_hitEvents.begin(),
                                 m_hitEvents.end(),
                                 System::HitFXSystem::HitEvent{
                                     m_visualTime, ::MMM::NoteType::NOTE });
            m_nextHitIndex = std::distance(m_hitEvents.begin(), it);
        } else {
            // 正常的帧推进
            while ( m_nextHitIndex < m_hitEvents.size() &&
                    m_hitEvents[m_nextHitIndex].timestamp <= m_visualTime ) {
                const auto& ev = m_hitEvents[m_nextHitIndex];
                // 确保只有在 prevVisualTime 之后触发的才发声
                if ( ev.timestamp > prevVisualTime ) {
                    triggeredEvents.push_back(ev);
                }
                m_nextHitIndex++;
            }
        }
        m_hitFXSystem.update(m_visualTime, triggeredEvents, config);
    } else {
        m_visualTime = m_currentTime + config.visualOffset;
        m_syncTimer  = 0.0;
    }

    // 执行逻辑计算和生成渲染快照
    updateECSAndRender(config);
}

void BeatmapSession::updateECSAndRender(const Common::EditorConfig& config)
{
    // 1. 调用 ECS System 更新全局物理位置 (Logical Transform)
    // 注意：物理位置更新应基于逻辑时间 m_currentTime
    System::NoteTransformSystem::update(
        m_noteRegistry, m_timelineRegistry, m_currentTime, config);

    // 2. 遍历所有注册的视口 (Camera) 进行独立的视口剔除和坐标映射
    for ( auto& [cameraId, camera] : m_cameras ) {
        // 从 EditorEngine 获取该 Camera 专属的缓冲
        auto syncBuffer = EditorEngine::instance().getSyncBuffer(cameraId);
        if ( !syncBuffer ) continue;

        RenderSnapshot* snapshot = syncBuffer->getWorkingSnapshot();
        if ( !snapshot ) continue;

        snapshot->clear();

        // 注入该 Camera 特有的 UV 映射到快照
        snapshot->uvMap     = EditorEngine::instance().getAtlasUVMap(cameraId);
        snapshot->isPlaying = m_isPlaying;
        snapshot->currentTime = m_visualTime;  // 快照使用视觉平滑时间

        if ( m_currentBeatmap ) {
            auto bgPath =
                m_currentBeatmap->m_baseMapMetadata.map_path.parent_path() /
                m_currentBeatmap->m_baseMapMetadata.main_cover_path;
            snapshot->backgroundPath = bgPath.string();
            snapshot->bgSize         = m_bgSize;
        }

        // 判定线高度比例计算
        float judgmentLineY = camera.viewportHeight * config.judgeline_pos;

        // 获取主视口高度用于预览区比例对齐
        float mainHeight = 1000.0f;
        auto  itMain     = m_cameras.find("Main");
        if ( itMain != m_cameras.end() ) {
            mainHeight = itMain->second.viewportHeight;
        }

        // 3. 调用 ECS System 针对当前 Camera 生成渲染快照
        // 使用视觉时间 m_visualTime 进行剔除和位置映射
        System::NoteRenderSystem::generateSnapshot(m_noteRegistry,
                                                   m_timelineRegistry,
                                                   snapshot,
                                                   cameraId,
                                                   m_visualTime,
                                                   camera.viewportWidth,
                                                   camera.viewportHeight,
                                                   judgmentLineY,
                                                   m_trackCount,
                                                   config,
                                                   mainHeight);

        // 4. 生成打击特效
        float leftX        = camera.viewportWidth * config.trackLayout.left;
        float rightX       = camera.viewportWidth * config.trackLayout.right;
        float trackAreaW   = rightX - leftX;
        float singleTrackW = trackAreaW / static_cast<float>(m_trackCount);

        // 针对预览区，布局参数略有不同
        if ( cameraId == "Preview" ) {
            leftX  = config.previewConfig.margin.left;
            rightX = camera.viewportWidth - config.previewConfig.margin.right;
            trackAreaW   = rightX - leftX;
            singleTrackW = trackAreaW / static_cast<float>(m_trackCount);
        }

        m_hitFXSystem.generateSnapshot(snapshot,
                                       m_visualTime,
                                       config,
                                       m_trackCount,
                                       judgmentLineY,
                                       leftX,
                                       singleTrackW);

        // 5. 提交专属快照
        syncBuffer->pushWorkingSnapshot();
    }
}

}  // namespace MMM::Logic
