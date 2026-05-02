#include "ui/imgui/manager/AudioManagerView.h"
#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "mmm/project/AudioResource.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/layout/box/CLayBox.h"

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

    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) ImGui::PushFont(fileManagerFont);

    CLayVBox rootVBox;

    // 已打开项目时的界面
    CLayVBox listVBox;
    listVBox.setSpacing(4);

    // 渲染音轨列表项的辅助函数
    auto renderAudioItem = [&](const AudioResource& audio,
                               bool                 isPermanentEffect = false) {
        listVBox.addElement(
            "Audio_" + audio.m_id + "_" + audio.m_path,
            Sizing::Grow(),
            Sizing::Fixed(28),
            [&, audio](Clay_BoundingBox r, bool isHovered) {
                ImGui::Indent();
                std::string label = audio.m_id + "##" + audio.m_path;
                if ( ImGui::Selectable(label.c_str()) ) {
                    // 点击弹出控制器
                    std::string viewName = "TrackController_" + audio.m_id;
                    if ( !sourceManager->getView<AudioTrackControllerUI>(
                             viewName) ) {
                        auto controller = std::make_unique<
                            AudioTrackControllerUI>(
                            audio.m_id,
                            audio.m_id,
                            audio.m_type == AudioTrackType::Main
                                ? AudioTrackControllerUI::TrackType::Main
                                : AudioTrackControllerUI::TrackType::Effect);
                        sourceManager->registerView(viewName,
                                                    std::move(controller));
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

    // 1. 特效音轨常驻显示 (皮肤音效) - 始终显示
    auto& skinData = Config::SkinManager::instance().getData();
    if ( !skinData.audioPaths.empty() ) {
        listVBox.addElement(
            "PermanentSFXHeader",
            Sizing::Grow(),
            Sizing::Fixed(24),
            [](Clay_BoundingBox r, bool isHovered) {
                float indent = ImGui::CalcTextSize("AA").x;
                ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, indent);
                ImGui::SeparatorText(
                    TR("ui.audio_manager.permanent_sfx").data());
                ImGui::PopStyleVar();
            });

        for ( const auto& [key, path] : skinData.audioPaths ) {
            AudioResource res;
            res.m_id            = key;
            res.m_path          = path.string();
            res.m_type          = AudioTrackType::Effect;
            res.m_config.volume = audioManager.getSFXPoolVolume(key);
            res.m_config.muted  = audioManager.getSFXPoolMute(key);

            renderAudioItem(res, true);
        }
    }

    if ( project ) {
        // 2. 显示主音轨列表
        listVBox.addElement("AudioTracksHeader",
                            Sizing::Grow(),
                            Sizing::Fixed(24),
                            [](Clay_BoundingBox r, bool isHovered) {
                                float indent = ImGui::CalcTextSize("AA").x;
                                ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing,
                                                    indent);
                                ImGui::SeparatorText(
                                    TR("ui.audio_manager.audio_tracks").data());
                                ImGui::PopStyleVar();
                            });

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
                Sizing::Fixed(24),
                [](Clay_BoundingBox r, bool isHovered) {
                    float indent = ImGui::CalcTextSize("AA").x;
                    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, indent);
                    ImGui::SeparatorText(
                        TR("ui.audio_manager.project_sfx").data());
                    ImGui::PopStyleVar();
                });
            for ( const auto& audio : effectTracks ) {
                renderAudioItem(audio);
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

    footerVBox.addElement(
        "FooterSeparator",
        Sizing::Grow(),
        Sizing::Fixed(24),
        [](Clay_BoundingBox r, bool isHovered) {
            float indent = ImGui::CalcTextSize("AA").x;
            ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, indent);
            ImGui::SeparatorText(TR("ui.audio_manager.global_settings").data());
            ImGui::PopStyleVar();
        });

    footerVBox.addElement(
        "GlobalVolume",
        Sizing::Grow(),
        Sizing::Fixed(30),
        [&audioManager](Clay_BoundingBox r, bool isHovered) {
            ImGui::Indent();
            float  volume    = audioManager.getGlobalVolume();
            float  spacing   = ImGui::GetStyle().ItemInnerSpacing.x;
            ImVec2 labelSize = ImGui::CalcTextSize(
                TR("ui.audio_manager.global_volume").data());

            // 预留标签宽度，动态分配 Slider 宽度
            ImGui::SetNextItemWidth(std::max(10.0f,
                                             ImGui::GetContentRegionAvail().x -
                                                 labelSize.x - spacing -
                                                 10.0f));

            if ( ImGui::SliderFloat(TR("ui.audio_manager.global_volume").data(),
                                    &volume,
                                    0.0f,
                                    1.0f,
                                    "%.2f") ) {
                audioManager.setGlobalVolume(volume);
            }
            ImGui::Unindent();
        });

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
                                  ImGuiWindowFlags_HorizontalScrollbar);

                // 直接修改临时的 LayoutContext
                // 字段，而不触发其析构函数（LayoutContext 析构会调用 End()）
                // 方案：手动备份和恢复关键字段，而不是拷贝整个对象
                ImVec2 oldStartPos = layoutContext.m_startPos;
                ImVec2 oldAvail    = layoutContext.m_avail;

                layoutContext.m_startPos = ImGui::GetCursorScreenPos();
                layoutContext.m_avail    = { 2000.0f, 10000.0f };

                listVBox.render(layoutContext);

                // 强制撑开滚动区域
                ImGui::SetCursorScreenPos(layoutContext.m_startPos);
                ImGui::Dummy({ r.width, 1200.0f });

                // 恢复上下文状态
                layoutContext.m_startPos = oldStartPos;
                layoutContext.m_avail    = oldAvail;

                ImGui::EndChild();
            })
        .addLayout("footerVBox", footerVBox, Sizing::Grow(), Sizing::Fixed(64));
    rootVBox.render(layoutContext);

    if ( fileManagerFont ) ImGui::PopFont();
}
}  // namespace MMM::UI
