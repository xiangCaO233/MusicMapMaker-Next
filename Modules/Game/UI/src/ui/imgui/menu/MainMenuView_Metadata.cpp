#define IMGUI_DEFINE_MATH_OPERATORS
#include "canvas/TimeFormatUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Polyline.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/menu/MainMenuView.h"
#include <ImGuiFileDialog.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>

namespace MMM::UI
{

/// @brief 渲染谱面元数据编辑窗口。
void MainMenuView::renderMetadataEditorWindow()
{
    if ( !m_showMetadataEditorWindow ) return;

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    ImGui::SetNextWindowSize(ImVec2(800.0f * dpiScale, 550.0f * dpiScale),
                             ImGuiCond_FirstUseEver);

    auto&   skinMgr   = Config::SkinManager::instance();
    ImFont* titleFont = skinMgr.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont);

    bool opened = ImGui::Begin("谱面额外元数据编辑###MetadataEditorWindow",
                               &m_showMetadataEditorWindow,
                               ImGuiWindowFlags_None);

    if ( titleFont ) ImGui::PopFont();

    if ( opened ) {
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( !session ) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "当前无活动的编辑器会话。");
        } else {
            auto beatmap = session->getContext().currentBeatmap;
            if ( !beatmap ) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "当前未加载任何谱面，请先打开或新建谱面。");
            } else {
                ImGui::Text("正在编辑谱面: %s (%s)",
                            beatmap->m_baseMapMetadata.name.c_str(),
                            beatmap->m_baseMapMetadata.version.c_str());
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                   "提示：在此处修改的额外字段会在保存谱面时一"
                                   "同导出。键入即可开始编辑。");
                ImGui::Separator();
                ImGui::Spacing();

                if ( ImGui::BeginTabBar("MetadataTypeTabBar") ) {
                    // ==================== OSU TAB ====================
                    if ( ImGui::BeginTabItem("osu! (OSU) 格式元数据") ) {
                        // Predefined OSU fields
                        static const std::vector<
                            std::pair<std::string, std::string>>
                            OSU_FIELDS = {
                                { "General::AudioFilename",
                                  "音频文件名 - 谱面所使用的音频文件路径" },
                                { "General::AudioLeadIn",
                                  "音频前导时间 (ms) - "
                                  "谱面开始播放前的缓冲时间" },
                                { "General::AudioHash",
                                  "音频哈希值 - 音频文件的 MD5" },
                                { "General::PreviewTime",
                                  "预览开始时间 (ms) - "
                                  "选歌界面预览音频的起点" },
                                { "General::Countdown",
                                  "倒计时样式 - 0=无, 1=普通, 2=快速, 3=极速" },
                                { "General::SampleSet",
                                  "默认音效样本组 - Normal, Soft, Drum" },
                                { "General::StackLeniency",
                                  "堆叠容差 - 影响连打/重叠物件的错开位移" },
                                { "General::Mode",
                                  "游戏模式 - 0=osu!, 1=Taiko, 2=Catch, "
                                  "3=Mania" },
                                { "General::LetterboxInBreaks",
                                  "休息段显示黑边 - 0/1" },
                                { "General::StoryFireInFront",
                                  "故事板火花在前 - 0/1" },
                                { "General::UseSkinSprites",
                                  "使用皮肤精灵 - 0/1" },
                                { "General::AlwaysShowPlayfield",
                                  "总是显示活动区域 - 0/1" },
                                { "General::OverlayPosition",
                                  "界面覆盖层位置 - NoChange, Below, Above" },
                                { "General::SkinPreference", "推荐皮肤名称" },
                                { "General::EpilepsyWarning",
                                  "癫痫警告 - 0/1" },
                                { "General::CountdownOffset",
                                  "倒计时偏移时间 (ms)" },
                                { "General::SpecialStyle",
                                  "特殊样式 - 0/1, mania 中用于 N+1 键位布局" },
                                { "General::WidescreenStoryboard",
                                  "宽屏故事板 - 0/1" },
                                { "General::SamplesMatchPlaybackRate",
                                  "音效速率跟随播放速度 - 0/1" },

                                { "Editor::Bookmarks",
                                  "书签 - 逗号分隔的毫秒整型数组" },
                                { "Editor::DistanceSpacing", "距离间距系数" },
                                { "Editor::BeatDivisor",
                                  "节拍细分数 - 例如 4, 8, 12, 16" },
                                { "Editor::GridSize", "网格大小" },
                                { "Editor::TimelineZoom", "时间轴缩放倍率" },

                                { "Metadata::Title",
                                  "歌曲标题 (对应 base 标题)" },
                                { "Metadata::TitleUnicode",
                                  "歌曲标题 (原语/Unicode)" },
                                { "Metadata::Artist",
                                  "艺术家 (对应 base 艺术家)" },
                                { "Metadata::ArtistUnicode",
                                  "艺术家 (原语/Unicode)" },
                                { "Metadata::Creator", "谱面创作者" },
                                { "Metadata::Version", "难度版本名" },
                                { "Metadata::Source",
                                  "歌曲来源 - 如动漫/游戏名" },
                                { "Metadata::Tags", "检索标签 - 空格分隔" },
                                { "Metadata::BeatmapID",
                                  "谱面唯一 ID (官网分配)" },
                                { "Metadata::BeatmapSetID",
                                  "谱面集唯一 ID (官网分配)" },

                                { "Difficulty::HPDrainRate",
                                  "HP 减少速率 (0-10)" },
                                { "Difficulty::CircleSize", "键数 / 轨道数" },
                                { "Difficulty::OverallDifficulty",
                                  "综合难度 / 判定严准度 (0-10)" },
                                { "Difficulty::ApproachRate",
                                  "缩圈速度 / 下落速度 (0-10)" },
                                { "Difficulty::SliderMultiplier",
                                  "滑条速度倍率" },
                                { "Difficulty::SliderTickRate",
                                  "滑条 Tick 生成率" },

                                { "Events::background",
                                  "背景图片设置串 - 格式: 0,0,\"文件名\",x,y" },
                                { "Events::breaks", "休息时间段定义串" }
                            };

                        auto& props = beatmap->m_metadata
                                          .map_properties[MapMetadataType::OSU];

                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;

                        float footerHeight = 45.0f * dpiScale;
                        if ( ImGui::BeginTable("OSUMetadataTable",
                                               4,
                                               tableFlags,
                                               ImVec2(0.0f, -footerHeight)) ) {
                            ImGui::TableSetupColumn(
                                "键名 (Key)",
                                ImGuiTableColumnFlags_WidthFixed,
                                220.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "描述 (Description)",
                                ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(
                                "数值 (Value)",
                                ImGuiTableColumnFlags_WidthFixed,
                                250.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "操作 (Action)",
                                ImGuiTableColumnFlags_WidthFixed,
                                60.0f * dpiScale);
                            ImGui::TableHeadersRow();

                            // 1. Render predefined fields
                            for ( const auto& [key, desc] : OSU_FIELDS ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();

                                bool hasKey = props.contains(key);
                                if ( !hasKey ) {
                                    ImGui::PushStyleColor(
                                        ImGuiCol_Text,
                                        ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                }
                                ImGui::TextUnformatted(key.c_str());
                                if ( !hasKey ) {
                                    ImGui::PopStyleColor();
                                }
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip(
                                        "双击可以复制该内置键名到剪贴板。");
                                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                                        ImGui::SetClipboardText(key.c_str());
                                    }
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::PushStyleColor(
                                    ImGuiCol_Text,
                                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                ImGui::TextWrapped("%s", desc.c_str());
                                ImGui::PopStyleColor();

                                ImGui::TableNextColumn();
                                char valBuf[1024] = { 0 };
                                if ( hasKey ) {
                                    std::string currentVal = props.at(key);
                                    size_t      copyLen    = std::min(
                                        currentVal.size(), sizeof(valBuf) - 1);
                                    std::copy(currentVal.begin(),
                                              currentVal.begin() + copyLen,
                                              valBuf);
                                }

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_osu_") + key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key] = valBuf;
                                }

                                ImGui::TableNextColumn();
                                if ( hasKey ) {
                                    if ( ImGui::Button(
                                             (std::string("清除##clear_osu_") +
                                              key)
                                                 .c_str()) ) {
                                        props.erase(key);
                                    }
                                } else {
                                    ImGui::TextDisabled("-");
                                }
                            }

                            // 2. Render other custom fields
                            std::vector<std::string> customKeys;
                            for ( const auto& [k, v] : props ) {
                                bool isPredefined = false;
                                for ( const auto& [pk, pd] : OSU_FIELDS ) {
                                    if ( pk == k ) {
                                        isPredefined = true;
                                        break;
                                    }
                                }
                                if ( !isPredefined ) {
                                    customKeys.push_back(k);
                                }
                            }

                            for ( const auto& key : customKeys ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(key.c_str());
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip("这是一个自定义键。");
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextDisabled("自定义键值");

                                ImGui::TableNextColumn();
                                char        valBuf[1024] = { 0 };
                                std::string currentVal   = props.at(key);
                                size_t copyLen = std::min(currentVal.size(),
                                                          sizeof(valBuf) - 1);
                                std::copy(currentVal.begin(),
                                          currentVal.begin() + copyLen,
                                          valBuf);

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_osu_custom_") +
                                          key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key] = valBuf;
                                }

                                ImGui::TableNextColumn();
                                if ( ImGui::Button(
                                         (std::string("删除##del_osu_") + key)
                                             .c_str()) ) {
                                    props.erase(key);
                                }
                            }

                            ImGui::EndTable();
                        }

                        // Add new field form
                        ImGui::Separator();
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("新增自定义键名:");
                        ImGui::SameLine();
                        static char newOsuKey[128] = "";
                        ImGui::SetNextItemWidth(200.0f * dpiScale);
                        ImGui::InputText(
                            "##new_osu_key", newOsuKey, sizeof(newOsuKey));

                        ImGui::SameLine();
                        ImGui::Text("数值:");
                        ImGui::SameLine();
                        static char newOsuVal[256] = "";
                        ImGui::SetNextItemWidth(250.0f * dpiScale);
                        ImGui::InputText(
                            "##new_osu_val", newOsuVal, sizeof(newOsuVal));

                        ImGui::SameLine();
                        if ( ImGui::Button("添加##add_osu_field") ) {
                            std::string nk = newOsuKey;
                            if ( !nk.empty() ) {
                                props[nk]    = newOsuVal;
                                newOsuKey[0] = '\0';
                                newOsuVal[0] = '\0';
                            }
                        }

                        ImGui::EndTabItem();
                    }

                    // ==================== MALODY TAB ====================
                    if ( ImGui::BeginTabItem("Malody (MALODY) 格式元数据") ) {
                        static const std::vector<
                            std::pair<std::string, std::string>>
                            MALODY_FIELDS = {
                                { "id", "谱面 ID" },
                                { "preview",
                                  "音频预览时间戳 (ms) - 选歌界面试听起点" },
                                { "mode",
                                  "游戏模式 - 0=Key, 1=Catch, 2=Pad, 3=Taiko, "
                                  "4=Ring, 5=Slide, 6=Live, 7=Cube" },
                                { "$ver", "文件格式版本" },
                                { "aimode", "AI 辅助模式配置" },
                                { "mode_ext", "模式额外扩展配置 (JSON 串)" },
                                { "extra", "额外顶层扩展配置 (JSON 串)" },
                                { "initialDelay",
                                  "初始节拍延迟 / 时间戳首点 (ms)" },
                                { "audioOffset", "音频时间偏移 (ms)" }
                            };

                        auto& props =
                            beatmap->m_metadata
                                .map_properties[MapMetadataType::MALODY];

                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;

                        float footerHeight = 45.0f * dpiScale;
                        if ( ImGui::BeginTable("MalodyMetadataTable",
                                               4,
                                               tableFlags,
                                               ImVec2(0.0f, -footerHeight)) ) {
                            ImGui::TableSetupColumn(
                                "键名 (Key)",
                                ImGuiTableColumnFlags_WidthFixed,
                                220.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "描述 (Description)",
                                ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(
                                "数值 (Value)",
                                ImGuiTableColumnFlags_WidthFixed,
                                250.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "操作 (Action)",
                                ImGuiTableColumnFlags_WidthFixed,
                                60.0f * dpiScale);
                            ImGui::TableHeadersRow();

                            // 1. Render predefined fields
                            for ( const auto& [key, desc] : MALODY_FIELDS ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();

                                bool hasKey = props.contains(key);
                                if ( !hasKey ) {
                                    ImGui::PushStyleColor(
                                        ImGuiCol_Text,
                                        ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                }
                                ImGui::TextUnformatted(key.c_str());
                                if ( !hasKey ) {
                                    ImGui::PopStyleColor();
                                }
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip(
                                        "双击可以复制该内置键名到剪贴板。");
                                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                                        ImGui::SetClipboardText(key.c_str());
                                    }
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::PushStyleColor(
                                    ImGuiCol_Text,
                                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                ImGui::TextWrapped("%s", desc.c_str());
                                ImGui::PopStyleColor();

                                ImGui::TableNextColumn();
                                char valBuf[1024] = { 0 };
                                if ( hasKey ) {
                                    std::string currentVal = props.at(key);
                                    size_t      copyLen    = std::min(
                                        currentVal.size(), sizeof(valBuf) - 1);
                                    std::copy(currentVal.begin(),
                                              currentVal.begin() + copyLen,
                                              valBuf);
                                }

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_mld_") + key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key] = valBuf;
                                }

                                ImGui::TableNextColumn();
                                if ( hasKey ) {
                                    if ( ImGui::Button(
                                             (std::string("清除##clear_mld_") +
                                              key)
                                                 .c_str()) ) {
                                        props.erase(key);
                                    }
                                } else {
                                    ImGui::TextDisabled("-");
                                }
                            }

                            // 2. Render other custom fields
                            std::vector<std::string> customKeys;
                            for ( const auto& [k, v] : props ) {
                                bool isPredefined = false;
                                for ( const auto& [pk, pd] : MALODY_FIELDS ) {
                                    if ( pk == k ) {
                                        isPredefined = true;
                                        break;
                                    }
                                }
                                if ( !isPredefined ) {
                                    customKeys.push_back(k);
                                }
                            }

                            for ( const auto& key : customKeys ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(key.c_str());
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip("这是一个自定义键。");
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextDisabled("自定义键值");

                                ImGui::TableNextColumn();
                                char        valBuf[1024] = { 0 };
                                std::string currentVal   = props.at(key);
                                size_t copyLen = std::min(currentVal.size(),
                                                          sizeof(valBuf) - 1);
                                std::copy(currentVal.begin(),
                                          currentVal.begin() + copyLen,
                                          valBuf);

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_mld_custom_") +
                                          key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key] = valBuf;
                                }

                                ImGui::TableNextColumn();
                                if ( ImGui::Button(
                                         (std::string("删除##del_mld_") + key)
                                             .c_str()) ) {
                                    props.erase(key);
                                }
                            }

                            ImGui::EndTable();
                        }

                        // Add new field form
                        ImGui::Separator();
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("新增自定义键名:");
                        ImGui::SameLine();
                        static char newMldKey[128] = "";
                        ImGui::SetNextItemWidth(200.0f * dpiScale);
                        ImGui::InputText(
                            "##new_mld_key", newMldKey, sizeof(newMldKey));

                        ImGui::SameLine();
                        ImGui::Text("数值:");
                        ImGui::SameLine();
                        static char newMldVal[256] = "";
                        ImGui::SetNextItemWidth(250.0f * dpiScale);
                        ImGui::InputText(
                            "##new_mld_val", newMldVal, sizeof(newMldVal));

                        ImGui::SameLine();
                        if ( ImGui::Button("添加##add_mld_field") ) {
                            std::string nk = newMldKey;
                            if ( !nk.empty() ) {
                                props[nk]    = newMldVal;
                                newMldKey[0] = '\0';
                                newMldVal[0] = '\0';
                            }
                        }

                        ImGui::EndTabItem();
                    }

                    // ==================== RM TAB ====================
                    if ( ImGui::BeginTabItem("RM (RM) 格式元数据") ) {
                        static const std::vector<
                            std::pair<std::string, std::string>>
                            RM_FIELDS = { { "Parameter", "全局额外参考参数" } };

                        auto& props = beatmap->m_metadata
                                          .map_properties[MapMetadataType::RM];

                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;

                        float footerHeight = 45.0f * dpiScale;
                        if ( ImGui::BeginTable("RMMetadataTable",
                                               4,
                                               tableFlags,
                                               ImVec2(0.0f, -footerHeight)) ) {
                            ImGui::TableSetupColumn(
                                "键名 (Key)",
                                ImGuiTableColumnFlags_WidthFixed,
                                220.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "描述 (Description)",
                                ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(
                                "数值 (Value)",
                                ImGuiTableColumnFlags_WidthFixed,
                                250.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                "操作 (Action)",
                                ImGuiTableColumnFlags_WidthFixed,
                                60.0f * dpiScale);
                            ImGui::TableHeadersRow();

                            // 1. Render predefined fields
                            for ( const auto& [key, desc] : RM_FIELDS ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();

                                bool hasKey = props.contains(key);
                                if ( !hasKey ) {
                                    ImGui::PushStyleColor(
                                        ImGuiCol_Text,
                                        ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                }
                                ImGui::TextUnformatted(key.c_str());
                                if ( !hasKey ) {
                                    ImGui::PopStyleColor();
                                }
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip(
                                        "双击可以复制该内置键名到剪贴板。");
                                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                                        ImGui::SetClipboardText(key.c_str());
                                    }
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::PushStyleColor(
                                    ImGuiCol_Text,
                                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                                ImGui::TextWrapped("%s", desc.c_str());
                                ImGui::PopStyleColor();

                                ImGui::TableNextColumn();
                                char valBuf[1024] = { 0 };
                                if ( hasKey ) {
                                    std::string currentVal = props.at(key);
                                    size_t      copyLen    = std::min(
                                        currentVal.size(), sizeof(valBuf) - 1);
                                    std::copy(currentVal.begin(),
                                              currentVal.begin() + copyLen,
                                              valBuf);
                                }

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_rm_") + key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key] = valBuf;
                                }

                                ImGui::TableNextColumn();
                                if ( hasKey ) {
                                    if ( ImGui::Button(
                                             (std::string("清除##clear_rm_") +
                                              key)
                                                 .c_str()) ) {
                                        props.erase(key);
                                    }
                                } else {
                                    ImGui::TextDisabled("-");
                                }
                            }

                            // 2. Render other custom fields
                            std::vector<std::string> customKeys;
                            for ( const auto& [k, v] : props ) {
                                bool isPredefined = false;
                                for ( const auto& [pk, pd] : RM_FIELDS ) {
                                    if ( pk == k ) {
                                        isPredefined = true;
                                        break;
                                    }
                                }
                                if ( !isPredefined ) {
                                    customKeys.push_back(k);
                                }
                            }

                            for ( const auto& key : customKeys ) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(key.c_str());
                                if ( ImGui::IsItemHovered() ) {
                                    ImGui::SetTooltip("这是一个自定义键。");
                                }

                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextDisabled("自定义键值");

                                ImGui::TableNextColumn();
                                char        valBuf[1024] = { 0 };
                                std::string currentVal   = props.at(key);
                                size_t copyLen = std::min(currentVal.size(),
                                                          sizeof(valBuf) - 1);
                                std::copy(currentVal.begin(),
                                          currentVal.begin() + copyLen,
                                          valBuf);

                                ImGui::SetNextItemWidth(-1.0f);
                                if ( ImGui::InputText(
                                         (std::string("##val_rm_custom_") + key)
                                             .c_str(),
                                         valBuf,
                                         sizeof(valBuf)) ) {
                                    props[key] = valBuf;
                                }

                                ImGui::TableNextColumn();
                                if ( ImGui::Button(
                                         (std::string("删除##del_rm_") + key)
                                             .c_str()) ) {
                                    props.erase(key);
                                }
                            }

                            ImGui::EndTable();
                        }

                        // Add new field form
                        ImGui::Separator();
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("新增自定义键名:");
                        ImGui::SameLine();
                        static char newRmKey[128] = "";
                        ImGui::SetNextItemWidth(200.0f * dpiScale);
                        ImGui::InputText(
                            "##new_rm_key", newRmKey, sizeof(newRmKey));

                        ImGui::SameLine();
                        ImGui::Text("数值:");
                        ImGui::SameLine();
                        static char newRmVal[256] = "";
                        ImGui::SetNextItemWidth(250.0f * dpiScale);
                        ImGui::InputText(
                            "##new_rm_val", newRmVal, sizeof(newRmVal));

                        ImGui::SameLine();
                        if ( ImGui::Button("添加##add_rm_field") ) {
                            std::string nk = newRmKey;
                            if ( !nk.empty() ) {
                                props[nk]   = newRmVal;
                                newRmKey[0] = '\0';
                                newRmVal[0] = '\0';
                            }
                        }

                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(6);
}

/// @brief 渲染选中音符元数据编辑窗口。
void MainMenuView::renderNoteMetadataEditorWindow()
{
    if ( !m_showNoteMetadataEditorWindow ) return;

    struct InputBuffer {
        char key[128] = "";
        char val[256] = "";
    };
    static std::unordered_map<std::string, InputBuffer> inputBuffers;
    static bool                                         lastShowState = false;
    if ( m_showNoteMetadataEditorWindow && !lastShowState ) {
        inputBuffers.clear();
    }
    lastShowState = m_showNoteMetadataEditorWindow;

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    ImGui::SetNextWindowSize(ImVec2(750.0f * dpiScale, 500.0f * dpiScale),
                             ImGuiCond_FirstUseEver);

    auto&   skinMgr   = Config::SkinManager::instance();
    ImFont* titleFont = skinMgr.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont);

    bool opened = ImGui::Begin(TR("ui.edit.note_metadata.title").data(),
                               &m_showNoteMetadataEditorWindow,
                               ImGuiWindowFlags_None);

    if ( titleFont ) ImGui::PopFont();

    if ( opened ) {
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( !session ) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "%s",
                               TR("ui.tools.no_active_session").data());
        } else {
            // --- 收集选中物件 ---
            struct SelectedNote {
                entt::entity entity;
                double       timestamp;
            };
            std::vector<SelectedNote> selectedNotes;
            auto& registry = session->getContextMutable().noteRegistry;

            auto view = registry.view<const Logic::NoteComponent,
                                      const Logic::InteractionComponent>();
            for ( auto e : view ) {
                const auto& interaction =
                    view.get<const Logic::InteractionComponent>(e);
                if ( interaction.isSelected ) {
                    const auto& nc = view.get<const Logic::NoteComponent>(e);
                    selectedNotes.push_back({ e, nc.m_timestamp });
                }
            }

            if ( selectedNotes.empty() ) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                    "%s",
                    TR("ui.edit.note_metadata.no_selection").data());
            } else {
                // --- 按元数据指纹分组 ---
                // 将 note_properties 序列化为字符串作为分组键
                auto serializeMetadata =
                    [](const ::MMM::NoteMetadata& meta) -> std::string {
                    std::string result;
                    // 按 NoteMetadataType 排序遍历
                    for ( int typeIdx = 0; typeIdx < 3; ++typeIdx ) {
                        auto metaType =
                            static_cast<::MMM::NoteMetadataType>(typeIdx);
                        auto it = meta.note_properties.find(metaType);
                        if ( it == meta.note_properties.end() ) continue;
                        if ( it->second.empty() ) continue;
                        result += std::to_string(typeIdx) + "{";
                        // 收集并排序 key
                        std::vector<std::string> keys;
                        for ( const auto& [k, v] : it->second ) {
                            keys.push_back(k);
                        }
                        std::sort(keys.begin(), keys.end());
                        for ( const auto& k : keys ) {
                            result += k + "=" + it->second.at(k) + ";";
                        }
                        result += "}";
                    }
                    return result;
                };

                struct MetadataGroup {
                    std::vector<entt::entity> entities;
                    double                    minTime;
                    double                    maxTime;
                };
                std::map<std::string, MetadataGroup> groups;

                for ( const auto& sn : selectedNotes ) {
                    auto& nc = registry.get<Logic::NoteComponent>(sn.entity);
                    std::string fingerprint = serializeMetadata(nc.m_metadata);
                    auto&       group       = groups[fingerprint];
                    group.entities.push_back(sn.entity);
                    if ( group.entities.size() == 1 ) {
                        group.minTime = sn.timestamp;
                        group.maxTime = sn.timestamp;
                    } else {
                        group.minTime = std::min(group.minTime, sn.timestamp);
                        group.maxTime = std::max(group.maxTime, sn.timestamp);
                    }
                }

                // --- 摘要信息 ---
                std::string summaryStr = TR_FMT("ui.edit.note_metadata.summary",
                                                selectedNotes.size(),
                                                groups.size());
                ImGui::TextUnformatted(summaryStr.c_str());
                ImGui::Separator();
                ImGui::Spacing();

                // --- 各组的编辑区域 ---
                ImGui::BeginChild("NoteMetaGroups",
                                  ImVec2(0, 0),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_None);

                int groupIdx = 0;
                for ( auto& [fingerprint, group] : groups ) {
                    ++groupIdx;
                    const auto minTimeText =
                        Canvas::formatCanvasTime(group.minTime);
                    const auto maxTimeText =
                        Canvas::formatCanvasTime(group.maxTime);
                    std::string headerStr =
                        TR_FMT("ui.edit.note_metadata.group_header",
                               groupIdx,
                               group.entities.size(),
                               minTimeText,
                               maxTimeText);

                    ImGui::PushID(groupIdx);

                    bool headerOpen = ImGui::CollapsingHeader(
                        headerStr.c_str(),
                        groups.size() == 1 ? ImGuiTreeNodeFlags_DefaultOpen
                                           : ImGuiTreeNodeFlags_None);

                    if ( headerOpen ) {
                        // 取第一个实体作为代表
                        auto  firstEntity = group.entities.front();
                        auto& firstNc =
                            registry.get<Logic::NoteComponent>(firstEntity);

                        if ( ImGui::BeginTabBar(
                                 fmt::format("MetaTabBar_{}", groupIdx)
                                     .c_str()) ) {
                            // --- OSU 标签页 ---
                            if ( ImGui::BeginTabItem("OSU") ) {
                                auto metaType = ::MMM::NoteMetadataType::OSU;
                                auto it =
                                    firstNc.m_metadata.note_properties.find(
                                        metaType);
                                static const decltype(firstNc.m_metadata
                                                          .note_properties)::
                                    mapped_type emptyMap;
                                const auto&     refProps =
                                    (it !=
                                     firstNc.m_metadata.note_properties.end())
                                        ? it->second
                                        : emptyMap;

                                ImGuiTableFlags tableFlags =
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_BordersOuter |
                                    ImGuiTableFlags_Resizable;

                                if ( ImGui::BeginTable(
                                         fmt::format("NoteMetaTable_OSU_{}",
                                                     groupIdx)
                                             .c_str(),
                                         3,
                                         tableFlags,
                                         ImVec2(0.0f, 0.0f)) ) {
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.key_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        220.0f * dpiScale);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.value_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.action_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        60.0f * dpiScale);
                                    ImGui::TableHeadersRow();

                                    std::vector<std::string> keys;
                                    for ( const auto& [k, v] : refProps ) {
                                        keys.push_back(k);
                                    }
                                    std::sort(keys.begin(), keys.end());

                                    for ( const auto& key : keys ) {
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();
                                        ImGui::AlignTextToFramePadding();
                                        ImGui::TextUnformatted(key.c_str());

                                        ImGui::TableNextColumn();
                                        char        valBuf[1024] = { 0 };
                                        std::string currentVal =
                                            refProps.at(key);
                                        size_t copyLen =
                                            std::min(currentVal.size(),
                                                     sizeof(valBuf) - 1);
                                        std::copy(currentVal.begin(),
                                                  currentVal.begin() + copyLen,
                                                  valBuf);

                                        ImGui::SetNextItemWidth(-1.0f);
                                        if ( ImGui::InputText(
                                                 fmt::format("##nm_osu_{}_{}",
                                                             groupIdx,
                                                             key)
                                                     .c_str(),
                                                 valBuf,
                                                 sizeof(valBuf)) ) {
                                            // 写回所有同组实体
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                nc.m_metadata
                                                    .note_properties[metaType]
                                                                    [key] =
                                                    valBuf;
                                            }
                                        }

                                        ImGui::TableNextColumn();
                                        if ( ImGui::Button(
                                                 fmt::format(
                                                     "{}##clr_osu_{}_{}",
                                                     TR("ui.edit.note_"
                                                        "metadata.clear_"
                                                        "btn")
                                                         .data(),
                                                     groupIdx,
                                                     key)
                                                     .c_str()) ) {
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                auto& m = nc.m_metadata
                                                              .note_properties
                                                                  [metaType];
                                                m.erase(key);
                                                if ( m.empty() ) {
                                                    nc.m_metadata
                                                        .note_properties.erase(
                                                            metaType);
                                                }
                                            }
                                        }
                                    }
                                    ImGui::EndTable();
                                }

                                // 新增字段
                                ImGui::Separator();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_key").data());
                                ImGui::SameLine();
                                std::string bufKey =
                                    fmt::format("{}_{}",
                                                groupIdx,
                                                static_cast<int>(metaType));
                                auto& buf = inputBuffers[bufKey];
                                ImGui::SetNextItemWidth(150.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmk_osu_{}", groupIdx)
                                        .c_str(),
                                    buf.key,
                                    sizeof(buf.key));
                                ImGui::SameLine();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_value")
                                        .data());
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(200.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmv_osu_{}", groupIdx)
                                        .c_str(),
                                    buf.val,
                                    sizeof(buf.val));
                                ImGui::SameLine();
                                if ( ImGui::Button(
                                         fmt::format("{}##nma_osu_{}",
                                                     TR("ui.edit.note_metadata."
                                                        "add_btn")
                                                         .data(),
                                                     groupIdx)
                                             .c_str()) ) {
                                    std::string nk = buf.key;
                                    if ( !nk.empty() ) {
                                        for ( auto e : group.entities ) {
                                            auto& nc =
                                                registry
                                                    .get<Logic::NoteComponent>(
                                                        e);
                                            nc.m_metadata
                                                .note_properties[metaType][nk] =
                                                buf.val;
                                        }
                                        buf.key[0] = '\0';
                                        buf.val[0] = '\0';
                                    }
                                }

                                ImGui::EndTabItem();
                            }

                            // --- MALODY 标签页 ---
                            if ( ImGui::BeginTabItem("MALODY") ) {
                                auto metaType = ::MMM::NoteMetadataType::MALODY;
                                auto it =
                                    firstNc.m_metadata.note_properties.find(
                                        metaType);
                                static const decltype(firstNc.m_metadata
                                                          .note_properties)::
                                    mapped_type emptyMap;
                                const auto&     refProps =
                                    (it !=
                                     firstNc.m_metadata.note_properties.end())
                                        ? it->second
                                        : emptyMap;

                                ImGuiTableFlags tableFlags =
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_BordersOuter |
                                    ImGuiTableFlags_Resizable;

                                if ( ImGui::BeginTable(
                                         fmt::format("NoteMetaTable_MLD_{}",
                                                     groupIdx)
                                             .c_str(),
                                         3,
                                         tableFlags,
                                         ImVec2(0.0f, 0.0f)) ) {
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.key_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        220.0f * dpiScale);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.value_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.action_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        60.0f * dpiScale);
                                    ImGui::TableHeadersRow();

                                    std::vector<std::string> keys;
                                    for ( const auto& [k, v] : refProps ) {
                                        keys.push_back(k);
                                    }
                                    std::sort(keys.begin(), keys.end());

                                    for ( const auto& key : keys ) {
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();
                                        ImGui::AlignTextToFramePadding();
                                        ImGui::TextUnformatted(key.c_str());

                                        ImGui::TableNextColumn();
                                        char        valBuf[1024] = { 0 };
                                        std::string currentVal =
                                            refProps.at(key);
                                        size_t copyLen =
                                            std::min(currentVal.size(),
                                                     sizeof(valBuf) - 1);
                                        std::copy(currentVal.begin(),
                                                  currentVal.begin() + copyLen,
                                                  valBuf);

                                        ImGui::SetNextItemWidth(-1.0f);
                                        if ( ImGui::InputText(
                                                 fmt::format("##nm_mld_{}_{}",
                                                             groupIdx,
                                                             key)
                                                     .c_str(),
                                                 valBuf,
                                                 sizeof(valBuf)) ) {
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                nc.m_metadata
                                                    .note_properties[metaType]
                                                                    [key] =
                                                    valBuf;
                                            }
                                        }

                                        ImGui::TableNextColumn();
                                        if ( ImGui::Button(
                                                 fmt::format(
                                                     "{}##clr_mld_{}_{}",
                                                     TR("ui.edit.note_"
                                                        "metadata.clear_"
                                                        "btn")
                                                         .data(),
                                                     groupIdx,
                                                     key)
                                                     .c_str()) ) {
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                auto& m = nc.m_metadata
                                                              .note_properties
                                                                  [metaType];
                                                m.erase(key);
                                                if ( m.empty() ) {
                                                    nc.m_metadata
                                                        .note_properties.erase(
                                                            metaType);
                                                }
                                            }
                                        }
                                    }
                                    ImGui::EndTable();
                                }

                                // 新增字段
                                ImGui::Separator();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_key").data());
                                ImGui::SameLine();
                                std::string bufKey =
                                    fmt::format("{}_{}",
                                                groupIdx,
                                                static_cast<int>(metaType));
                                auto& buf = inputBuffers[bufKey];
                                ImGui::SetNextItemWidth(150.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmk_mld_{}", groupIdx)
                                        .c_str(),
                                    buf.key,
                                    sizeof(buf.key));
                                ImGui::SameLine();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_value")
                                        .data());
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(200.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmv_mld_{}", groupIdx)
                                        .c_str(),
                                    buf.val,
                                    sizeof(buf.val));
                                ImGui::SameLine();
                                if ( ImGui::Button(
                                         fmt::format("{}##nma_mld_{}",
                                                     TR("ui.edit.note_metadata."
                                                        "add_btn")
                                                         .data(),
                                                     groupIdx)
                                             .c_str()) ) {
                                    std::string nk = buf.key;
                                    if ( !nk.empty() ) {
                                        for ( auto e : group.entities ) {
                                            auto& nc =
                                                registry
                                                    .get<Logic::NoteComponent>(
                                                        e);
                                            nc.m_metadata
                                                .note_properties[metaType][nk] =
                                                buf.val;
                                        }
                                        buf.key[0] = '\0';
                                        buf.val[0] = '\0';
                                    }
                                }

                                ImGui::EndTabItem();
                            }

                            // --- RM 标签页 ---
                            if ( ImGui::BeginTabItem("RM") ) {
                                auto metaType = ::MMM::NoteMetadataType::RM;
                                auto it =
                                    firstNc.m_metadata.note_properties.find(
                                        metaType);
                                static const decltype(firstNc.m_metadata
                                                          .note_properties)::
                                    mapped_type emptyMap;
                                const auto&     refProps =
                                    (it !=
                                     firstNc.m_metadata.note_properties.end())
                                        ? it->second
                                        : emptyMap;

                                ImGuiTableFlags tableFlags =
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_BordersOuter |
                                    ImGuiTableFlags_Resizable;

                                if ( ImGui::BeginTable(
                                         fmt::format("NoteMetaTable_RM_{}",
                                                     groupIdx)
                                             .c_str(),
                                         3,
                                         tableFlags,
                                         ImVec2(0.0f, 0.0f)) ) {
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.key_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        220.0f * dpiScale);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.value_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn(
                                        TR("ui.edit.note_metadata.action_col")
                                            .data(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        60.0f * dpiScale);
                                    ImGui::TableHeadersRow();

                                    std::vector<std::string> keys;
                                    for ( const auto& [k, v] : refProps ) {
                                        keys.push_back(k);
                                    }
                                    std::sort(keys.begin(), keys.end());

                                    for ( const auto& key : keys ) {
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();
                                        ImGui::AlignTextToFramePadding();
                                        ImGui::TextUnformatted(key.c_str());

                                        ImGui::TableNextColumn();
                                        char        valBuf[1024] = { 0 };
                                        std::string currentVal =
                                            refProps.at(key);
                                        size_t copyLen =
                                            std::min(currentVal.size(),
                                                     sizeof(valBuf) - 1);
                                        std::copy(currentVal.begin(),
                                                  currentVal.begin() + copyLen,
                                                  valBuf);

                                        ImGui::SetNextItemWidth(-1.0f);
                                        if ( ImGui::InputText(
                                                 fmt::format("##nm_rm_{}_{}",
                                                             groupIdx,
                                                             key)
                                                     .c_str(),
                                                 valBuf,
                                                 sizeof(valBuf)) ) {
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                nc.m_metadata
                                                    .note_properties[metaType]
                                                                    [key] =
                                                    valBuf;
                                            }
                                        }

                                        ImGui::TableNextColumn();
                                        if ( ImGui::Button(
                                                 fmt::format(
                                                     "{}##clr_rm_{}_{}",
                                                     TR("ui.edit.note_"
                                                        "metadata.clear_"
                                                        "btn")
                                                         .data(),
                                                     groupIdx,
                                                     key)
                                                     .c_str()) ) {
                                            for ( auto e : group.entities ) {
                                                auto& nc = registry.get<
                                                    Logic::NoteComponent>(e);
                                                auto& m = nc.m_metadata
                                                              .note_properties
                                                                  [metaType];
                                                m.erase(key);
                                                if ( m.empty() ) {
                                                    nc.m_metadata
                                                        .note_properties.erase(
                                                            metaType);
                                                }
                                            }
                                        }
                                    }
                                    ImGui::EndTable();
                                }

                                // 新增字段
                                ImGui::Separator();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_key").data());
                                ImGui::SameLine();
                                std::string bufKey =
                                    fmt::format("{}_{}",
                                                groupIdx,
                                                static_cast<int>(metaType));
                                auto& buf = inputBuffers[bufKey];
                                ImGui::SetNextItemWidth(150.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmk_rm_{}", groupIdx)
                                        .c_str(),
                                    buf.key,
                                    sizeof(buf.key));
                                ImGui::SameLine();
                                ImGui::TextUnformatted(
                                    TR("ui.edit.note_metadata.add_value")
                                        .data());
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(200.0f * dpiScale);
                                ImGui::InputText(
                                    fmt::format("##nmv_rm_{}", groupIdx)
                                        .c_str(),
                                    buf.val,
                                    sizeof(buf.val));
                                ImGui::SameLine();
                                if ( ImGui::Button(
                                         fmt::format("{}##nma_rm_{}",
                                                     TR("ui.edit.note_metadata."
                                                        "add_btn")
                                                         .data(),
                                                     groupIdx)
                                             .c_str()) ) {
                                    std::string nk = buf.key;
                                    if ( !nk.empty() ) {
                                        for ( auto e : group.entities ) {
                                            auto& nc =
                                                registry
                                                    .get<Logic::NoteComponent>(
                                                        e);
                                            nc.m_metadata
                                                .note_properties[metaType][nk] =
                                                buf.val;
                                        }
                                        buf.key[0] = '\0';
                                        buf.val[0] = '\0';
                                    }
                                }

                                ImGui::EndTabItem();
                            }

                            ImGui::EndTabBar();
                        }
                    }

                    ImGui::PopID();

                    if ( groupIdx < static_cast<int>(groups.size()) ) {
                        ImGui::Spacing();
                    }
                }

                ImGui::EndChild();
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
