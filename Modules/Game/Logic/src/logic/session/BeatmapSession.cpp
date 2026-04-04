#include "logic/BeatmapSession.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/NoteTransformSystem.h"
#include "logic/ecs/system/ScrollCache.h"

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

void BeatmapSession::update(double dt, const EditorConfig& config)
{
    m_lastConfig = config;

    // 处理来自 UI 的指令
    processCommands();

    // 更新播放时间
    if ( m_isPlaying ) {
        m_currentTime += dt;
    }

    // 同步最新的图集 UV 映射
    m_atlasUVMap = EditorEngine::instance().getAtlasUVMap();

    // 执行逻辑计算和生成渲染快照
    updateECSAndRender(config);
}

void BeatmapSession::updateECSAndRender(const EditorConfig& config)
{
    // 1. 调用 ECS System 更新全局物理位置 (Logical Transform)
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

        // 注入当前 UV 映射到快照，供 Batcher 使用
        snapshot->uvMap = m_atlasUVMap;

        // 判定线高度比例计算
        float judgmentLineY = camera.viewportHeight * config.judgeline_pos;

        // 3. 调用 ECS System 针对当前 Camera 生成渲染快照
        System::NoteRenderSystem::generateSnapshot(m_noteRegistry,
                                                   m_timelineRegistry,
                                                   snapshot,
                                                   m_currentTime,
                                                   camera.viewportWidth,
                                                   camera.viewportHeight,
                                                   judgmentLineY,
                                                   m_trackCount,
                                                   config);

        // 4. 提交专属快照
        syncBuffer->pushWorkingSnapshot();
    }
}

}  // namespace MMM::Logic
