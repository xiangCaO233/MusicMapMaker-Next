#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <stb_image.h>

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
                               std::is_same_v<T, CmdPackBeatmap> ||
                               std::is_same_v<T, CmdUpdateBeatmapMetadata> ) {
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
                                    std::is_same_v<T, CmdRemoveMarqueeAt> ||
                                    std::is_same_v<T, CmdStartBrush> ||
                                    std::is_same_v<T, CmdUpdateBrush> ||
                                    std::is_same_v<T, CmdEndBrush> ||
                                    std::is_same_v<T, CmdStartErase> ||
                                    std::is_same_v<T, CmdUpdateErase> ||
                                    std::is_same_v<T, CmdEndErase> ||
                                    std::is_same_v<T, CmdSelectAll> ) {
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
                                    std::is_same_v<T, CmdDeleteSelected> ||
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

        auto savePath = m_ctx->currentBeatmap->m_baseMapMetadata.map_path;
        if ( m_ctx->lastConfig.settings.saveFormatPreference ==
             Config::SaveFormatPreference::ForceMMM ) {
            savePath.replace_extension(".mmm");
            // 更新元数据中的路径，确保之后的一致性
            m_ctx->currentBeatmap->m_baseMapMetadata.map_path = savePath;
        }

        m_ctx->currentBeatmap->saveToFile(savePath);
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

void BeatmapSession::handleCommand(const CmdUpdateBeatmapMetadata& cmd)
{
    if ( m_ctx->currentBeatmap ) {
        auto oldAudio =
            m_ctx->currentBeatmap->m_baseMapMetadata.main_audio_path;
        auto oldCover =
            m_ctx->currentBeatmap->m_baseMapMetadata.main_cover_path;
        auto oldBPM   = m_ctx->currentBeatmap->m_baseMapMetadata.preference_bpm;
        auto oldTrack = m_ctx->currentBeatmap->m_baseMapMetadata.track_count;

        m_ctx->currentBeatmap->m_baseMapMetadata = cmd.baseMeta;
        XINFO("BeatmapSession: Metadata updated for {}",
              m_ctx->currentBeatmap->m_baseMapMetadata.name);

        // 同步轨道数到上下文，确保渲染实时更新
        m_ctx->trackCount = cmd.baseMeta.track_count;

        // 如果关键渲染参数发生变化，刷新 ScrollCache
        if ( oldBPM != cmd.baseMeta.preference_bpm ||
             oldTrack != cmd.baseMeta.track_count ) {
            XINFO(
                "BeatmapSession: Critical metadata changed, dirtying "
                "ScrollCache...");
            auto* cache =
                m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                cache->isDirty = true;
            }
        }

        // 如果音频路径发生变化，重新加载音频
        if ( oldAudio != cmd.baseMeta.main_audio_path ) {
            XINFO("BeatmapSession: Audio path changed, reloading...");
            auto audioPath = m_ctx->currentBeatmap->m_baseMapMetadata.map_path
                                 .parent_path() /
                             cmd.baseMeta.main_audio_path;
            if ( std::filesystem::exists(audioPath) ) {
                // 查找对应的 AudioResource 配置
                AudioTrackConfig config;
                auto* project = EditorEngine::instance().getCurrentProject();
                if ( project ) {
                    auto u8 =
                        cmd.baseMeta.main_audio_path.filename().u8string();
                    std::string fileName(
                        reinterpret_cast<const char*>(u8.c_str()), u8.size());

                    auto u8Full = cmd.baseMeta.main_audio_path.u8string();
                    std::string fullPathStr(
                        reinterpret_cast<const char*>(u8Full.c_str()),
                        u8Full.size());

                    for ( const auto& res : project->m_audioResources ) {
                        if ( res.m_id == fileName ||
                             res.m_path == fullPathStr ) {
                            config = res.m_config;
                            break;
                        }
                    }
                }
                auto        u8Audio = audioPath.u8string();
                std::string audioPathStr(
                    reinterpret_cast<const char*>(u8Audio.c_str()),
                    u8Audio.size());
                Audio::AudioManager::instance().loadBGM(audioPathStr, config);
            }
        }

        // 如果封面路径发生变化，更新背景图尺寸
        if ( oldCover != cmd.baseMeta.main_cover_path ) {
            auto bgPath = m_ctx->currentBeatmap->m_baseMapMetadata.map_path
                              .parent_path() /
                          cmd.baseMeta.main_cover_path;
            if ( std::filesystem::exists(bgPath) ) {
                int         w = 0, h = 0, comp = 0;
                auto        u8Bg = bgPath.u8string();
                std::string bgPathStr(
                    reinterpret_cast<const char*>(u8Bg.c_str()), u8Bg.size());
                if ( stbi_info(bgPathStr.c_str(), &w, &h, &comp) ) {
                    m_ctx->bgSize =
                        glm::vec2(static_cast<float>(w), static_cast<float>(h));
                }
            } else {
                m_ctx->bgSize = glm::vec2(0.0f);
            }
        }

        // 同步修改到项目入口列表中，确保侧边栏等 UI 实时更新
        auto* project = EditorEngine::instance().getCurrentProject();
        if ( project ) {
            for ( auto& entry : project->m_beatmaps ) {
                auto fullEntryPath =
                    project->m_projectRoot /
                    std::filesystem::path(reinterpret_cast<const char8_t*>(
                        entry.m_filePath.c_str()));

                if ( std::filesystem::exists(fullEntryPath) &&
                     std::filesystem::equivalent(fullEntryPath,
                                                 cmd.baseMeta.map_path) ) {
                    entry.m_name = cmd.baseMeta.version;
                    XINFO("BeatmapSession: Synced name '{}' to project entry",
                          entry.m_name);
                    break;
                }
            }
        }
    }
}

}  // namespace MMM::Logic
