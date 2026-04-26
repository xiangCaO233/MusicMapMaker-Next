#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/SessionUtils.h"
#include "mmm/beatmap/BeatMap.h"
#include <stb_image.h>

namespace MMM::Logic
{

void SessionUtils::loadBeatmap(SessionContext&               ctx,
                               std::shared_ptr<MMM::BeatMap> beatmap)
{
    // 将 path 转为 UTF-8 std::string 供外部 C API 使用（Windows 下 string() 为
    // ANSI）
    auto pathToStr = [](const std::filesystem::path& p) {
        auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.c_str()),
                           u8.size());
    };

    ctx.noteRegistry.clear();
    ctx.timelineRegistry.clear();
    ctx.actionStack.clear();
    Audio::AudioManager::instance().stop();

    // m_isPlaying      = true;
    ctx.currentTime    = 0.0;
    ctx.currentBeatmap = beatmap;

    if ( !beatmap ) {
        ctx.hitEvents.clear();
        ctx.nextHitIndex = 0;
        return;
    }

    // 计算背景图片的绝对路径并获取尺寸
    ctx.bgSize  = glm::vec2(0.0f);
    auto bgPath = beatmap->m_baseMapMetadata.map_path.parent_path() /
                  beatmap->m_baseMapMetadata.main_cover_path;

    if ( !beatmap->m_baseMapMetadata.main_cover_path.empty() &&
         std::filesystem::exists(bgPath) ) {
        int w = 0, h = 0, comp = 0;
        if ( stbi_info(pathToStr(bgPath).c_str(), &w, &h, &comp) ) {
            ctx.bgSize =
                glm::vec2(static_cast<float>(w), static_cast<float>(h));
        }
    }

    ctx.trackCount = beatmap->m_baseMapMetadata.track_count;
    if ( ctx.trackCount <= 0 ) ctx.trackCount = 12;  // 默认值

    // 加载音频
    auto audioPath = beatmap->m_baseMapMetadata.map_path.parent_path() /
                     beatmap->m_baseMapMetadata.main_audio_path;
    if ( !beatmap->m_baseMapMetadata.main_audio_path.empty() &&
         std::filesystem::exists(audioPath) ) {
        // 查找对应的 AudioResource 配置
        AudioTrackConfig config;
        auto*            project = EditorEngine::instance().getCurrentProject();
        if ( project ) {
            for ( const auto& res : project->m_audioResources ) {
                if ( res.m_id == pathToStr(beatmap->m_baseMapMetadata
                                               .main_audio_path.filename()) ||
                     res.m_path ==
                         pathToStr(
                             beatmap->m_baseMapMetadata.main_audio_path) ) {
                    config = res.m_config;
                    break;
                }
            }
        }
        Audio::AudioManager::instance().loadBGM(pathToStr(audioPath), config);
    }

    // 清空缓存上下文，以确保重新构建
    if ( auto* cache =
             ctx.timelineRegistry.ctx().find<System::ScrollCache>() ) {
        cache->isDirty = true;
    }

    // 根据传入的 BeatMap 构建 ECS 实体
    // 1. 加载 Timing 点
    for ( const auto& timing : beatmap->m_timings ) {
        auto entity = ctx.timelineRegistry.create();
        ctx.timelineRegistry.emplace<TimelineComponent>(
            entity,
            timing.m_timestamp / 1000.0,  // 毫秒转秒
            timing.m_timingEffect,
            timing.m_timingEffectParameter);
    }

    // 用于追踪子物件，防止在折线之外重复绘制
    std::unordered_map<const ::MMM::Note*, entt::entity> noteToEntity;

    // 2. 加载普通音符 (Notes)
    for ( const auto& note : beatmap->m_noteData.notes ) {
        auto entity         = ctx.noteRegistry.create();
        noteToEntity[&note] = entity;

        int track = static_cast<int>(note.m_track);

        ctx.noteRegistry.emplace<NoteComponent>(
            entity,
            note.m_type,
            note.m_timestamp / 1000.0,  // 毫秒转秒
            0.0,
            track);

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 3. 加载长键 (Holds)
    for ( const auto& hold : beatmap->m_noteData.holds ) {
        auto entity         = ctx.noteRegistry.create();
        noteToEntity[&hold] = entity;

        int track = static_cast<int>(hold.m_track);

        ctx.noteRegistry.emplace<NoteComponent>(
            entity,
            hold.m_type,
            hold.m_timestamp / 1000.0,  // 毫秒转秒
            hold.m_duration / 1000.0,   // 毫秒转秒
            track);

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 4. 加载滑键 (Flicks)
    for ( const auto& flick : beatmap->m_noteData.flicks ) {
        auto entity          = ctx.noteRegistry.create();
        noteToEntity[&flick] = entity;

        int track = static_cast<int>(flick.m_track);

        ctx.noteRegistry.emplace<NoteComponent>(entity,
                                                flick.m_type,
                                                flick.m_timestamp / 1000.0,
                                                0.0,
                                                track,
                                                flick.m_dtrack);

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 5. 加载折线 (Polylines)
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        auto entity = ctx.noteRegistry.create();

        int track = static_cast<int>(polyline.m_track);

        auto& comp = ctx.noteRegistry.emplace<NoteComponent>(
            entity, polyline.m_type, polyline.m_timestamp / 1000.0, 0.0, track);

        // 填充子物件并标记它们为 SubNote
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            const auto& subNote = subNoteRef.get();

            // 标记原始实体为 SubNote，防止独立绘制
            if ( auto it = noteToEntity.find(&subNote);
                 it != noteToEntity.end() ) {
                auto& subComp = ctx.noteRegistry.get<NoteComponent>(it->second);
                subComp.m_isSubNote      = true;
                subComp.m_parentPolyline = entity;
            }

            NoteComponent::SubNote sn;
            sn.type       = subNote.m_type;
            sn.timestamp  = subNote.m_timestamp / 1000.0;
            sn.trackIndex = static_cast<int>(subNote.m_track);
            sn.duration   = 0.0;
            sn.dtrack     = 0;

            if ( subNote.m_type == ::MMM::NoteType::HOLD ) {
                const auto& h = static_cast<const ::MMM::Hold&>(subNote);
                sn.duration   = h.m_duration / 1000.0;
            } else if ( subNote.m_type == ::MMM::NoteType::FLICK ) {
                const auto& f = static_cast<const ::MMM::Flick&>(subNote);
                sn.dtrack     = f.m_dtrack;
            }
            comp.m_subNotes.push_back(sn);
        }

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 构建音效触发事件队列并排序
    ctx.hitEvents.clear();
    ctx.nextHitIndex = 0;

    // 收集所有的 subNote 引用，避免它们被重复加入普通音符的播放队列
    std::unordered_set<const ::MMM::Note*> subNotesSet;
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            subNotesSet.insert(&subNoteRef.get());
        }
    }

    using HitRole = System::HitFXSystem::HitEvent::Role;

    for ( const auto& note : beatmap->m_noteData.notes ) {
        if ( subNotesSet.find(&note) != subNotesSet.end() ) continue;
        ctx.hitEvents.push_back({ note.m_timestamp / 1000.0,
                                  note.m_type,
                                  HitRole::None,
                                  1,
                                  static_cast<int>(note.m_track),
                                  0,
                                  0.0,
                                  false });
    }
    for ( const auto& hold : beatmap->m_noteData.holds ) {
        if ( subNotesSet.find(&hold) != subNotesSet.end() ) continue;
        ctx.hitEvents.push_back({ hold.m_timestamp / 1000.0,
                                  hold.m_type,
                                  HitRole::None,
                                  1,
                                  static_cast<int>(hold.m_track),
                                  0,
                                  hold.m_duration / 1000.0,
                                  false });
    }
    for ( const auto& flick : beatmap->m_noteData.flicks ) {
        if ( subNotesSet.find(&flick) != subNotesSet.end() ) continue;
        int span = std::abs(flick.m_dtrack) + 1;
        ctx.hitEvents.push_back({ flick.m_timestamp / 1000.0,
                                  flick.m_type,
                                  HitRole::None,
                                  span,
                                  static_cast<int>(flick.m_track),
                                  flick.m_dtrack,
                                  0.0,
                                  false });
    }
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        // 对于 Polyline 本身不发声，由子物件发声
        size_t subNoteCount = polyline.m_subNotes.size();
        for ( size_t i = 0; i < subNoteCount; ++i ) {
            const auto& subNote = polyline.m_subNotes[i].get();

            HitRole role = HitRole::Internal;
            if ( i == 0 )
                role = HitRole::Head;
            else if ( i == subNoteCount - 1 )
                role = HitRole::Tail;

            int    span        = 1;
            int    trackOffset = 0;
            double duration    = 0.0;
            if ( subNote.m_type == ::MMM::NoteType::FLICK ) {
                const auto& f = static_cast<const ::MMM::Flick&>(subNote);
                span          = std::abs(f.m_dtrack) + 1;
                trackOffset   = f.m_dtrack;
            } else if ( subNote.m_type == ::MMM::NoteType::HOLD ) {
                const auto& h = static_cast<const ::MMM::Hold&>(subNote);
                duration      = h.m_duration / 1000.0;
            }

            ctx.hitEvents.push_back({ subNote.m_timestamp / 1000.0,
                                      subNote.m_type,
                                      role,
                                      span,
                                      static_cast<int>(subNote.m_track),
                                      trackOffset,
                                      duration,
                                      true });
        }
    }
    std::sort(ctx.hitEvents.begin(), ctx.hitEvents.end());

    XINFO(
        "Loaded new BeatMap with {} notes, {} holds, {} flicks, {} polylines "
        "and {} timings.",
        beatmap->m_noteData.notes.size(),
        beatmap->m_noteData.holds.size(),
        beatmap->m_noteData.flicks.size(),
        beatmap->m_noteData.polylines.size(),
        beatmap->m_timings.size());
}


void SessionUtils::syncBeatmap(SessionContext& ctx)
{
    if ( !ctx.currentBeatmap ) return;

    // 1. 清空原始数据
    ctx.currentBeatmap->m_allNotes.clear();
    ctx.currentBeatmap->m_noteData.notes.clear();
    ctx.currentBeatmap->m_noteData.holds.clear();
    ctx.currentBeatmap->m_noteData.flicks.clear();
    ctx.currentBeatmap->m_noteData.polylines.clear();
    ctx.currentBeatmap->m_timings.clear();

    // 2. 同步时间线
    auto tlView = ctx.timelineRegistry.view<TimelineComponent>();
    std::vector<TimelineComponent> sortedTLs;
    for ( auto entity : tlView ) {
        sortedTLs.push_back(tlView.get<TimelineComponent>(entity));
    }
    std::sort(
        sortedTLs.begin(), sortedTLs.end(), [](const auto& a, const auto& b) {
            return a.m_timestamp < b.m_timestamp;
        });

    double currentBPM = 120.0;
    if ( ctx.currentBeatmap &&
         ctx.currentBeatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
        currentBPM = ctx.currentBeatmap->m_baseMapMetadata.preference_bpm;
    }
    for ( const auto& tc : sortedTLs ) {
        Timing timing;
        timing.m_timestamp             = tc.m_timestamp * 1000.0;
        timing.m_timingEffect          = tc.m_effect;
        timing.m_timingEffectParameter = tc.m_value;

        if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
            currentBPM           = tc.m_value;
            timing.m_bpm         = currentBPM;
            timing.m_beat_length = 60000.0 / std::max(0.1, timing.m_bpm);
        } else {
            timing.m_bpm         = currentBPM;  // 继承当前红线BPM
            timing.m_beat_length = tc.m_value;
        }

        ctx.currentBeatmap->m_timings.push_back(timing);
    }

    // 3. 同步物体
    auto noteView = ctx.noteRegistry.view<NoteComponent>();

    // 第一遍：创建所有非折线物件（包含折线子物件在 notedata 中的独立副本）
    std::unordered_map<entt::entity, std::reference_wrapper<Note>> entityToRef;

    for ( auto entity : noteView ) {
        const auto& nc = noteView.get<NoteComponent>(entity);
        if ( nc.m_type == ::MMM::NoteType::POLYLINE ) continue;

        if ( nc.m_type == ::MMM::NoteType::NOTE ) {
            Note n;
            n.m_type      = ::MMM::NoteType::NOTE;
            n.m_timestamp = nc.m_timestamp * 1000.0;
            n.m_track     = static_cast<uint32_t>(nc.m_trackIndex);
            ctx.currentBeatmap->m_noteData.notes.push_back(std::move(n));
            auto& ref = ctx.currentBeatmap->m_noteData.notes.back();
            entityToRef.emplace(entity, ref);
            ctx.currentBeatmap->m_allNotes.push_back(ref);
        } else if ( nc.m_type == ::MMM::NoteType::HOLD ) {
            Hold h;
            h.m_type      = ::MMM::NoteType::HOLD;
            h.m_timestamp = nc.m_timestamp * 1000.0;
            h.m_track     = static_cast<uint32_t>(nc.m_trackIndex);
            h.m_duration  = nc.m_duration * 1000.0;
            ctx.currentBeatmap->m_noteData.holds.push_back(std::move(h));
            auto& ref = ctx.currentBeatmap->m_noteData.holds.back();
            entityToRef.emplace(entity, ref);
            ctx.currentBeatmap->m_allNotes.push_back(ref);
        } else if ( nc.m_type == ::MMM::NoteType::FLICK ) {
            Flick f;
            f.m_type      = ::MMM::NoteType::FLICK;
            f.m_timestamp = nc.m_timestamp * 1000.0;
            f.m_track     = static_cast<uint32_t>(nc.m_trackIndex);
            f.m_dtrack    = nc.m_dtrack;
            ctx.currentBeatmap->m_noteData.flicks.push_back(std::move(f));
            auto& ref = ctx.currentBeatmap->m_noteData.flicks.back();
            entityToRef.emplace(entity, ref);
            ctx.currentBeatmap->m_allNotes.push_back(ref);
        }
    }

    // 第二遍：处理 Polyline
    for ( auto entity : noteView ) {
        const auto& nc = noteView.get<NoteComponent>(entity);
        if ( nc.m_type != ::MMM::NoteType::POLYLINE ) continue;

        Polyline p;
        p.m_type      = ::MMM::NoteType::POLYLINE;
        p.m_timestamp = nc.m_timestamp * 1000.0;
        p.m_track     = static_cast<uint32_t>(nc.m_trackIndex);

        // 查找子实体
        struct TempSub {
            entt::entity e;
            double       t;
        };
        std::vector<TempSub> subEntities;
        for ( auto subEnt : noteView ) {
            const auto& snc = noteView.get<NoteComponent>(subEnt);
            if ( snc.m_isSubNote && snc.m_parentPolyline == entity ) {
                subEntities.push_back({ subEnt, snc.m_timestamp });
            }
        }

        if ( !subEntities.empty() ) {
            std::sort(subEntities.begin(),
                      subEntities.end(),
                      [](const auto& a, const auto& b) { return a.t < b.t; });
            for ( const auto& te : subEntities ) {
                auto it = entityToRef.find(te.e);
                if ( it != entityToRef.end() ) {
                    p.m_subNotes.push_back(it->second);
                    if ( it->second.get().m_type == ::MMM::NoteType::HOLD ) {
                        p.m_subHolds.push_back(
                            static_cast<Hold&>(it->second.get()));
                    } else if ( it->second.get().m_type ==
                                ::MMM::NoteType::FLICK ) {
                        p.m_subFlicks.push_back(
                            static_cast<Flick&>(it->second.get()));
                    }
                }
            }
        } else {
            // 回退方案：根据 m_subNotes 向量重建
            for ( const auto& sn : nc.m_subNotes ) {
                if ( sn.type == ::MMM::NoteType::NOTE ) {
                    Note n;
                    n.m_type      = ::MMM::NoteType::NOTE;
                    n.m_timestamp = sn.timestamp * 1000.0;
                    n.m_track     = static_cast<uint32_t>(sn.trackIndex);
                    ctx.currentBeatmap->m_noteData.notes.push_back(
                        std::move(n));
                    auto& ref = ctx.currentBeatmap->m_noteData.notes.back();
                    p.m_subNotes.push_back(ref);
                    ctx.currentBeatmap->m_allNotes.push_back(ref);
                } else if ( sn.type == ::MMM::NoteType::HOLD ) {
                    Hold h;
                    h.m_type      = ::MMM::NoteType::HOLD;
                    h.m_timestamp = sn.timestamp * 1000.0;
                    h.m_track     = static_cast<uint32_t>(sn.trackIndex);
                    h.m_duration  = sn.duration * 1000.0;
                    ctx.currentBeatmap->m_noteData.holds.push_back(
                        std::move(h));
                    auto& ref = ctx.currentBeatmap->m_noteData.holds.back();
                    p.m_subNotes.push_back(ref);
                    p.m_subHolds.push_back(ref);
                    ctx.currentBeatmap->m_allNotes.push_back(ref);
                } else if ( sn.type == ::MMM::NoteType::FLICK ) {
                    Flick f;
                    f.m_type      = ::MMM::NoteType::FLICK;
                    f.m_timestamp = sn.timestamp * 1000.0;
                    f.m_track     = static_cast<uint32_t>(sn.trackIndex);
                    f.m_dtrack    = sn.dtrack;
                    ctx.currentBeatmap->m_noteData.flicks.push_back(
                        std::move(f));
                    auto& ref = ctx.currentBeatmap->m_noteData.flicks.back();
                    p.m_subNotes.push_back(ref);
                    p.m_subFlicks.push_back(ref);
                    ctx.currentBeatmap->m_allNotes.push_back(ref);
                }
            }
        }

        ctx.currentBeatmap->m_noteData.polylines.push_back(std::move(p));
        ctx.currentBeatmap->m_allNotes.push_back(
            ctx.currentBeatmap->m_noteData.polylines.back());
    }

    // 最后对所有物件按时间排序
    std::sort(ctx.currentBeatmap->m_allNotes.begin(),
              ctx.currentBeatmap->m_allNotes.end(),
              [](const std::reference_wrapper<Note>& a,
                 const std::reference_wrapper<Note>& b) {
                  return a.get().m_timestamp < b.get().m_timestamp;
              });
}

}  // namespace MMM::Logic
