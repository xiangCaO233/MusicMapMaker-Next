#include "ui/imgui/manager/AudioManagerView.h"
#include "audio/AudioManager.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "mmm/project/AudioResource.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <nfd.h>

namespace MMM::UI
{

// 内部绘制逻辑 (Clay/ImGui)
void AudioManagerView::onUpdate(LayoutContext& layoutContext,
                                UIManager*     sourceManager)
{
    auto& engine       = Logic::EditorEngine::instance();
    auto* project      = engine.getCurrentProject();
    auto& skinCfg      = Config::SkinManager::instance();
    auto& audioManager = Audio::AudioManager::instance();

    float   dpiScale        = layoutContext.m_dpiScale;
    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) ImGui::PushFont(fileManagerFont);

    CLayVBox rootVBox;

    // 已打开项目时的界面
    CLayVBox listVBox;
    listVBox.setSpacing(4);

    size_t rowIndex     = 0;
    size_t subHBoxIndex = 0;

    // 渲染一行音频控制项 (Label + Mute + Slider)
    auto addControlRow = [&](CLayVBox&   parent,
                             const char* id,
                             const char* label,
                             float       labelWidth,
                             float       volume,
                             bool        muted,
                             float       levelL,
                             float       levelR,
                             float       minVal,
                             float       maxVal,
                             const char* tooltip,
                             const char* format,
                             auto        onVolumeChange,
                             auto        onMuteChange) {
        // 创建水平行布局 (主容器)
        CLayHBox& row = this->getRow(rowIndex++);
        row.clear();
        row.setPadding(0, 0, 0, 0)
            .setSpacing(8)
            .setAlignment(Alignment::Center());

        // --- A. 左侧容器: (FixW) [ 标签 + 弹簧 ] ---
        CLayHBox& leftBox = this->getSubHBox(subHBoxIndex++);
        leftBox.clear();
        leftBox.setSpacing(4).setAlignment(Alignment::Center());

        // 1. 标签
        leftBox.addElement(std::string(id) + "_lbl",
                           Sizing::Fixed(ImGui::CalcTextSize(label).x),
                           Sizing::Grow(),
                           [label](Clay_BoundingBox r, bool) {
                               float textH  = ImGui::CalcTextSize(label).y;
                               float offset = (r.height - textH) * 0.5f;
                               ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                               ImGui::Text("%s", label);
                           });

        // 2. 弹簧 (填充 FixW 剩余空间)
        leftBox.addSpring();

        // 将左侧容器加入主行
        row.addLayout((std::string(id) + "_left").c_str(),
                      leftBox,
                      Sizing::Fixed(labelWidth),
                      Sizing::Grow());

        // --- B. 右侧容器: (GrowW) [ 按钮 + 滑条 ] ---
        CLayHBox& rightBox = this->getSubHBox(subHBoxIndex++);
        rightBox.clear();
        rightBox.setSpacing(8).setAlignment(Alignment::Center());

        // 1. 静音按钮
        rightBox.addElement(
            std::string(id) + "_mute",
            Sizing::Fixed(32),
            Sizing::Fixed(30),
            [&, id, muted, volume, tooltip, onMuteChange](Clay_BoundingBox r,
                                                          bool isHovered) {
                const char* icon = ICON_MMM_VOLUME_MUTE;
                if ( !muted ) {
                    if ( volume <= 0.33f )
                        icon = ICON_MMM_VOLUME_OFF;
                    else if ( volume <= 0.66f )
                        icon = ICON_MMM_VOLUME_LOW;
                    else
                        icon = ICON_MMM_VOLUME_HIGH;
                }

                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - 30) * 0.5f });
                if ( muted ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getDangerColor());
                }
                if ( ImGui::Button((std::string(icon) + "##Btn" + id).c_str(),
                                   ImVec2(32, 30)) ) {
                    onMuteChange(!muted);
                }
                if ( muted ) {
                    ImGui::PopStyleColor();
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s (%s)",
                                      tooltip,
                                      muted
                                          ? TR("ui.audio_manager.unmute").data()
                                          : TR("ui.audio_manager.mute").data());
                }
            });

        // 2. 拉条 (自动延伸)
        rightBox.addElement(
            std::string(id) + "_slider",
            Sizing::Grow(),
            Sizing::Fixed(30),
            [&, id, volume, minVal, maxVal, format, tooltip, onVolumeChange](
                Clay_BoundingBox r, bool isHovered) {
                float frameH = ImGui::GetFrameHeight();
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - frameH) * 0.5f });
                ImGui::SetNextItemWidth(r.width);
                float val = volume;
                if ( ImGui::SliderFloat((std::string("##Slider") + id).c_str(),
                                        &val,
                                        minVal,
                                        maxVal,
                                        format) ) {
                    onVolumeChange(val);
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s", tooltip);
                }
            });

        // 将右侧容器加入主行
        row.addLayout((std::string(id) + "_right").c_str(),
                      rightBox,
                      Sizing::Grow(),
                      Sizing::Grow());

        parent.addLayout((std::string(id) + "_row").c_str(),
                         row,
                         Sizing::Grow(),
                         Sizing::Fixed(32));
    };

    // 渲染音轨列表项的辅助函数
    auto renderAudioItem = [&](const AudioResource& audio,
                               bool                 isPermanentEffect = false) {
        listVBox.addElement(
            "Audio_" + audio.m_id + "_" + audio.m_path,
            Sizing::Grow(),
            Sizing::Fixed(28 * dpiScale),
            [=, &engine, this](Clay_BoundingBox r, bool isHovered) {
                ImGui::Indent();
                std::string labelStr = audio.m_id + " - " + audio.m_path;
                float       availW   = ImGui::GetContentRegionAvail().x;

                Utils::renderScrollingSelectable(
                    audio.m_id, labelStr, availW, 28 * dpiScale, [&]() {
                        // 点击弹出控制器
                        std::string viewName = "TrackController_" + audio.m_id;
                        if ( !sourceManager->getView<AudioTrackControllerUI>(
                                 viewName) ) {
                            AudioTrackControllerUI::TrackType type =
                                (audio.m_type == AudioTrackType::Main)
                                    ? AudioTrackControllerUI::TrackType::Main
                                    : AudioTrackControllerUI::TrackType::Effect;
                            std::unique_ptr<IUIView> controller =
                                std::make_unique<AudioTrackControllerUI>(
                                    audio.m_id, audio.m_id, type);
                            sourceManager->registerView(viewName,
                                                        std::move(controller));
                        }
                    });

                static int s_audioLogCounter = 0;
                if ( !isPermanentEffect ) {
                    bool hovered = ImGui::IsItemHovered();
                    bool rclicked =
                        ImGui::IsMouseClicked(ImGuiMouseButton_Right);
                    if ( (hovered || rclicked) && s_audioLogCounter < 10 ) {
                        XINFO(
                            "AudioItem [{}]: hovered={} rclicked={} "
                            "isPermanent={} pos=({},{}) size=({},{}) "
                            "mouse=({},{})",
                            audio.m_id,
                            hovered,
                            rclicked,
                            isPermanentEffect,
                            r.x,
                            r.y,
                            r.width,
                            r.height,
                            ImGui::GetMousePos().x,
                            ImGui::GetMousePos().y);
                        s_audioLogCounter++;
                    }
                    if ( hovered && rclicked ) {
                        XINFO("AudioItem RIGHT-CLICK TRIGGERED: {}",
                              audio.m_id);
                        m_manageTrackId   = audio.m_id;
                        m_manageTrackType = audio.m_type;
                        m_openManageModal = true;
                    }
                }

                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s (Type: %s)",
                                      audio.m_path.c_str(),
                                      audio.m_type == AudioTrackType::Main
                                          ? "Main"
                                          : "Effect");
                }
                ImGui::Unindent();
            });
    };

    // 1. 特效音轨常驻显示 (皮肤音效)
    auto& skinData = Config::SkinManager::instance().getData();
    if ( !skinData.audioPaths.empty() ) {
        listVBox.addElement("PermanentSFXHeader",
                            Sizing::Grow(),
                            Sizing::Fixed(ImGui::GetFrameHeight()),
                            [&](Clay_BoundingBox r, bool isHovered) {
                                Utils::renderCollapsingHeader(
                                    TR("ui.audio_manager.permanent_sfx").data(),
                                    &m_showPermanentSFX,
                                    r);
                            });

        if ( m_showPermanentSFX ) {
            for ( const auto& [key, path] : skinData.audioPaths ) {
                AudioResource res;
                res.m_id            = key;
                res.m_path          = Config::pathToUtf8(path);
                res.m_type          = AudioTrackType::Effect;
                res.m_config.volume = audioManager.getSFXPoolVolume(key);
                res.m_config.muted  = audioManager.getSFXPoolMute(key);

                renderAudioItem(res, true);
            }
        }
    }

    if ( project ) {
        // 2. 显示主音轨列表
        listVBox.addElement("AudioTracksHeader",
                            Sizing::Grow(),
                            Sizing::Fixed(ImGui::GetFrameHeight()),
                            [&](Clay_BoundingBox r, bool isHovered) {
                                Utils::renderCollapsingHeader(
                                    TR("ui.audio_manager.audio_tracks").data(),
                                    &m_showMainTracks,
                                    r);
                            });

        if ( m_showMainTracks ) {
            std::vector<AudioResource> mainTracks;
            std::vector<AudioResource> effectTracks;
            for ( const auto& audio : project->m_audioResources ) {
                if ( audio.m_type == AudioTrackType::Main ) {
                    mainTracks.push_back(audio);
                } else {
                    effectTracks.push_back(audio);
                }
            }

            for ( const auto& audio : mainTracks ) {
                renderAudioItem(audio);
            }

            // 3. 显示项目特效音轨
            if ( !effectTracks.empty() ) {
                listVBox.addElement(
                    "ProjectSFXHeader",
                    Sizing::Grow(),
                    Sizing::Fixed(ImGui::GetFrameHeight()),
                    [&](Clay_BoundingBox r, bool isHovered) {
                        Utils::renderCollapsingHeader(
                            TR("ui.audio_manager.project_sfx").data(),
                            &m_showProjectSFX,
                            r);
                    });

                if ( m_showProjectSFX ) {
                    for ( const auto& audio : effectTracks ) {
                        renderAudioItem(audio);
                    }
                }
            }
        }
    } else {
        // 未打开项目时的提示
        listVBox.addElement("InitialHintSpacer",
                            Sizing::Grow(),
                            Sizing::Fixed(20),
                            [](Clay_BoundingBox, bool) {});
        listVBox.addElement("InitialHint",
                            Sizing::Grow(),
                            Sizing::Fixed(30),
                            [=](Clay_BoundingBox r, bool isHovered) {
                                ImGui::Indent();
                                ImGui::TextDisabled(
                                    "%s",
                                    TR("ui.audio_manager.initial_hint").data());
                                ImGui::Unindent();
                            });
    }

    // 底部全局控制 - 始终显示
    CLayVBox footerVBox;
    // 使用对称的水平内边距，移除手动 Indent，确保左右居中对齐
    footerVBox.setPadding(16, 16, 0, 0).setSpacing(2);

    footerVBox.addElement("FooterHeader",
                          Sizing::Grow(),
                          Sizing::Fixed(ImGui::GetFrameHeight()),
                          [&](Clay_BoundingBox r, bool isHovered) {
                              Utils::renderCollapsingHeader(
                                  TR("ui.audio_manager.global_settings").data(),
                                  &m_showGlobalSettings,
                                  r);
                          });

    if ( m_showGlobalSettings ) {
        footerVBox.addSpring();  // 顶部弹簧，实现垂直居中

        // 计算标签宽度 (FixW)
        float maxLabelW = 0;
        maxLabelW       = std::max(
            maxLabelW,
            ImGui::CalcTextSize(TR("ui.audio_manager.global_volume").data()).x);
        maxLabelW = std::max(
            maxLabelW,
            ImGui::CalcTextSize(TR("ui.audio_manager.bgm_gain").data()).x);
        maxLabelW = std::max(
            maxLabelW,
            ImGui::CalcTextSize(TR("ui.audio_manager.sfx_gain").data()).x);
        maxLabelW += 12.0f;  // 额外间距 (预留弹簧运动空间)

        addControlRow(
            footerVBox,
            "Global",
            TR("ui.audio_manager.global_volume").data(),
            maxLabelW,
            audioManager.getGlobalVolume(),
            audioManager.isGlobalMuted(),
            audioManager.getOutputLevelL(),
            audioManager.getOutputLevelR(),
            0.0f,
            1.0f,
            TR("ui.audio_manager.global_volume").data(),
            "%.2f",
            [&](float v) { audioManager.setGlobalVolume(v); },
            [&](bool m) { audioManager.setGlobalMute(m); });

        addControlRow(
            footerVBox,
            "BGMGain",
            TR("ui.audio_manager.bgm_gain").data(),
            maxLabelW,
            audioManager.getBGMGain(),
            audioManager.isBGMGainMuted(),
            audioManager.getMainTrackLevelL(),
            audioManager.getMainTrackLevelR(),
            0.0f,
            1.0f,
            TR("ui.audio_manager.bgm_gain").data(),
            "%.2f",
            [&](float v) { audioManager.setBGMGain(v); },
            [&](bool m) { audioManager.setBGMGainMute(m); });

        addControlRow(
            footerVBox,
            "SFXGain",
            TR("ui.audio_manager.sfx_gain").data(),
            maxLabelW,
            audioManager.getSFXGain(),
            audioManager.isSFXGainMuted(),
            0.0f,
            0.0f,
            0.0f,
            1.0f,
            TR("ui.audio_manager.sfx_gain").data(),
            "%.2f",
            [&](float v) { audioManager.setSFXGain(v); },
            [&](bool m) { audioManager.setSFXGainMute(m); });

        footerVBox.addSpring();  // 底部弹簧
    }

    // --- 执行分段渲染 ---
    // 1. 动态计算页脚高度
    float footerH = ImGui::GetFrameHeightWithSpacing();
    if ( m_showGlobalSettings ) {
        // 3个32px的项目 + 间距 + 上下预留的缓冲空间
        footerH += 3 * 32.0f + 3 * 2.0f + 16.0f;
    }
    // 3. 底部导入按钮 (32px + 间距)
    footerH += 32.0f + 8.0f;

    // 2. 渲染顶部列表区域 (自动占据剩余空间)
    rootVBox.setPadding(12, 12, 12, 12)
        .setSpacing(12)
        .addElement(
            "listContentArea",
            Sizing::Grow(),
            Sizing::Grow(),
            [&listVBox, &layoutContext](Clay_BoundingBox r, bool isHovered) {
                ImGui::BeginChild("AudioListChild",
                                  { r.width, r.height },
                                  false,
                                  ImGuiWindowFlags_None);

                ImVec2 oldStartPos = layoutContext.m_startPos;
                ImVec2 oldAvail    = layoutContext.m_avail;

                layoutContext.m_startPos = ImGui::GetCursorScreenPos();
                layoutContext.m_avail    = { r.width, 10000.0f };

                listVBox.render(layoutContext);

                layoutContext.m_startPos = oldStartPos;
                layoutContext.m_avail    = oldAvail;

                ImGui::EndChild();
            });

    ImVec2 totalSize = rootVBox.renderInCurrent(
        layoutContext.m_startPos,
        { layoutContext.m_avail.x, layoutContext.m_avail.y - footerH });

    // 3. 底部全局控制区域 (独立渲染)
    ImVec2 footerPos = { layoutContext.m_startPos.x,
                         layoutContext.m_startPos.y + totalSize.y };

    float controlH = footerH - (32.0f + 8.0f);
    footerVBox.renderInCurrent(footerPos,
                               { layoutContext.m_avail.x, controlH });

    // 4. 底部加号按钮 (全宽)
    CLayHBox bottomBtnHBox;
    bottomBtnHBox.setPadding(12, 12, 0, 0)
        .setAlignment(Alignment::Center())
        .addElement(
            "Audio_ImportNew",
            Sizing::Grow(),
            Sizing::Fixed(32.0f),
            [&engine](Clay_BoundingBox r, bool isHovered) {
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(1, 1, 1, 0.2f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImDrawList* dl    = ImGui::GetWindowDrawList();
                ImVec4      bgCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
                bgCol.w *= 0.5f;
                float rounding = ImGui::GetStyle().FrameRounding;

                if ( isHovered ) bgCol.w *= 1.5f;

                dl->AddRectFilled({ r.x, r.y },
                                  { r.x + r.width, r.y + r.height },
                                  ImGui::ColorConvertFloat4ToU32(bgCol),
                                  rounding);

                if ( ImGui::Button(
                         fmt::format("{}##ImportAudio", ICON_MMM_PLUS).c_str(),
                         ImVec2(r.width, r.height)) ) {
                    auto& settings = engine.getEditorConfig().settings;
                    if ( settings.filePickerStyle ==
                         Config::FilePickerStyle::Native ) {
                        nfdu8char_t*      outPath    = nullptr;
                        nfdu8filteritem_t filters[1] = {
                            { "Audio Files", "mp3,ogg,wav,flac" }
                        };
                        nfdresult_t result =
                            NFD_OpenDialogU8(&outPath, filters, 1, nullptr);

                        if ( result == NFD_OKAY ) {
                            Event::EventBus::instance().publish(
                                Event::AudioImportTriggerEvent{ outPath });
                            NFD_FreePathU8(outPath);
                        } else if ( result == NFD_ERROR ) {
                            XERROR("NFD Error: {}", NFD_GetError());
                        }
                    } else {
                        IGFD::FileDialogConfig fdConfig;
                        fdConfig.path     = settings.lastFilePickerPath;
                        fdConfig.fileName = "";
                        fdConfig.flags =
                            ImGuiFileDialogFlags_Modal |
                            ImGuiFileDialogFlags_HideColumnType |
                            ImGuiFileDialogFlags_ReadOnlyFileNameField;
                        ImGuiFileDialog::Instance()->OpenDialog(
                            "AudioImportPicker",
                            TR("ui.audio_manager.import_audio").data(),
                            ".mp3,.ogg,.wav,.flac",
                            fdConfig);
                    }
                }

                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip(
                        "%s", TR("ui.audio_manager.import_audio").data());
                }
            });

    ImVec2 btnPos = { footerPos.x, footerPos.y + controlH + 4.0f };
    bottomBtnHBox.renderInCurrent(btnPos, { layoutContext.m_avail.x, 32.0f });

    // --- 5. 音轨管理窗口 ---
    bool showManageModal = !m_manageTrackId.empty();
    if ( showManageModal ) {
        std::string windowTitle =
            fmt::format("{} {}",
                        TR("ui.audio_manager.manage_title").data(),
                        m_manageTrackId);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize({ 420 * dpiScale, 0 }, ImGuiCond_Appearing);
        if ( ImGui::Begin(windowTitle.c_str(),
                          &showManageModal,
                          ImGuiWindowFlags_NoCollapse) ) {
            if ( !showManageModal ) {
                m_manageTrackId = "";
            }

            // --- 使用 Clay 重构对话框内容 ---
            CLayVBox modalLayout;
            float    padding = 16 * dpiScale;
            modalLayout.setPadding(padding, padding, padding, padding);
            modalLayout.setSpacing(12 * dpiScale);

            // 1. 标题与分隔线 (移除了冗余的 Text，仅保留分隔线)
            modalLayout.addElement(
                "ModalSep1",
                Sizing::Grow(),
                Sizing::Fixed(1),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::GetWindowDrawList()->AddLine(
                        { r.x, r.y },
                        { r.x + r.width, r.y },
                        ImGui::GetColorU32(ImGuiCol_Separator));
                });

            // 2. 配置项 (音轨类型)
            CLayHBox typeRow;
            typeRow.setAlignment(Alignment::Center());
            typeRow.setSpacing(8 * dpiScale);
            typeRow.addElement(
                "TypeLabel",
                Sizing::Fixed(100 * dpiScale),
                Sizing::Grow(),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("%s:",
                                TR("ui.audio_manager.track_type").data());
                });
            typeRow.addElement(
                "TypeCombo",
                Sizing::Grow(),
                Sizing::Fixed(28 * dpiScale),
                [=, this, &engine](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    int currentType =
                        (m_manageTrackType == AudioTrackType::Main ? 0 : 1);
                    const char* typeNames[] = { "Main", "Effect" };
                    ImGui::SetNextItemWidth(r.width);
                    if ( ImGui::Combo(
                             "##TrackType", &currentType, typeNames, 2) ) {
                        m_manageTrackType =
                            (currentType == 0 ? AudioTrackType::Main
                                              : AudioTrackType::Effect);
                        engine.pushCommand(Logic::CmdUpdateAudioResource{
                            m_manageTrackId, m_manageTrackType });
                        ImGui::CloseCurrentPopup();
                    }
                });
            modalLayout.addLayout("TypeRow",
                                  typeRow,
                                  Sizing::Grow(),
                                  Sizing::Fixed(32 * dpiScale));

            modalLayout.addElement(
                "ModalSep2",
                Sizing::Grow(),
                Sizing::Fixed(1),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::GetWindowDrawList()->AddLine(
                        { r.x, r.y },
                        { r.x + r.width, r.y },
                        ImGui::GetColorU32(ImGuiCol_Separator));
                });

            // 3. 操作按钮
            CLayHBox btnRow;
            btnRow.setAlignment(Alignment::Center());
            btnRow.setSpacing(12 * dpiScale);
            btnRow.addElement(
                "RemoveBtn",
                Sizing::Fixed(140 * dpiScale),
                Sizing::Fixed(32 * dpiScale),
                [=](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ImGui::Button(
                             TR("ui.audio_manager.remove_track").data(),
                             { r.width, r.height }) ) {
                        ImGui::OpenPopup("RemoveTrackConfirm");
                    }
                });
            btnRow.addElement(
                "CancelBtn",
                Sizing::Fixed(100 * dpiScale),
                Sizing::Fixed(32 * dpiScale),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ImGui::Button(TR("ui.common.cancel").data(),
                                       { r.width, r.height }) ) {
                        m_manageTrackId = "";
                    }
                });
            modalLayout.addLayout(
                "BtnRow", btnRow, Sizing::Grow(), Sizing::Fixed(32 * dpiScale));

            // 渲染布局
            // 注意：在模态框中使用 renderInCurrent 来适配 ImGui 的自动大小计算
            ImVec2 modalSize = modalLayout.renderInCurrent(
                ImGui::GetCursorScreenPos(), { 400 * dpiScale, 0 });
            ImGui::Dummy(modalSize);

            // 二次确认弹窗
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                    ImGuiCond_Appearing,
                                    ImVec2(0.5f, 0.5f));
            if ( ImGui::BeginPopupModal(
                     "RemoveTrackConfirm", nullptr, ImGuiWindowFlags_None) ) {
                ImGui::Text("%s", TR("ui.audio_manager.remove_confirm").data());
                ImGui::Spacing();
                if ( ImGui::Button(TR("ui.common.confirm").data(),
                                   { 100 * dpiScale, 0 }) ) {
                    engine.pushCommand(
                        Logic::CmdRemoveAudioResource{ m_manageTrackId });
                    m_manageTrackId = "";
                }
                ImGui::SameLine();
                if ( ImGui::Button(TR("ui.common.cancel").data(),
                                   { 100 * dpiScale, 0 }) ) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::End();
        }
    }

    if ( fileManagerFont ) ImGui::PopFont();
}
}  // namespace MMM::UI
