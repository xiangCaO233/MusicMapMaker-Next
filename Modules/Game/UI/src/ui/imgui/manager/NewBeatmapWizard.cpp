#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "audio/AudioManager.h"
#include "common/AudioInfoUtils.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/utils/UIThemeUtils.h"
#include <ImGuiFileDialog.h>

namespace MMM::UI
{

NewBeatmapWizard::NewBeatmapWizard() : IUIView("NewBeatmapWizard")
{
    reset();
}

void NewBeatmapWizard::update(UIManager* sourceManager)
{
    if ( !m_isOpen ) return;

    if ( m_shouldOpen ) {
        ImGui::OpenPopup(TR("ui.wizard.new_beatmap.title").data());
        m_shouldOpen = false;
        XINFO("NewBeatmapWizard: Opening popup modal...");
    }

    ImGui::SetNextWindowSize(ImVec2(600, 700), ImGuiCond_FirstUseEver);
    if ( ImGui::BeginPopupModal(TR("ui.wizard.new_beatmap.title").data(),
                                &m_isOpen) ) {

        auto DrawInput = [&](const char* label, char* buf, size_t bufSize) {
            ImGui::InputText(label, buf, bufSize);
        };

        ImGui::SeparatorText(TR("ui.settings.beatmap.info").data());
        DrawInput(TR("ui.settings.beatmap.name").data(),
                  m_nameBuf,
                  sizeof(m_nameBuf));
        DrawInput(TR("ui.settings.beatmap.title").data(),
                  m_titleBuf,
                  sizeof(m_titleBuf));
        DrawInput(TR("ui.settings.beatmap.title_unicode").data(),
                  m_titleUnicodeBuf,
                  sizeof(m_titleUnicodeBuf));
        DrawInput(TR("ui.settings.beatmap.artist").data(),
                  m_artistBuf,
                  sizeof(m_artistBuf));
        DrawInput(TR("ui.settings.beatmap.artist_unicode").data(),
                  m_artistUnicodeBuf,
                  sizeof(m_artistUnicodeBuf));
        DrawInput(TR("ui.settings.beatmap.mapper").data(),
                  m_authorBuf,
                  sizeof(m_authorBuf));
        DrawInput(TR("ui.settings.beatmap.version").data(),
                  m_versionBuf,
                  sizeof(m_versionBuf));

        ImGui::SeparatorText(TR("ui.settings.beatmap.preference").data());
        float bpm = (float)m_bpm;
        if ( ImGui::DragFloat(TR("ui.settings.beatmap.bpm").data(),
                              &bpm,
                              0.1f,
                              0.0f,
                              1000.0f,
                              "%.2f") ) {
            m_bpm = (double)bpm;
        }

        if ( ImGui::InputInt(TR("ui.settings.beatmap.tracks").data(),
                             &m_trackCount) ) {
            if ( m_trackCount < 1 ) m_trackCount = 1;
        }

        ImGui::SeparatorText(TR("ui.settings.beatmap.resource").data());

        auto* project = Logic::EditorEngine::instance().getCurrentProject();
        if ( !project ) {
            ImGui::TextColored(Utils::UIThemeUtils::getDangerColor(),
                               "%s",
                               TR("ui.wizard.new_beatmap.no_project").data());
            ImGui::EndPopup();
            return;
        }

        // 音频选择
        std::string audioPreview =
            m_selectedAudioPath.empty()
                ? TR("ui.wizard.new_beatmap.select_audio").data()
                : m_selectedAudioPath.generic_string();

        if ( ImGui::BeginCombo(TR("ui.wizard.new_beatmap.select_audio").data(),
                               audioPreview.c_str()) ) {
            for ( const auto& res : project->m_audioResources ) {
                if ( res.m_type != MMM::AudioTrackType::Main ) continue;

                bool        isSelected = (m_selectedAudioPath == res.m_path);
                std::string label      = res.m_id + "##" + res.m_path;
                if ( ImGui::Selectable(label.c_str(), isSelected) ) {
                    onAudioSelected(res.m_path);
                }
                if ( isSelected ) ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", res.m_path.c_str());
            }
            ImGui::EndCombo();
        }

        // 背景选择
        std::string coverPreview =
            m_selectedCoverPath.empty()
                ? TR("ui.wizard.new_beatmap.select_cover").data()
                : m_selectedCoverPath.generic_string();

        if ( ImGui::BeginCombo(TR("ui.wizard.new_beatmap.select_cover").data(),
                               coverPreview.c_str()) ) {
            // 扫描项目中的图片/视频文件
            std::vector<std::string> resources;
            try {
                for ( const auto& entry :
                      std::filesystem::recursive_directory_iterator(
                          project->m_projectRoot) ) {
                    if ( entry.is_regular_file() ) {
                        auto ext = entry.path().extension().string();
                        std::transform(
                            ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if ( ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                             ext == ".bmp" || ext == ".mp4" || ext == ".avi" ) {
                            auto rel = std::filesystem::relative(
                                entry.path(), project->m_projectRoot);
                            resources.push_back(rel.generic_string());
                        }
                    }
                }
            } catch ( ... ) {
            }

            for ( const auto& resPath : resources ) {
                bool isSelected = (m_selectedCoverPath == resPath);
                if ( ImGui::Selectable(resPath.c_str(), isSelected) ) {
                    m_selectedCoverPath = resPath;
                }
                if ( isSelected ) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( m_selectedAudioPath.empty() ) {
            ImGui::BeginDisabled();
        }

        if ( ImGui::Button(TR("ui.wizard.new_beatmap.create").data(),
                           ImVec2(120, 0)) ) {
            // 从 UI 状态同步所有字段到 m_meta，确保不丢失任何编辑内容
            m_meta.name           = m_nameBuf;
            m_meta.title          = m_titleBuf;
            m_meta.title_unicode  = m_titleUnicodeBuf;
            m_meta.artist         = m_artistBuf;
            m_meta.artist_unicode = m_artistUnicodeBuf;
            m_meta.author         = m_authorBuf;
            m_meta.version        = m_versionBuf;
            m_meta.track_count    = std::max(1, m_trackCount);
            m_meta.preference_bpm = m_bpm;
            m_meta.map_length     = m_audioDuration;

            m_meta.main_audio_path = m_selectedAudioPath;
            m_meta.main_cover_path = m_selectedCoverPath;

            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdCreateBeatmap{ m_meta });
            close();
        }

        if ( m_selectedAudioPath.empty() ) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(
                Utils::UIThemeUtils::getWarningColor(),
                "%s",
                TR("ui.wizard.new_beatmap.audio_not_selected").data());
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 130);
        if ( ImGui::Button(TR("ui.wizard.new_beatmap.cancel").data(),
                           ImVec2(120, 0)) ) {
            close();
        }


        ImGui::EndPopup();
    }
}

void NewBeatmapWizard::open()
{
    m_isOpen     = true;
    m_shouldOpen = true;
    reset();
}

void NewBeatmapWizard::close()
{
    m_isOpen = false;
    ImGui::CloseCurrentPopup();
}

void NewBeatmapWizard::reset()
{
    m_meta = MMM::BaseMapMeta();

    m_bpm        = 120.0;
    m_trackCount = 4;

    strncpy(m_nameBuf, "New Beatmap", sizeof(m_nameBuf));
    strncpy(m_titleBuf, "", sizeof(m_titleBuf));
    strncpy(m_titleUnicodeBuf, "", sizeof(m_titleUnicodeBuf));
    strncpy(m_artistBuf, "", sizeof(m_artistBuf));
    strncpy(m_artistUnicodeBuf, "", sizeof(m_artistUnicodeBuf));
    strncpy(m_authorBuf, "Unknown", sizeof(m_authorBuf));
    strncpy(m_versionBuf, "Easy", sizeof(m_versionBuf));

    m_selectedAudioPath.clear();
    m_selectedCoverPath.clear();
    m_audioDuration = 0.0;
}

void NewBeatmapWizard::onAudioSelected(const std::filesystem::path& path)
{
    m_selectedAudioPath = path;

    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return;

    // 使用 ffmpeg 获取信息 (需要绝对路径)
    auto absPath = project->m_projectRoot / path;
    auto infoOpt = MMM::Utils::AudioInfoUtils::probeAudioInfo(absPath);
    if ( infoOpt ) {
        auto& info = *infoOpt;

        m_audioDuration = info.duration;

        strncpy(m_titleBuf, info.title.c_str(), sizeof(m_titleBuf));
        strncpy(
            m_titleUnicodeBuf, info.title.c_str(), sizeof(m_titleUnicodeBuf));
        strncpy(m_artistBuf, info.artist.c_str(), sizeof(m_artistBuf));
        strncpy(m_artistUnicodeBuf,
                info.artist.c_str(),
                sizeof(m_artistUnicodeBuf));

        // 自动设置谱面内部名称为标题（去空格）
        std::string safeName = info.title;
        safeName.erase(
            std::remove_if(safeName.begin(), safeName.end(), ::isspace),
            safeName.end());
        if ( !safeName.empty() ) {
            strncpy(m_nameBuf, safeName.c_str(), sizeof(m_nameBuf));
        }
    }
}

}  // namespace MMM::UI
