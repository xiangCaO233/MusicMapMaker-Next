#include "canvas/TimeFormatUtils.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>

namespace MMM::UI
{
/// @brief 渲染谱面设置页。
void SettingsView::drawBeatmapSettings()
{
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    auto*                                 project = engine.getCurrentProject();

    if ( !session || !session->getContext().currentBeatmap ) {
        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::TextColored(
            dangerCol, "%s", TR("ui.settings.beatmap.no_beatmap").data());
        return;
    }

    auto&       beatmap = *session->getContext().currentBeatmap;
    std::string currentPath =
        Config::pathToUtf8(beatmap.m_baseMapMetadata.map_path);
    if ( m_lastBeatmapPath != currentPath ) {
        m_editingMeta     = beatmap.m_baseMapMetadata;
        m_lastBeatmapPath = currentPath;
    } else {
        const auto& currentMeta = beatmap.m_baseMapMetadata;
        // 右侧工具栏和原始元数据编辑器可直接修改这些字段，设置页需要同步外部变更。
        m_editingMeta.track_count     = currentMeta.track_count;
        m_editingMeta.main_cover_path = currentMeta.main_cover_path;
        m_editingMeta.cover_path      = currentMeta.cover_path;
        m_editingMeta.cover_type      = currentMeta.cover_type;
        m_editingMeta.video_starttime = currentMeta.video_starttime;
        m_editingMeta.bgxoffset       = currentMeta.bgxoffset;
        m_editingMeta.bgyoffset       = currentMeta.bgyoffset;
    }
    auto& meta    = m_editingMeta;
    bool  changed = false;

    auto stripProjectFolderPrefix = [&](const std::filesystem::path& path) {
        if ( !project || project->m_projectRoot.empty() || path.empty() ||
             path.is_absolute() ) {
            return std::filesystem::path{};
        }

        auto iterator = path.begin();
        if ( iterator == path.end() ||
             *iterator != project->m_projectRoot.filename() ) {
            return std::filesystem::path{};
        }

        std::filesystem::path stripped;
        ++iterator;
        for ( ; iterator != path.end(); ++iterator ) {
            stripped /= *iterator;
        }
        return stripped.lexically_normal();
    };

    auto normalizeProjectResourcePath = [&](const std::filesystem::path& path) {
        if ( path.empty() || path.is_absolute() || !project ) {
            return path.lexically_normal();
        }

        const auto stripped = stripProjectFolderPrefix(path);
        if ( !stripped.empty() ) {
            std::error_code filesystemError;
            if ( std::filesystem::exists(project->m_projectRoot / stripped,
                                         filesystemError) &&
                 !filesystemError ) {
                return stripped.lexically_normal();
            }
        }
        return path.lexically_normal();
    };

    auto resolveProjectPath = [&](const std::filesystem::path& path) {
        if ( path.empty() || path.is_absolute() || !project ) {
            return path.lexically_normal();
        }

        auto directPath = (project->m_projectRoot / path).lexically_normal();
        std::error_code filesystemError;
        if ( std::filesystem::exists(directPath, filesystemError) &&
             !filesystemError ) {
            return directPath;
        }

        const auto stripped = stripProjectFolderPrefix(path);
        if ( !stripped.empty() ) {
            auto strippedPath =
                (project->m_projectRoot / stripped).lexically_normal();
            filesystemError.clear();
            if ( std::filesystem::exists(strippedPath, filesystemError) &&
                 !filesystemError ) {
                return strippedPath;
            }
        }

        return directPath;
    };

    auto displayProjectPath = [&](const std::filesystem::path& path) {
        if ( path.empty() ) return std::string{};
        if ( !project || path.is_relative() ) {
            return Config::pathToUtf8(normalizeProjectResourcePath(path));
        }

        std::error_code ec;
        auto            relativePath =
            std::filesystem::relative(path, project->m_projectRoot, ec);
        if ( !ec && !relativePath.empty() ) {
            return Config::pathToUtf8(relativePath);
        }
        return Config::pathToUtf8(path);
    };

    auto collectProjectResources =
        [&](std::initializer_list<std::string_view> allowedExtensions) {
            std::vector<std::string> resources;
            if ( !project ) return resources;

            std::error_code                               filesystemError;
            std::filesystem::recursive_directory_iterator it(
                project->m_projectRoot,
                std::filesystem::directory_options::skip_permission_denied,
                filesystemError);
            std::filesystem::recursive_directory_iterator end;
            if ( filesystemError ) return resources;

            for ( ; it != end; it.increment(filesystemError) ) {
                if ( filesystemError ) {
                    filesystemError.clear();
                    continue;
                }
                if ( !it->is_regular_file(filesystemError) ||
                     filesystemError ) {
                    filesystemError.clear();
                    continue;
                }

                auto ext = Config::pathToUtf8(it->path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
                    return static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c)));
                });
                const bool accepted = std::any_of(
                    allowedExtensions.begin(),
                    allowedExtensions.end(),
                    [&](std::string_view allowed) { return ext == allowed; });
                if ( !accepted ) continue;

                auto relativePath = std::filesystem::relative(
                    it->path(), project->m_projectRoot, filesystemError);
                if ( filesystemError ) {
                    filesystemError.clear();
                    continue;
                }
                resources.push_back(Config::pathToUtf8(relativePath));
            }
            return resources;
        };

    bool isImd = false;
    if ( !beatmap.m_baseMapMetadata.map_path.empty() ) {
        auto ext =
            Config::pathToUtf8(beatmap.m_baseMapMetadata.map_path.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if ( ext == ".imd" ) {
            isImd = true;
        }
    }

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 使用布局缓存中的统一标签列宽，避免设置页每帧重复测量全部标签。
    const float maxLabelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "MAP_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.beatmap.info").data(), true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        auto DrawInput = [&](const char*  labelPtr,
                             std::string& valRef,
                             bool         enabled = true) {
            addSettingItem(
                *sec,
                rowIndex,
                labelPtr,
                maxLabelW,
                [labelPtr, &valRef = valRef, &changed, enabled](
                    Clay_BoundingBox r, bool) {
                    ImGui::PushID(labelPtr);
                    if ( !enabled ) {
                        ImGui::BeginDisabled();
                    }
                    char buf[256];
                    strncpy(buf, valRef.c_str(), sizeof(buf));
                    buf[sizeof(buf) - 1] = '\0';
                    ImGui::SetNextItemWidth(r.width);
                    if ( ImGui::InputText("##Input", buf, sizeof(buf)) ) {
                        valRef  = buf;
                        changed = true;
                    }
                    if ( !enabled ) {
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                });
        };

        DrawInput(
            TR_CACHE("ui.settings.beatmap.name").data(), meta.name, !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.title").data(), meta.title, !isImd);
        DrawInput(TR_CACHE("ui.settings.beatmap.title_unicode").data(),
                  meta.title_unicode,
                  !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.artist").data(), meta.artist, !isImd);
        DrawInput(TR_CACHE("ui.settings.beatmap.artist_unicode").data(),
                  meta.artist_unicode,
                  !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.mapper").data(), meta.author, !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.version").data(), meta.version, true);

        std::string relativePathStr = "";
        std::string absolutePathStr = "";
        if ( !beatmap.m_baseMapMetadata.map_path.empty() ) {
            auto absolutePath =
                resolveProjectPath(beatmap.m_baseMapMetadata.map_path);
            absolutePathStr = Config::pathToUtf8(absolutePath);
            if ( project ) {
                relativePathStr =
                    displayProjectPath(beatmap.m_baseMapMetadata.map_path);
            } else {
                relativePathStr = absolutePathStr;
            }
        }

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.path").data(),
            maxLabelW,
            [relativePathStr, absolutePathStr](Clay_BoundingBox r, bool) {
                ImVec2 cursorPos = ImGui::GetCursorScreenPos();
                ImVec2 textSize  = ImGui::CalcTextSize(relativePathStr.c_str());

                // 计算滚动位移
                float offset       = 0.0f;
                float visibleWidth = r.width;

                if ( textSize.x > visibleWidth ) {
                    float scrollRange = textSize.x - visibleWidth + 40.0f;
                    float time        = (float)ImGui::GetTime();
                    // 平滑往复滚动，两端停顿
                    float t = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
                    t       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
                    offset  = t * scrollRange;
                }

                // 垂直居中计算
                float textH   = ImGui::GetFontSize();
                float widgetH = ImGui::GetFrameHeight();
                float offsetY = (widgetH - textH) * 0.5f;

                // 应用剪切矩形并绘制文本
                ImGui::PushClipRect(
                    cursorPos,
                    ImVec2(cursorPos.x + r.width, cursorPos.y + widgetH),
                    true);

                // 绘制一个透明的 dummy 控件以接收 hover 状态显示 Tooltip
                ImGui::SetCursorScreenPos(cursorPos);
                ImGui::Dummy(ImVec2(r.width, widgetH));
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s", absolutePathStr.c_str());
                }

                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(cursorPos.x - offset, cursorPos.y + offsetY),
                    ImGui::GetColorU32(ImGuiCol_Text),
                    relativePathStr.c_str());

                ImGui::PopClipRect();
            });
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.beatmap.cover_type").data(), true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        if ( isImd ) {
            ImGui::BeginDisabled();
        }

        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.beatmap.cover_type").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.beatmap.cover_type.image").data(), 0 },
              { TR_CACHE("ui.settings.beatmap.cover_type.video").data(), 1 } },
            (int&)meta.cover_type,
            changed);

        if ( meta.cover_type == MMM::CoverType::VIDEO ) {
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.beatmap.video_start").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               ImGui::SetNextItemWidth(r.width);
                               if ( ImGui::InputInt("##VideoStart",
                                                    &meta.video_starttime) ) {
                                   changed = true;
                               }
                           });
        }

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.bg_offset").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int offsets[2] = { meta.bgxoffset, meta.bgyoffset };
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragInt2("##BgOffset", offsets) ) {
                    meta.bgxoffset = offsets[0];
                    meta.bgyoffset = offsets[1];
                    changed        = true;
                }
            });

        if ( isImd ) {
            ImGui::EndDisabled();
        }
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.beatmap.preference").data(), true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.bpm").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                if ( isImd ) {
                    ImGui::BeginDisabled();
                }
                float bpm = (float)meta.preference_bpm;
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat(
                         "##BPM", &bpm, 0.1f, -1.0f, 1000.0f, "%.2f") ) {
                    meta.preference_bpm = (double)bpm;
                    changed             = true;
                }
                if ( isImd ) {
                    ImGui::EndDisabled();
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.tracks").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::InputInt("##Tracks", &meta.track_count) ) {
                    changed = true;
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.bgm_tracks").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                const auto bgmTrackCount =
                    std::max(0, session->getContext().bgmTrackCount);
                const float buttonSize       = ImGui::GetFrameHeight();
                const auto& style            = ImGui::GetStyle();
                const float buttonGlyphWidth = std::max(
                    ImGui::CalcTextSize("-").x, ImGui::CalcTextSize("+").x);
                const float maxHorizontalPadding = std::max(
                    0.0f, (buttonSize - buttonGlyphWidth) * 0.5f - 1.0f);
                const ImVec2 compactButtonPadding{
                    std::min(style.FramePadding.x, maxHorizontalPadding),
                    style.FramePadding.y,
                };
                // 紧凑方形按钮只收窄横向内边距，避免大 FramePadding
                // 主题裁掉加减号。
                const auto drawTrackCountButton = [&](const char* label) {
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                        compactButtonPadding);
                    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                        ImVec2(0.5f, 0.5f));
                    const bool clicked = ::MMM::UI::FeedbackButton(
                        label, ImVec2(buttonSize, buttonSize));
                    ImGui::PopStyleVar(2);
                    return clicked;
                };
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImGui::BeginDisabled(bgmTrackCount <= 0);
                if ( drawTrackCountButton("-##RemovePersistentBgmTrack") ) {
                    engine.pushCommand(Logic::CmdUpdateBgmTrackCount{
                        bgmTrackCount - 1,
                    });
                }
                ImGui::EndDisabled();
                if ( ImGui::IsItemHovered(
                         ImGuiHoveredFlags_AllowWhenDisabled) ) {
                    ImGui::SetTooltip(
                        "%s",
                        TR("ui.settings.beatmap.bgm_tracks_remove").data());
                }

                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%d", bgmTrackCount);
                ImGui::SameLine();

                const bool canAdd =
                    bgmTrackCount < std::numeric_limits<std::int32_t>::max();
                ImGui::BeginDisabled(!canAdd);
                if ( drawTrackCountButton("+##AddPersistentBgmTrack") ) {
                    engine.pushCommand(Logic::CmdUpdateBgmTrackCount{
                        bgmTrackCount + 1,
                    });
                }
                ImGui::EndDisabled();
                if ( ImGui::IsItemHovered(
                         ImGuiHoveredFlags_AllowWhenDisabled) ) {
                    ImGui::SetTooltip(
                        "%s", TR("ui.settings.beatmap.bgm_tracks_add").data());
                }
            });

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.beatmap.length").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           const auto lengthText = Canvas::formatCanvasDuration(
                               meta.map_length / 1000.0);
                           ImGui::SetCursorScreenPos({ r.x, r.y });
                           ImGui::TextUnformatted(lengthText.c_str());
                       });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.beatmap.resource").data(),
                               true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        if ( isImd ) {
            ImGui::BeginDisabled();
        }

        // 音频选择
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.audio").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                /// @brief 歌曲文件提示只服务于外部格式，不决定实际播放时间线。
                const auto& audioHint        = meta.song_file_hint.empty()
                                                   ? meta.main_audio_path
                                                   : meta.song_file_hint;
                std::string currentAudioPath = displayProjectPath(audioHint);
                std::string audioPreview     = currentAudioPath;

                bool audioExists =
                    project &&
                    std::filesystem::exists(resolveProjectPath(audioHint));
                bool audioPushed = false;
                if ( !audioExists && !currentAudioPath.empty() ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    audioPushed = true;
                }

                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo("##AudioCombo",
                                                   audioPreview.c_str()) ) {
                    if ( audioPushed ) {
                        ImGui::PopStyleColor();
                        audioPushed = false;
                    }
                    if ( project ) {
                        for ( const auto& res : project->m_audioResources ) {
                            if ( res.m_type != MMM::AudioTrackType::Main )
                                continue;
                            bool isSelected = (currentAudioPath == res.m_path);
                            if ( ::MMM::UI::FeedbackSelectable(
                                     (res.m_id + "##" + res.m_path).c_str(),
                                     isSelected) ) {
                                meta.song_file_hint =
                                    normalizeProjectResourcePath(
                                        Config::utf8ToPath(res.m_path));
                                meta.main_audio_path.clear();
                                changed = true;
                            }
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                if ( audioPushed ) ImGui::PopStyleColor();
            });

        // 封面选择
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.cover").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                std::string currentCoverPath =
                    displayProjectPath(meta.cover_path);
                std::string coverPreview = currentCoverPath;

                std::error_code coverExistsError;
                bool            coverExists =
                    project &&
                    std::filesystem::exists(resolveProjectPath(meta.cover_path),
                                            coverExistsError) &&
                    !coverExistsError;
                bool coverPushed = false;
                if ( !coverExists && !currentCoverPath.empty() ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    coverPushed = true;
                }

                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo("##CoverCombo",
                                                   coverPreview.c_str()) ) {
                    if ( coverPushed ) {
                        ImGui::PopStyleColor();
                        coverPushed = false;
                    }
                    if ( project ) {
                        std::vector<std::string> images =
                            collectProjectResources(
                                { ".png", ".jpg", ".jpeg", ".bmp" });

                        for ( const auto& imgPath : images ) {
                            bool isSelected = (currentCoverPath == imgPath);
                            if ( ::MMM::UI::FeedbackSelectable(
                                     (imgPath + "##" + imgPath).c_str(),
                                     isSelected) ) {
                                meta.cover_path = Config::utf8ToPath(imgPath);
                                changed         = true;
                            }
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                if ( coverPushed ) ImGui::PopStyleColor();
            });

        // 背景选择
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.background").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                std::string currentBgPath =
                    displayProjectPath(meta.main_cover_path);
                std::string bgPreview = currentBgPath;

                std::error_code bgExistsError;
                const bool      bgExists =
                    project &&
                    std::filesystem::exists(
                        resolveProjectPath(meta.main_cover_path),
                        bgExistsError) &&
                    !bgExistsError;
                bool bgPushed = false;
                if ( !bgExists && !currentBgPath.empty() ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    bgPushed = true;
                }

                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo("##BgCombo",
                                                   bgPreview.c_str()) ) {
                    if ( bgPushed ) {
                        ImGui::PopStyleColor();
                        bgPushed = false;
                    }
                    if ( project ) {
                        // 背景类型是用户的显式选择，下拉项只展示同类型资源。
                        std::vector<std::string> backgroundResources;
                        if ( meta.cover_type == MMM::CoverType::VIDEO ) {
                            backgroundResources =
                                collectProjectResources({ ".mp4",
                                                          ".avi",
                                                          ".mkv",
                                                          ".webm",
                                                          ".mov",
                                                          ".flv",
                                                          ".m4v" });
                        } else {
                            backgroundResources = collectProjectResources(
                                { ".png", ".jpg", ".jpeg", ".bmp" });
                        }

                        for ( const auto& backgroundPath :
                              backgroundResources ) {
                            bool isSelected = (currentBgPath == backgroundPath);
                            if ( ::MMM::UI::FeedbackSelectable(
                                     (backgroundPath + "##" + backgroundPath)
                                         .c_str(),
                                     isSelected) ) {
                                auto chosenPath =
                                    Config::utf8ToPath(backgroundPath);
                                meta.main_cover_path = chosenPath;
                                changed              = true;

                                // 如果背景是图片且封面为空，则自动沿用同一张图片。
                                auto ext =
                                    Config::pathToUtf8(chosenPath.extension());
                                std::transform(ext.begin(),
                                               ext.end(),
                                               ext.begin(),
                                               ::tolower);
                                if ( ext == ".png" || ext == ".jpg" ||
                                     ext == ".jpeg" || ext == ".bmp" ) {
                                    if ( meta.cover_path.empty() ) {
                                        meta.cover_path = chosenPath;
                                    }
                                }
                            }
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                if ( bgPushed ) ImGui::PopStyleColor();
            });

        if ( isImd ) {
            ImGui::EndDisabled();
        }
    }

    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( changed ) {
        engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ meta });
    }
}

}  // namespace MMM::UI
