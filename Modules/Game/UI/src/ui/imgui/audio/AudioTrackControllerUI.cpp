#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "audio/AudioManager.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "implot.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"

namespace MMM::UI
{

AudioTrackControllerUI::AudioTrackControllerUI(const std::string& trackId,
                                               const std::string& trackName,
                                               TrackType          type)
    : IUIView(trackName)
    , m_trackId(trackId)
    , m_trackName(trackName)
    , m_type(type)
{
}

void AudioTrackControllerUI::update(UIManager* sourceManager)
{
    if ( !m_isOpen ) {
        return;
    }

    auto& audio   = Audio::AudioManager::instance();
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();

    ImGui::SetNextWindowSize(ImVec2(350, 450), ImGuiCond_FirstUseEver);
    if ( ImGui::Begin(m_trackName.c_str(), &m_isOpen) ) {
        float volume = 0.5f;
        float speed  = 1.0f;
        float pitch  = 0.0f;
        bool  muted  = false;

        AudioTrackConfig* config = nullptr;
        if ( project ) {
            for ( auto& res : project->m_audioResources ) {
                if ( res.m_id == m_trackId ) {
                    config = &res.m_config;
                    break;
                }
            }
        }

        if ( config ) {
            volume = config->volume;
            speed  = config->playbackSpeed;
            pitch  = config->playbackPitch;
            muted  = config->muted;
            if ( m_type == TrackType::Main ) {
                m_currentPreset =
                    config->eqEnabled
                        ? static_cast<Audio::EQPreset>(config->eqPreset)
                        : Audio::EQPreset::None;
            }
        } else {
            // 回退到内存状态
            if ( m_type == TrackType::Main ) {
                volume = audio.getMainTrackVolume();
                speed  = (float)audio.getPlaybackSpeed();
                pitch  = (float)audio.getPlaybackPitch();
                muted  = audio.isMainTrackMuted();
            } else {
                volume = audio.getSFXPoolVolume(m_trackId);
                muted  = audio.getSFXPoolMute(m_trackId);
            }
        }

        bool changed = false;

        // --- 1. 静音按钮与音量图标 ---
        const char* icon = ICON_MMM_VOLUME_MUTE;
        if ( !muted ) {
            if ( volume <= 0.33f )
                icon = ICON_MMM_VOLUME_OFF;
            else if ( volume <= 0.66f )
                icon = ICON_MMM_VOLUME_LOW;
            else
                icon = ICON_MMM_VOLUME_HIGH;
        }

        bool pushedTextColor = false;
        if ( muted ) {
            auto dangerCol =
                Config::SkinManager::instance().getColor("ui.danger");
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                ImVec4(dangerCol.r, dangerCol.g, dangerCol.b, dangerCol.a));
            pushedTextColor = true;
        }
        auto btnTransCol =
            Config::SkinManager::instance().getColor("ui.button.transparent");
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImVec4(btnTransCol.r, btnTransCol.g, btnTransCol.b, btnTransCol.a));
        if ( ImGui::Button(icon, ImVec2(30, 0)) ) {
            muted   = !muted;
            changed = true;
        }
        ImGui::PopStyleColor();  // Pop ImGuiCol_Button
        if ( pushedTextColor ) {
            ImGui::PopStyleColor();  // Pop ImGuiCol_Text
        }
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetTooltip("%s",
                              muted ? TR("ui.audio_manager.unmute").data()
                                    : TR("ui.audio_manager.mute").data());
        }

        ImGui::SameLine();

        ImGui::AlignTextToFramePadding();

        // --- 2. 音量拉条 ---
        ImGui::SetNextItemWidth(-50);
        if ( ImGui::SliderFloat("##Volume", &volume, 0.0f, 1.0f, "%.2f") ) {
            changed = true;
            // 如果拉动了进度条，自动解除静音（可选，但通常用户期望这样）
            if ( muted && volume > 0.0f ) {
                muted = false;
            }
        }

        // --- 3. 播放速度与均衡器控制 (仅对 Main 有效) ---
        if ( m_type == TrackType::Main ) {
            ImGui::Separator();
            ImGui::Text("%s", TR("ui.audio_manager.speed_control").data());

            // 显示期望和实际倍率
            float actualSpeed = (float)audio.getActualPlaybackSpeed();
            ImGui::Text(
                TR("ui.audio_manager.speed_info").data(), speed, actualSpeed);

            // 预设按钮
            if ( ImGui::Button(TR("ui.audio_manager.speed_025x").data()) ) {
                speed   = 0.25f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.speed_050x").data()) ) {
                speed   = 0.5f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.speed_075x").data()) ) {
                speed   = 0.75f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.speed_100x").data()) ) {
                speed   = 1.0f;
                changed = true;
            }

            // 滑块
            if ( ImGui::SliderFloat(
                     "##SpeedSlider", &speed, 0.25f, 2.0f, "%.4fx") ) {
                changed = true;
            }

            // --- 3.5 音高控制 ---
            ImGui::Separator();
            ImGui::Text("%s", TR("ui.audio_manager.pitch_control").data());

            // 预设按钮
            if ( ImGui::Button(TR("ui.audio_manager.pitch_n24").data()) ) {
                pitch   = -24.0f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.pitch_n12").data()) ) {
                pitch   = -12.0f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.pitch_n5").data()) ) {
                pitch   = -5.0f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.pitch_0").data()) ) {
                pitch   = 0.0f;
                changed = true;
            }

            // 滑块
            if ( ImGui::SliderFloat(
                     "##PitchSlider", &pitch, -24.0f, 24.0f, "%.1f st") ) {
                changed = true;
            }

            // --- 3.6 图形均衡器 (Graphic EQ) ---
            ImGui::Separator();
            ImGui::Text("%s", TR("ui.audio_manager.eq_control").data());

            const char* presets[] = {
                TR("ui.audio_manager.eq_none").data(),
                TR("ui.audio_manager.eq_10_band").data(),
                TR("ui.audio_manager.eq_15_band").data()
            };
            int presetIdx = static_cast<int>(m_currentPreset);
            if ( ImGui::Combo("##EQPreset",
                              &presetIdx,
                              presets,
                              IM_ARRAYSIZE(presets)) ) {
                m_currentPreset = static_cast<Audio::EQPreset>(presetIdx);
                audio.createMainTrackEQ(m_currentPreset);
                changed = true;
            }

            if ( audio.isMainTrackEQEnabled() ) {
                size_t bandCount = audio.getMainTrackEQBandCount();

                // 绘制增益预览图
                if ( ImPlot::BeginPlot(
                         "##EQCurve",
                         ImVec2(-1, 150),
                         ImPlotFlags_NoLegend | ImPlotFlags_NoMenus) ) {
                    ImPlot::SetupAxis(ImAxis_X1,
                                      TR("ui.audio_manager.freq_hz").data());
                    ImPlot::SetupAxis(ImAxis_Y1,
                                      TR("ui.audio_manager.gain_db").data());
                    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                    ImPlot::SetupAxisLimits(ImAxis_X1, 20, 20000);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, -24, 24);
                    ImPlot::SetupAxisFormat(ImAxis_Y1, "%.0f dB");

                    // 绘制平滑的真幅频响应曲线
                    const int                  sampleCount = 128;
                    static std::vector<double> curveX(sampleCount);
                    static std::vector<double> curveY(sampleCount);

                    for ( int j = 0; j < sampleCount; ++j ) {
                        // 对数空间采样 20Hz - 20kHz
                        double t = (double)j / (double)(sampleCount - 1);
                        double freq =
                            20.0 * std::pow(1000.0, t);  // 20 * 10^(3*t)
                        curveX[j] = freq;
                        curveY[j] =
                            (double)audio.getMainTrackEQResponse((float)freq);
                    }

                    ImPlot::PlotLine(TR("ui.audio_manager.response").data(),
                                     curveX.data(),
                                     curveY.data(),
                                     sampleCount,
                                     ImPlotSpec(ImPlotProp_LineColor,
                                                ImVec4(0.2f, 0.8f, 1.0f, 1.0f),
                                                ImPlotProp_LineWeight,
                                                2.0f));

                    // 绘制各频段控制点
                    std::vector<double> plotX(bandCount);
                    std::vector<double> plotY(bandCount);
                    for ( size_t i = 0; i < bandCount; ++i ) {
                        plotX[i] = audio.getMainTrackEQBandFrequency(i);
                        plotY[i] = audio.getMainTrackEQBandGain(i);
                    }

                    ImPlot::PlotScatter(
                        TR("ui.audio_manager.bands").data(),
                        plotX.data(),
                        plotY.data(),
                        (int)bandCount,
                        ImPlotSpec(ImPlotProp_Marker, ImPlotMarker_Circle));

                    ImPlot::EndPlot();
                }

                // 绘制滑块组
                ImGui::Separator();

                float sliderWidth = ImGui::GetFrameHeight();
                float eqSpacing   = 8.0f;  // 增加间距以提高可读性

                // 启用水平滚动条，高度增加以容纳滚动条
                ImGui::BeginChild("EQSliders",
                                  ImVec2(0, 220),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_HorizontalScrollbar);

                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(eqSpacing, 4));

                float startY = ImGui::GetCursorPosY();
                for ( size_t i = 0; i < bandCount; ++i ) {
                    ImGui::PushID((int)i);

                    ImGui::SetCursorPosY(i == 0 ? startY + 3 : startY);

                    ImGui::BeginGroup();

                    float gain = audio.getMainTrackEQBandGain(i);
                    float q    = audio.getMainTrackEQBandQ(i);
                    char  label[32];
                    float freq = audio.getMainTrackEQBandFrequency(i);
                    if ( freq >= 1000.0f )
                        snprintf(label, sizeof(label), "%.1fk", freq / 1000.0f);
                    else
                        snprintf(label, sizeof(label), "%.0f", freq);

                    // 1. 频率标签 (居中对齐)
                    float textWidth = ImGui::CalcTextSize(label).x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                         (sliderWidth - textWidth) * 0.5f);
                    ImGui::TextUnformatted(label);

                    // 2. 增益滑块 (固定宽度)
                    if ( ImGui::VSliderFloat("##Gain",
                                             ImVec2(sliderWidth, 110),
                                             &gain,
                                             -24.0f,
                                             24.0f,
                                             "") ) {
                        audio.setMainTrackEQBandGain(i, gain);
                        changed = true;
                    }
                    if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                        ImGui::SetTooltip(
                            TR("ui.audio_manager.eq_tooltip").data(),
                            label,
                            gain);
                    }

                    // 3. Q 值滑块 (改为竖直布局，交互更稳定)
                    if ( ImGui::VSliderFloat("##Q",
                                             ImVec2(sliderWidth, 72),
                                             &q,
                                             0.1f,
                                             10.0f,
                                             "") ) {
                        audio.setMainTrackEQBandQ(i, q);
                        changed = true;
                    }
                    if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                        ImGui::SetTooltip(
                            TR("ui.audio_manager.q_factor_tooltip").data(), q);
                    }

                    ImGui::EndGroup();
                    ImGui::PopID();

                    if ( i < bandCount - 1 ) {
                        ImGui::SameLine();
                    }
                }
                ImGui::PopStyleVar();
                ImGui::EndChild();


                if ( ImGui::Button(TR("ui.audio_manager.reset_eq").data()) ) {
                    for ( size_t i = 0; i < bandCount; ++i ) {
                        audio.setMainTrackEQBandGain(i, 0.0f);
                        audio.setMainTrackEQBandQ(i, 1.414f);
                    }
                    changed = true;
                }
            }
        }

        // --- 4. 特效音轨预览控件 ---
        if ( m_type == TrackType::Effect ) {
            ImGui::Separator();
            if ( ImGui::Button(TR("ui.audio_manager.play_preview").data()) ) {
                audio.playSoundEffect(m_trackId);
            }

            ImGui::SameLine();
            float duration     = (float)audio.getSFXDuration(m_trackId);
            float playbackTime = (float)audio.getSFXPlaybackTime(m_trackId);
            float progress =
                (duration > 0.0f) ? (playbackTime / duration) : 0.0f;
            std::string progressText =
                fmt::format("{:.2f}s / {:.2f}s", playbackTime, duration);

            ImGui::ProgressBar(progress, ImVec2(-1, 0), progressText.c_str());
        }

        // --- 5. 应用更改并持久化 ---
        if ( changed ) {
            if ( config ) {
                config->volume        = volume;
                config->muted         = muted;
                config->playbackSpeed = speed;
                config->playbackPitch = pitch;
                if ( m_type == TrackType::Main ) {
                    config->eqEnabled = audio.isMainTrackEQEnabled();
                    config->eqPreset =
                        static_cast<int>(audio.getMainTrackEQPreset());
                    config->eqBandGains.clear();
                    config->eqBandQs.clear();

                    const size_t bandCount = audio.getMainTrackEQBandCount();
                    config->eqBandGains.reserve(bandCount);
                    config->eqBandQs.reserve(bandCount);
                    for ( size_t i = 0; i < bandCount; ++i ) {
                        config->eqBandGains.push_back(
                            audio.getMainTrackEQBandGain(i));
                        config->eqBandQs.push_back(
                            audio.getMainTrackEQBandQ(i));
                    }
                }
            }

            if ( m_type == TrackType::Main ) {
                audio.setMainTrackVolume(volume);
                audio.setMainTrackMute(muted);
                audio.setPlaybackSpeed(speed);
                audio.setPlaybackPitch(pitch);
            } else {
                bool  isPermanent = true;
                auto& skinData    = Config::SkinManager::instance().getData();
                if ( skinData.audioPaths.count(m_trackId) == 0 ) {
                    isPermanent = false;
                }
                audio.setSFXPoolVolume(m_trackId, volume, isPermanent);
                audio.setSFXPoolMute(m_trackId, muted, isPermanent);
            }

            if ( project ) {
                engine.saveProject();
            }
        }
    }
    ImGui::End();
}

}  // namespace MMM::UI
