#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "audio/AudioManager.h"
#include "common/AudioInfoUtils.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/SessionUtils.h"
#include "ui/UIManager.h"
#include "ui/imgui/tools/BpmMeasurementToolView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fmt/format.h>
#include <mutex>
#include <string_view>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 将字符串安全写入固定长度输入缓冲区。
/// @param buffer 目标缓冲区。
/// @param bufferSize 缓冲区长度。
/// @param value 待写入文本。
void copyToBuffer(char* buffer, std::size_t bufferSize, std::string_view value)
{
    if ( bufferSize == 0 ) return;

    const auto copySize = std::min(bufferSize - 1, value.size());
    std::memcpy(buffer, value.data(), copySize);
    buffer[copySize] = '\0';
}

/// @brief 判断两个项目内路径是否指向相同资源。
/// @param lhs 左侧路径。
/// @param rhs 右侧路径。
/// @return UTF-8 字符串或文件名匹配时返回 true。
bool resourcePathMatches(const std::filesystem::path& lhs,
                         const std::filesystem::path& rhs)
{
    const auto lhsUtf8 = Config::pathToUtf8(lhs);
    const auto rhsUtf8 = Config::pathToUtf8(rhs);
    if ( lhsUtf8 == rhsUtf8 ) return true;
    return Config::pathToUtf8(lhs.filename()) ==
           Config::pathToUtf8(rhs.filename());
}
}  // namespace

NewBeatmapWizard::NewBeatmapWizard() : IUIView("NewBeatmapWizard")
{
    reset();
}

std::vector<NewBeatmapWizard::OpenTemplateOption>
NewBeatmapWizard::collectOpenTemplateOptions() const
{
    std::vector<OpenTemplateOption> options;

    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> lock(engine.getSessionMutex());
    auto                                  entries = engine.getSessionEntries();
    options.reserve(entries.size());

    for ( int32_t index = 0; index < static_cast<int32_t>(entries.size());
          ++index ) {
        const auto& entry = entries[static_cast<std::size_t>(index)];
        if ( entry.isLogoPlaceholder || !entry.session ) continue;

        const auto& ctx = entry.session->getContext();
        if ( !ctx.currentBeatmap ) continue;

        const auto&        meta = ctx.currentBeatmap->m_baseMapMetadata;
        OpenTemplateOption option;
        option.sessionIndex = index;
        option.cameraId     = entry.cameraId;
        option.displayName =
            entry.displayName.empty() ? meta.name : entry.displayName;
        option.internalName = meta.name;
        option.mapPath      = meta.map_path;
        option.beatmap      = ctx.currentBeatmap;
        options.push_back(std::move(option));
    }

    return options;
}

const NewBeatmapWizard::OpenTemplateOption*
NewBeatmapWizard::findSelectedTemplate(
    const std::vector<OpenTemplateOption>& templateOptions) const
{
    if ( m_templateCameraId.empty() ) return nullptr;

    auto it = std::find_if(templateOptions.begin(),
                           templateOptions.end(),
                           [&](const OpenTemplateOption& option) {
                               return option.cameraId == m_templateCameraId;
                           });
    if ( it == templateOptions.end() ) return nullptr;
    return &(*it);
}

void NewBeatmapWizard::selectTemplate(const OpenTemplateOption& option)
{
    m_createMode          = CreateMode::OpenTemplate;
    m_templateCameraId    = option.cameraId;
    m_templateDisplayName = option.displayName;
    m_templateBeatmap     = option.beatmap;

    if ( m_templateBeatmap ) {
        applyTemplateResourceDefaults(*m_templateBeatmap);
    }
}

void NewBeatmapWizard::applyTemplateResourceDefaults(
    const MMM::BeatMap& beatmap)
{
    const auto& meta = beatmap.m_baseMapMetadata;

    if ( !meta.main_audio_path.empty() ) {
        m_selectedAudioPath    = meta.main_audio_path;
        m_selectedAudioTrackId = findAudioTrackIdForPath(meta.main_audio_path);
    }
    if ( !meta.main_cover_path.empty() ) {
        m_selectedCoverPath = meta.main_cover_path;
    }
    if ( !meta.cover_path.empty() ) {
        m_selectedCoverImgPath = meta.cover_path;
    }
    if ( meta.preference_bpm > 0.0 ) {
        m_bpm = meta.preference_bpm;
    }
    if ( meta.track_count > 0 ) {
        m_trackCount = meta.track_count;
    }
    if ( meta.map_length > 0.0 ) {
        m_audioDuration = meta.map_length;
    }

    if ( m_titleBuf[0] == '\0' && !meta.title.empty() ) {
        copyToBuffer(m_titleBuf, sizeof(m_titleBuf), meta.title);
    }
    if ( m_titleUnicodeBuf[0] == '\0' && !meta.title_unicode.empty() ) {
        copyToBuffer(
            m_titleUnicodeBuf, sizeof(m_titleUnicodeBuf), meta.title_unicode);
    }
    if ( m_artistBuf[0] == '\0' && !meta.artist.empty() ) {
        copyToBuffer(m_artistBuf, sizeof(m_artistBuf), meta.artist);
    }
    if ( m_artistUnicodeBuf[0] == '\0' && !meta.artist_unicode.empty() ) {
        copyToBuffer(m_artistUnicodeBuf,
                     sizeof(m_artistUnicodeBuf),
                     meta.artist_unicode);
    }
}

std::string NewBeatmapWizard::findAudioTrackIdForPath(
    const std::filesystem::path& path) const
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || path.empty() ) return {};

    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_type != MMM::AudioTrackType::Main ) continue;

        const auto resourcePath = Config::utf8ToPath(resource.m_path);
        if ( resourcePathMatches(resourcePath, path) ||
             resource.m_id == Config::pathToUtf8(path.filename()) ) {
            return resource.m_id;
        }
    }

    return {};
}

void NewBeatmapWizard::syncSelectedTemplateBeatmap()
{
    if ( m_templateCameraId.empty() ) return;

    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> lock(engine.getSessionMutex());
    auto                                  entries = engine.getSessionEntries();
    for ( const auto& entry : entries ) {
        if ( entry.cameraId != m_templateCameraId || !entry.session ) continue;

        auto& ctx = entry.session->getContextMutable();
        Logic::SessionUtils::syncBeatmap(ctx);
        m_templateBeatmap = ctx.currentBeatmap;
        return;
    }
}

void NewBeatmapWizard::syncMetaFromInputs()
{
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
    m_meta.cover_path      = m_selectedCoverImgPath;
}

bool NewBeatmapWizard::hasInternalNameConflict() const
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return false;

    std::string name(m_nameBuf);
    if ( name.empty() ) return false;

    return std::any_of(project->m_beatmaps.begin(),
                       project->m_beatmaps.end(),
                       [&](const auto& entry) { return entry.m_name == name; });
}

void NewBeatmapWizard::submitCreateRequest()
{
    syncMetaFromInputs();

    Logic::CmdCreateBeatmap cmd;
    cmd.baseMeta = m_meta;
    if ( m_createMode == CreateMode::OpenTemplate && m_templateBeatmap ) {
        syncSelectedTemplateBeatmap();
        cmd.templateBeatmap = m_templateBeatmap;
        cmd.templateOptions = m_templateOptions;
    }

    Logic::EditorEngine::instance().pushCommand(std::move(cmd));
    close();
}

void NewBeatmapWizard::renderTemplatePickerPopup(
    const std::vector<OpenTemplateOption>& templateOptions)
{
    if ( ImGui::BeginPopupModal("NewBeatmapTemplatePicker",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize) ) {
        ImGui::TextUnformatted(
            TR("ui.wizard.new_beatmap.template.pick_title").data());
        ImGui::Separator();

        if ( templateOptions.empty() ) {
            ImGui::TextDisabled(
                "%s", TR("ui.wizard.new_beatmap.template.none_open").data());
        } else {
            ImGui::BeginChild(
                "TemplateBeatmapList", ImVec2(460.0f, 220.0f), true);
            for ( const auto& option : templateOptions ) {
                std::string label    = fmt::format("{} ({})##{}",
                                                   option.displayName,
                                                   option.internalName,
                                                   option.cameraId);
                bool        selected = option.cameraId == m_templateCameraId;
                if ( ImGui::Selectable(label.c_str(), selected) ) {
                    selectTemplate(option);
                    m_shouldOpenTemplateOptions = true;
                    ImGui::CloseCurrentPopup();
                }
                if ( !option.mapPath.empty() && ImGui::IsItemHovered() ) {
                    auto pathText = Config::pathToUtf8(option.mapPath);
                    ImGui::SetTooltip("%s", pathText.c_str());
                }
            }
            ImGui::EndChild();
        }

        if ( ImGui::Button(TR("ui.wizard.new_beatmap.cancel").data(),
                           ImVec2(120.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void NewBeatmapWizard::renderTemplateOptionsPopup()
{
    if ( ImGui::BeginPopupModal("NewBeatmapTemplateOptions",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize) ) {
        ImGui::TextUnformatted(
            TR("ui.wizard.new_beatmap.template.options_title").data());
        ImGui::Separator();

        ImGui::Checkbox(
            TR("ui.wizard.new_beatmap.template.copy_metadata").data(),
            &m_templateOptions.copyMetadata);
        ImGui::Checkbox(
            TR("ui.wizard.new_beatmap.template.copy_timelines").data(),
            &m_templateOptions.copyTimelines);
        ImGui::Checkbox(
            TR("ui.wizard.new_beatmap.template.copy_objects").data(),
            &m_templateOptions.copyObjects);

        ImGui::Spacing();
        if ( ImGui::Button(TR("ui.help.ok").data(), ImVec2(120.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.wizard.new_beatmap.cancel").data(),
                           ImVec2(120.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void NewBeatmapWizard::renderDuplicateNameWarningPopup()
{
    if ( ImGui::BeginPopupModal("NewBeatmapDuplicateNameWarning",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize) ) {
        ImGui::TextColored(
            Utils::UIThemeUtils::getWarningColor(),
            "%s",
            TR("ui.wizard.new_beatmap.duplicate_name.title").data());
        ImGui::Separator();

        std::string name(m_nameBuf);
        ImGui::TextWrapped(
            "%s",
            TR_FMT("ui.wizard.new_beatmap.duplicate_name.message", name)
                .c_str());

        ImGui::Spacing();
        if ( ImGui::Button(
                 TR("ui.wizard.new_beatmap.duplicate_name.continue").data(),
                 ImVec2(140.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
            submitCreateRequest();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.wizard.new_beatmap.cancel").data(),
                           ImVec2(120.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void NewBeatmapWizard::renderTemplateSourceControls(
    const std::vector<OpenTemplateOption>& templateOptions)
{
    if ( templateOptions.empty() && m_createMode == CreateMode::OpenTemplate ) {
        m_createMode = CreateMode::Blank;
        m_templateCameraId.clear();
        m_templateDisplayName.clear();
        m_templateBeatmap.reset();
    }

    if ( auto* selected = findSelectedTemplate(templateOptions) ) {
        m_templateBeatmap     = selected->beatmap;
        m_templateDisplayName = selected->displayName;
    } else if ( !m_templateCameraId.empty() ) {
        m_templateCameraId.clear();
        m_templateDisplayName.clear();
        m_templateBeatmap.reset();
    }

    ImGui::SeparatorText(TR("ui.wizard.new_beatmap.creation_source").data());

    if ( ImGui::RadioButton(TR("ui.wizard.new_beatmap.source.blank").data(),
                            m_createMode == CreateMode::Blank) ) {
        m_createMode = CreateMode::Blank;
    }
    ImGui::SameLine();

    if ( templateOptions.empty() ) {
        ImGui::BeginDisabled();
    }
    if ( ImGui::RadioButton(TR("ui.wizard.new_beatmap.source.template").data(),
                            m_createMode == CreateMode::OpenTemplate) ) {
        m_createMode                    = CreateMode::OpenTemplate;
        m_shouldOpenTemplatePicker      = true;
        m_templateOptions.copyTimelines = true;
    }
    if ( templateOptions.empty() ) {
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "%s", TR("ui.wizard.new_beatmap.template.none_open").data());
    }

    if ( m_createMode == CreateMode::OpenTemplate ) {
        std::string selectedText =
            m_templateBeatmap
                ? TR_FMT("ui.wizard.new_beatmap.template.selected",
                         m_templateDisplayName)
                : std::string(
                      TR("ui.wizard.new_beatmap.template.not_selected").data());
        ImGui::TextWrapped("%s", selectedText.c_str());

        if ( ImGui::Button(TR("ui.wizard.new_beatmap.template.pick").data(),
                           ImVec2(150.0f, 0.0f)) ) {
            m_shouldOpenTemplatePicker = true;
        }
        ImGui::SameLine();
        if ( !m_templateBeatmap ) {
            ImGui::BeginDisabled();
        }
        if ( ImGui::Button(TR("ui.wizard.new_beatmap.template.options").data(),
                           ImVec2(150.0f, 0.0f)) ) {
            m_shouldOpenTemplateOptions = true;
        }
        if ( !m_templateBeatmap ) {
            ImGui::EndDisabled();
        }
    }

    if ( m_shouldOpenTemplatePicker ) {
        ImGui::OpenPopup("NewBeatmapTemplatePicker");
        m_shouldOpenTemplatePicker = false;
    }
    renderTemplatePickerPopup(templateOptions);

    if ( m_shouldOpenTemplateOptions ) {
        ImGui::OpenPopup("NewBeatmapTemplateOptions");
        m_shouldOpenTemplateOptions = false;
    }
    renderTemplateOptionsPopup();
}

void NewBeatmapWizard::update(UIManager* sourceManager)
{
    if ( !m_isOpen ) return;

    if ( m_shouldOpen ) {
        ImGui::OpenPopup(TR("ui.wizard.new_beatmap.title").data());
        m_shouldOpen = false;
        XINFO("NewBeatmapWizard: Opening popup modal...");
    }

    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin(TR("ui.wizard.new_beatmap.title").data(),
                          &m_isOpen,
                          ImGuiWindowFlags_None,
                          ImVec2(600.0f * dpiScale, 700.0f * dpiScale),
                          false) ) {

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

        auto* project = Logic::EditorEngine::instance().getCurrentProject();
        if ( !project ) {
            ImGui::TextColored(Utils::UIThemeUtils::getDangerColor(),
                               "%s",
                               TR("ui.wizard.new_beatmap.no_project").data());
            ImGui::EndPopup();
            return;
        }

        auto templateOptions = collectOpenTemplateOptions();
        renderTemplateSourceControls(templateOptions);

        ImGui::SeparatorText(TR("ui.settings.beatmap.resource").data());

        // 音频选择
        std::string audioPreview =
            m_selectedAudioPath.empty()
                ? TR("ui.wizard.new_beatmap.select_audio").data()
                : Config::pathToUtf8(m_selectedAudioPath);

        const char* measureBpmLabel =
            TR("ui.wizard.new_beatmap.measure_bpm").data();
        const float measureBpmWidth = ImGui::CalcTextSize(measureBpmLabel).x +
                                      ImGui::GetStyle().FramePadding.x * 2.0f;
        const float comboWidth =
            std::max(120.0f,
                     ImGui::GetContentRegionAvail().x - measureBpmWidth -
                         ImGui::GetStyle().ItemSpacing.x);

        ImGui::SetNextItemWidth(comboWidth);
        if ( ImGui::BeginCombo("##NewBeatmapAudioSelect",
                               audioPreview.c_str()) ) {
            for ( const auto& res : project->m_audioResources ) {
                if ( res.m_type != MMM::AudioTrackType::Main ) continue;

                bool        isSelected = (m_selectedAudioTrackId == res.m_id);
                std::string label      = res.m_id + "##" + res.m_path;
                if ( ImGui::Selectable(label.c_str(), isSelected) ) {
                    m_selectedAudioTrackId = res.m_id;
                    onAudioSelected(Config::utf8ToPath(res.m_path));
                }
                if ( isSelected ) ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", res.m_path.c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if ( m_selectedAudioTrackId.empty() ) {
            ImGui::BeginDisabled();
        }
        if ( ImGui::Button(measureBpmLabel, ImVec2(measureBpmWidth, 0.0f)) ) {
            std::string viewName = "BpmMeasurementTool";
            auto*       tool =
                sourceManager->getView<BpmMeasurementToolView>(viewName);
            if ( !tool ) {
                auto toolView = std::make_unique<BpmMeasurementToolView>(
                    TR("ui.tools.bpm_measure").data());
                tool = toolView.get();
                sourceManager->registerView(viewName, std::move(toolView));
            }
            if ( tool ) {
                tool->openWithAudioTrack(m_selectedAudioTrackId);
            }
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }
        if ( m_selectedAudioTrackId.empty() ) {
            ImGui::EndDisabled();
        }

        // 封面选择 (只可指向图片文件)
        std::string coverImgPreview =
            m_selectedCoverImgPath.empty()
                ? TR("ui.wizard.new_beatmap.select_cover_img").data()
                : Config::pathToUtf8(m_selectedCoverImgPath);

        ImGui::SetNextItemWidth(-FLT_MIN);
        if ( ImGui::BeginCombo("##NewBeatmapCoverImageSelect",
                               coverImgPreview.c_str()) ) {
            // 扫描项目中的图片文件
            std::vector<std::string> resources;
            try {
                for ( const auto& entry :
                      std::filesystem::recursive_directory_iterator(
                          project->m_projectRoot) ) {
                    if ( entry.is_regular_file() ) {
                        auto ext = Config::pathToUtf8(entry.path().extension());
                        std::transform(
                            ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if ( ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                             ext == ".bmp" ) {
                            auto rel = std::filesystem::relative(
                                entry.path(), project->m_projectRoot);
                            resources.push_back(Config::pathToUtf8(rel));
                        }
                    }
                }
            } catch ( ... ) {
            }

            for ( const auto& resPath : resources ) {
                bool isSelected = (m_selectedCoverImgPath == resPath);
                if ( ImGui::Selectable(resPath.c_str(), isSelected) ) {
                    m_selectedCoverImgPath = resPath;
                }
                if ( isSelected ) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // 背景选择
        std::string coverPreview =
            m_selectedCoverPath.empty()
                ? TR("ui.wizard.new_beatmap.select_cover").data()
                : Config::pathToUtf8(m_selectedCoverPath);

        ImGui::SetNextItemWidth(-FLT_MIN);
        if ( ImGui::BeginCombo("##NewBeatmapBackgroundSelect",
                               coverPreview.c_str()) ) {
            // 扫描项目中的图片/视频文件
            std::vector<std::string> resources;
            try {
                for ( const auto& entry :
                      std::filesystem::recursive_directory_iterator(
                          project->m_projectRoot) ) {
                    if ( entry.is_regular_file() ) {
                        auto ext = Config::pathToUtf8(entry.path().extension());
                        std::transform(
                            ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if ( ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                             ext == ".bmp" || ext == ".mp4" || ext == ".avi" ) {
                            auto rel = std::filesystem::relative(
                                entry.path(), project->m_projectRoot);
                            resources.push_back(Config::pathToUtf8(rel));
                        }
                    }
                }
            } catch ( ... ) {
            }

            for ( const auto& resPath : resources ) {
                bool isSelected = (m_selectedCoverPath == resPath);
                if ( ImGui::Selectable(resPath.c_str(), isSelected) ) {
                    m_selectedCoverPath = resPath;

                    // 如果背景是图片且封面为空，则自动沿用同一张图片。
                    auto ext = Config::pathToUtf8(
                        Config::utf8ToPath(resPath).extension());
                    std::transform(
                        ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if ( ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                         ext == ".bmp" ) {
                        if ( m_selectedCoverImgPath.empty() ) {
                            m_selectedCoverImgPath = resPath;
                        }
                    }
                }
                if ( isSelected ) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool needsTemplateSelection =
            m_createMode == CreateMode::OpenTemplate && !m_templateBeatmap;
        const bool canCreate =
            !m_selectedAudioPath.empty() && !needsTemplateSelection;

        if ( !canCreate ) {
            ImGui::BeginDisabled();
        }

        if ( ImGui::Button(TR("ui.wizard.new_beatmap.create").data(),
                           ImVec2(120, 0)) ) {
            if ( hasInternalNameConflict() ) {
                ImGui::OpenPopup("NewBeatmapDuplicateNameWarning");
            } else {
                submitCreateRequest();
            }
        }

        if ( !canCreate ) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            const char* warningText =
                m_selectedAudioPath.empty()
                    ? TR("ui.wizard.new_beatmap.audio_not_selected").data()
                    : TR("ui.wizard.new_beatmap.template.not_selected").data();
            ImGui::TextColored(
                Utils::UIThemeUtils::getWarningColor(), "%s", warningText);
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 130);
        if ( ImGui::Button(TR("ui.wizard.new_beatmap.cancel").data(),
                           ImVec2(120, 0)) ) {
            close();
        }

        renderDuplicateNameWarningPopup();

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

    copyToBuffer(m_nameBuf, sizeof(m_nameBuf), "New Beatmap");
    copyToBuffer(m_titleBuf, sizeof(m_titleBuf), "");
    copyToBuffer(m_titleUnicodeBuf, sizeof(m_titleUnicodeBuf), "");
    copyToBuffer(m_artistBuf, sizeof(m_artistBuf), "");
    copyToBuffer(m_artistUnicodeBuf, sizeof(m_artistUnicodeBuf), "");
    copyToBuffer(m_authorBuf, sizeof(m_authorBuf), "Unknown");
    copyToBuffer(m_versionBuf, sizeof(m_versionBuf), "Easy");

    m_selectedAudioPath.clear();
    m_selectedAudioTrackId.clear();
    m_selectedCoverPath.clear();
    m_selectedCoverImgPath.clear();
    m_audioDuration = 0.0;

    m_createMode = CreateMode::Blank;
    m_templateCameraId.clear();
    m_templateDisplayName.clear();
    m_templateBeatmap.reset();
    m_templateOptions           = {};
    m_shouldOpenTemplatePicker  = false;
    m_shouldOpenTemplateOptions = false;
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

        copyToBuffer(m_titleBuf, sizeof(m_titleBuf), info.title);
        copyToBuffer(m_titleUnicodeBuf, sizeof(m_titleUnicodeBuf), info.title);
        copyToBuffer(m_artistBuf, sizeof(m_artistBuf), info.artist);
        copyToBuffer(
            m_artistUnicodeBuf, sizeof(m_artistUnicodeBuf), info.artist);

        // 自动设置谱面内部名称为标题（去空格）
        std::string safeName = info.title;
        safeName.erase(
            std::remove_if(safeName.begin(), safeName.end(), ::isspace),
            safeName.end());
        if ( !safeName.empty() ) {
            copyToBuffer(m_nameBuf, sizeof(m_nameBuf), safeName);
        }
    }
}

}  // namespace MMM::UI
