#include "logic/BeatmapSession.h"
#include "logic/ecs/system/ScrollCache.h"
#include <variant>

namespace MMM::Logic
{

void BeatmapSession::processCommands()
{
    LogicCommand cmd;
    while ( m_commandQueue.try_dequeue(cmd) ) {
        std::visit([this](auto&& arg) { handleCommand(arg); }, cmd);
    }
}

// --- Session & Config Handlers ---

void BeatmapSession::handleCommand(const CmdUpdateEditorConfig& cmd)
{
    m_lastConfig = cmd.config;
    auto* cache  = m_timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        cache->isDirty = true;
    }
}

void BeatmapSession::handleCommand(const CmdUpdateViewport& cmd)
{
    if ( m_cameras.find(cmd.cameraId) == m_cameras.end() ) {
        m_cameras[cmd.cameraId] =
            CameraInfo{ cmd.cameraId, cmd.width, cmd.height };
    } else {
        m_cameras[cmd.cameraId].viewportWidth  = cmd.width;
        m_cameras[cmd.cameraId].viewportHeight = cmd.height;
    }
}

void BeatmapSession::handleCommand(const CmdLoadBeatmap& cmd)
{
    loadBeatmap(cmd.beatmap);
}

void BeatmapSession::handleCommand(const CmdSaveBeatmap& cmd)
{
    // TODO: 实现保存逻辑
}

void BeatmapSession::handleCommand(const CmdPackBeatmap& cmd)
{
    // TODO: 实现打包逻辑
}

} // namespace MMM::Logic