#include "logic/BeatmapSession.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"

namespace MMM::Logic
{

void BeatmapSession::processCommands()
{
    LogicCommand cmd;
    while ( m_commandQueue.try_dequeue(cmd) ) {
        std::visit(
            [this](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;

                // --- Session 自己处理的命令 ---
                if constexpr ( std::is_same_v<T, CmdUpdateEditorConfig> ||
                               std::is_same_v<T, CmdUpdateViewport> ||
                               std::is_same_v<T, CmdLoadBeatmap> ||
                               std::is_same_v<T, CmdSaveBeatmap> ||
                               std::is_same_v<T, CmdSaveBeatmapAs> ||
                               std::is_same_v<T, CmdPackBeatmap> ) {
                    this->handleCommand(arg);
                }
                // --- Playback 处理的命令 ---
                else if constexpr ( std::is_same_v<T, CmdSetPlayState> ||
                                    std::is_same_v<T, CmdSeek> ||
                                    std::is_same_v<T, CmdSetPlaybackSpeed> ||
                                    std::is_same_v<T, CmdScroll> ) {
                    m_playback->handleCommand(arg);
                }
                // --- Interaction 处理的命令 ---
                else if constexpr ( std::is_same_v<T, CmdSetHoveredEntity> ||
                                    std::is_same_v<T, CmdSelectEntity> ||
                                    std::is_same_v<T, CmdStartDrag> ||
                                    std::is_same_v<T, CmdUpdateDrag> ||
                                    std::is_same_v<T, CmdEndDrag> ||
                                    std::is_same_v<T, CmdChangeTool> ||
                                    std::is_same_v<T, CmdSetMousePosition> ||
                                    std::is_same_v<T, CmdUpdateTrackCount> ||
                                    std::is_same_v<T, CmdStartMarquee> ||
                                    std::is_same_v<T, CmdUpdateMarquee> ||
                                    std::is_same_v<T, CmdEndMarquee> ||
                                    std::is_same_v<T, CmdRemoveMarqueeAt> ) {
                    m_interaction->handleCommand(arg);
                }
                // --- Action 处理的命令 ---
                else if constexpr ( std::is_same_v<T, CmdUndo> ||
                                    std::is_same_v<T, CmdRedo> ||
                                    std::is_same_v<T, CmdCopy> ||
                                    std::is_same_v<T, CmdCut> ||
                                    std::is_same_v<T, CmdPaste> ||
                                    std::is_same_v<T, CmdUpdateTimelineEvent> ||
                                    std::is_same_v<T, CmdDeleteTimelineEvent> ||
                                    std::is_same_v<T,
                                                   CmdCreateTimelineEvent> ) {
                    m_actions->handleCommand(arg);
                }
            },
            cmd);
    }
}

// --- Session 自己处理的 ---

void BeatmapSession::handleCommand(const CmdUpdateEditorConfig& cmd)
{
    m_ctx->lastConfig = cmd.config;
    auto* cache = m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        cache->isDirty = true;
    }
}

void BeatmapSession::handleCommand(const CmdUpdateViewport& cmd)
{
    if ( m_ctx->cameras.find(cmd.cameraId) == m_ctx->cameras.end() ) {
        m_ctx->cameras[cmd.cameraId] =
            CameraInfo{ cmd.cameraId, cmd.width, cmd.height };
    } else {
        m_ctx->cameras[cmd.cameraId].viewportWidth  = cmd.width;
        m_ctx->cameras[cmd.cameraId].viewportHeight = cmd.height;
    }
}

void BeatmapSession::handleCommand(const CmdLoadBeatmap& cmd)
{
    SessionUtils::loadBeatmap(*m_ctx, cmd.beatmap);
}

void BeatmapSession::handleCommand(const CmdSaveBeatmap& cmd)
{
    if ( m_ctx->currentBeatmap ) {
        SessionUtils::syncBeatmap(*m_ctx);
        m_ctx->currentBeatmap->saveToFile(
            m_ctx->currentBeatmap->m_baseMapMetadata.map_path);
    }
}

void BeatmapSession::handleCommand(const CmdSaveBeatmapAs& cmd)
{
    if ( m_ctx->currentBeatmap ) {
        SessionUtils::syncBeatmap(*m_ctx);
        m_ctx->currentBeatmap->saveToFile(cmd.path);
    }
}

void BeatmapSession::handleCommand(const CmdPackBeatmap& cmd)
{
    // TODO: 实现打包逻辑
}

}  // namespace MMM::Logic
