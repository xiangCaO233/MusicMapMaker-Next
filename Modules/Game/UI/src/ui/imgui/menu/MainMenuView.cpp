#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/imgui/menu/MainMenuView.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveConflictEvent.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/manager/NewProjectWizard.h"
#include "ui/imgui/tools/BpmMeasurementToolView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <concurrentqueue.h>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <system_error>

namespace MMM::UI
{
namespace
{
/// @brief 跨线程传递给 UI 帧内消费的保存提示载荷。
struct SaveTooltipPayload {
    /// @brief 目标文件路径，使用 UTF-8 字符串。
    std::string path;
    /// @brief 是否为成功状态。
    bool success{ true };
    /// @brief 是否来自另存为/导出流程。
    bool isExport{ false };
};

/// @brief 跨线程传递给 UI 帧内消费的保存冲突确认载荷。
struct SaveConflictPayload {
    /// @brief 存在覆盖风险的目标路径，使用 UTF-8 字符串。
    std::string path;
};

/// @brief 获取保存结果提示队列。
moodycamel::ConcurrentQueue<SaveTooltipPayload>& getSaveTooltipQueue()
{
    static moodycamel::ConcurrentQueue<SaveTooltipPayload> queue;
    return queue;
}

/// @brief 获取保存冲突确认队列。
moodycamel::ConcurrentQueue<SaveConflictPayload>& getSaveConflictQueue()
{
    static moodycamel::ConcurrentQueue<SaveConflictPayload> queue;
    return queue;
}

/// @brief 根据保存结果事件构建用户可见的提示文本。
std::string buildSaveTooltipMessage(const SaveTooltipPayload& payload)
{
    auto       path      = Config::utf8ToPath(payload.path);
    const bool isPackage = findPackageSupportedFileTypes(
                               Config::pathToUtf8(path.extension())) != nullptr;
    if ( !payload.success ) {
        if ( isPackage ) return "打包失败";
        return payload.isExport ? "导出失败" : "保存失败";
    }
    if ( !payload.isExport ) {
        return TR("ui.status.beatmap.saved").data();
    }

    std::string fileName = Config::pathToUtf8(path.filename());
    if ( fileName.empty() ) {
        if ( isPackage ) return "打包成功";
        return "导出成功";
    }
    if ( isPackage ) {
        return "打包 " + fileName + " 成功";
    }
    return "导出 " + fileName + " 成功";
}

/// @brief 订阅逻辑层保存结果事件，将事件转交 UI 帧内处理。
void ensureSaveResultSubscription()
{
    static bool subscribed = false;
    if ( subscribed ) return;

    Event::EventBus::instance().subscribe<Event::BeatmapSaveResultEvent>(
        [](const Event::BeatmapSaveResultEvent& event) {
            getSaveTooltipQueue().enqueue(SaveTooltipPayload{
                .path     = event.path,
                .success  = event.success,
                .isExport = event.isExport,
            });
        });
    subscribed = true;
}

/// @brief 订阅逻辑层保存冲突事件，将事件转交 UI 帧内处理。
void ensureSaveConflictSubscription()
{
    static bool subscribed = false;
    if ( subscribed ) return;

    Event::EventBus::instance().subscribe<Event::BeatmapSaveConflictEvent>(
        [](const Event::BeatmapSaveConflictEvent& event) {
            getSaveConflictQueue().enqueue(SaveConflictPayload{
                .path = event.path,
            });
        });
    subscribed = true;
}

/// @brief 将项目谱面路径规范化为候选比较键。
/// @param projectRoot 当前项目根目录。
/// @param path 谱面路径，可为项目相对路径或绝对路径。
/// @return 规范化后的 UTF-8 路径键。
std::string makeDataSourceBeatmapPathKey(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( path.empty() ) return {};

    std::filesystem::path fullPath =
        path.is_absolute() ? path : (projectRoot / path);
    std::error_code filesystemError;
    auto            canonicalPath = std::filesystem::weakly_canonical(
        fullPath.lexically_normal(), filesystemError);
    if ( !filesystemError ) {
        fullPath = canonicalPath;
    }
    return Config::pathToUtf8Generic(fullPath.lexically_normal());
}
}  // namespace

/// @brief 构造主菜单视图并初始化菜单状态、弹窗状态和更新检查器。
MainMenuView::MainMenuView()
    : m_openFileMenuNextFrame(false)
    , m_openEditMenuNextFrame(false)
    , m_openToolsMenuNextFrame(false)
    , m_openViewMenuNextFrame(false)
    , m_openHelpMenuNextFrame(false)
    , m_closeFileMenuNextFrame(false)
    , m_closeEditMenuNextFrame(false)
    , m_closeToolsMenuNextFrame(false)
    , m_closeViewMenuNextFrame(false)
    , m_closeHelpMenuNextFrame(false)
    , m_showOverlapCheckWindow(false)
    , m_showMetadataEditorWindow(false)
    , m_showNoteMetadataEditorWindow(false)
    , m_hasOverlapScan(false)
    , m_showAboutPopup(false)
    , m_showUpdatePopup(false)
    , m_showCheckingPopup(false)
    , m_updatePopupCanceled(false)
    , m_showBeatmapSpeedExportPopup(false)
    , m_speedExportRunning(false)
    , m_updateChecker(std::make_unique<MMM::Network::UpdateChecker>())
{
    ensureSaveResultSubscription();
    ensureSaveConflictSubscription();
}

/// @brief 销毁主菜单视图。
MainMenuView::~MainMenuView() {}

/// @brief 将逻辑命令发布到事件总线。
/// @param cmd 需要分发给逻辑层的命令。
void MainMenuView::dispatchCommand(const MMM::Logic::LogicCommand& cmd)
{
    Event::EventBus::instance().publish(Event::LogicCommandEvent(cmd));
}

/// @brief 处理主菜单相关的全局快捷键。
/// @param sourceManager 当前 UI 管理器，用于打开向导或访问视图。
void MainMenuView::handleHotkeys(UIManager* sourceManager)
{
    auto* project    = Logic::EditorEngine::instance().getCurrentProject();
    bool  hasProject = (project != nullptr);

    ImGuiIO& io = ImGui::GetIO();

    // 如果 ImGui 当前处于文本输入状态，跳过全局快捷键处理以防冲突 (如 Ctrl+A
    // 全选)
    if ( io.WantTextInput ) return;
    if ( ShortcutUtils::isShortcutRecordingActive() ) return;

    // 只有在没有文本输入激活时才处理快捷键，除非是 Ctrl 组合键
    if ( ImGui::IsAnyItemActive() && !io.KeyCtrl ) return;

    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    if ( ShortcutUtils::isShortcutPressed(
             settings.shortcutConfig.mirrorPaste) ) {
        dispatchCommand(Logic::CmdPaste{ true, settings.selectPastedObjects });
        return;
    }
    if ( ShortcutUtils::isShortcutPressed(settings.shortcutConfig.mirror) ) {
        dispatchCommand(Logic::CmdMirrorSelected{});
        return;
    }

    if ( io.KeyCtrl ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_N) ) {
            if ( io.KeyShift ) {
                auto* wizard = sourceManager->getView<NewProjectWizard>(
                    "NewProjectWizard");
                if ( wizard ) wizard->open();
            } else if ( hasProject ) {
                auto* wizard = sourceManager->getView<NewBeatmapWizard>(
                    "NewBeatmapWizard");
                if ( wizard ) wizard->open();
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_I, false) ) {
            openAudioImportPicker();
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_O) ) {
            openFolderPicker();
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_S) ) {
            if ( io.KeyShift ) {
                openExportFilePicker("");
            } else {
                dispatchCommand(Logic::CmdSaveBeatmap{});
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_Z) ) {
            if ( io.KeyShift ) {
                dispatchCommand(Logic::CmdRedo{});
            } else {
                dispatchCommand(Logic::CmdUndo{});
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_Y) ) {
            dispatchCommand(Logic::CmdRedo{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_C, false) ) {
            dispatchCommand(Logic::CmdCopy{});
        }
        if ( !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_V, false) ) {
            dispatchCommand(
                Logic::CmdPaste{ false, settings.selectPastedObjects });
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_X, false) ) {
            dispatchCommand(Logic::CmdCut{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_A, false) ) {
            dispatchCommand(Logic::CmdSelectAll{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_F, false) ) {
            dispatchCommand(Logic::CmdAlignSelectedToCommonBeats{});
        }
    } else if ( io.KeyAlt ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_F, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.file")) ) {
                m_closeFileMenuNextFrame = true;
            } else {
                m_openFileMenuNextFrame = true;
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_E, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.edit")) ) {
                m_closeEditMenuNextFrame = true;
            } else {
                m_openEditMenuNextFrame = true;
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_T, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.tools")) ) {
                m_closeToolsMenuNextFrame = true;
            } else {
                m_openToolsMenuNextFrame = true;
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_V, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.view")) ) {
                m_closeViewMenuNextFrame = true;
            } else {
                m_openViewMenuNextFrame = true;
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_H, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.help")) ) {
                m_closeHelpMenuNextFrame = true;
            } else {
                m_openHelpMenuNextFrame = true;
            }
        }
    } else if ( !io.KeySuper && !io.KeyShift ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_Space, false) ) {
            bool playing = Logic::EditorEngine::instance().isPlaybackPlaying();
            dispatchCommand(Logic::CmdSetPlayState{ !playing });
        }
    }
}

/// @brief 更新主菜单计时器、弹窗和启动检查状态。
/// @param sourceManager 当前 UI 管理器，用于处理快捷键和菜单窗口。
void MainMenuView::update(UIManager* sourceManager)
{
    consumeBeatmapSpeedExportQueues();

    SaveTooltipPayload payload;
    while ( getSaveTooltipQueue().try_dequeue(payload) ) {
        m_saveTooltipMessage = buildSaveTooltipMessage(payload);
        m_saveTooltipSuccess = payload.success;
        m_saveTooltipTimer   = payload.success ? 2.0f : 3.0f;
    }

    SaveConflictPayload conflictPayload;
    while ( getSaveConflictQueue().try_dequeue(conflictPayload) ) {
        m_pendingSaveConflictPath = conflictPayload.path;
        m_showSaveConflictWarning = true;
    }

    if ( m_statusMessageTimer > 0.0f )
        m_statusMessageTimer -= ImGui::GetIO().DeltaTime;

    // 启动时自动检查更新
    if ( !m_hasCheckedOnStartup ) {
        m_hasCheckedOnStartup = true;

        // 先检查是否刚完成更新
        if ( MMM::Network::UpdateChecker::checkStartupUpdateMarker() ) {
            m_showUpdateSuccessPopup = true;
        } else {
            m_isSilentCheck = true;  // 静默检查
            m_updateChecker->checkAsync();
        }
    }

    // 如果是静默检查，监测状态
    if ( m_isSilentCheck ) {
        auto info = m_updateChecker->getInfo();
        if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
            m_showUpdatePopup = true;
            m_isSilentCheck   = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kUpToDate ) {
            m_statusMessage      = TR("ui.help.up_to_date").data();
            m_statusMessageTimer = 5.0f;
            m_isSilentCheck      = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
            m_isSilentCheck = false;
        }
    }

    renderSaveTooltip();
}

/// @brief 渲染保存目标被外部修改时的覆盖确认弹窗。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderSaveConflictWarningPopup(float dpiScale)
{
    constexpr const char* popupId =
        "文件已被另外修改过###SaveConflictWarningModal";
    if ( m_showSaveConflictWarning ) {
        ImGui::OpenPopup(popupId);
        m_showSaveConflictWarning = false;
    }

    if ( !ImGui::IsPopupOpen(popupId) ) return;

    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(540.0f * dpiScale, 0.0f)) ) {
            ImGui::TextWrapped(
                "文件已被另外修改过，强行覆盖可能会导致丢失数据，是否确认？");
            if ( !m_pendingSaveConflictPath.empty() ) {
                ImGui::Spacing();
                ImGui::TextWrapped("目标文件：%s",
                                   m_pendingSaveConflictPath.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
            if ( ImGui::Button("确认覆盖", buttonSize) ) {
                dispatchCommand(Logic::CmdSaveBeatmap{
                    .allowExternallyModifiedOverwrite = true,
                });
                m_pendingSaveConflictPath.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.common.cancel").data(), buttonSize) ) {
                m_pendingSaveConflictPath.clear();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
}

/// @brief 收集可用于替换当前焦点谱面的项目谱面候选。
/// @return 数据来源候选列表。
std::vector<MainMenuView::DataSourceReplaceCandidate>
MainMenuView::collectDataSourceReplaceCandidates() const
{
    std::vector<DataSourceReplaceCandidate> candidates;

    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) return candidates;

    std::string activePathKey;
    {
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( session && session->getContext().currentBeatmap ) {
            activePathKey = makeDataSourceBeatmapPathKey(
                project->m_projectRoot,
                session->getContext()
                    .currentBeatmap->m_baseMapMetadata.map_path);
        }
    }

    candidates.reserve(project->m_beatmaps.size());
    for ( const auto& entry : project->m_beatmaps ) {
        if ( entry.m_filePath.empty() ) continue;

        auto relativePath =
            Config::utf8ToPath(entry.m_filePath).lexically_normal();
        auto candidatePathKey =
            makeDataSourceBeatmapPathKey(project->m_projectRoot, relativePath);
        if ( candidatePathKey.empty() || candidatePathKey == activePathKey ) {
            continue;
        }

        std::error_code filesystemError;
        const auto      fullPath =
            (project->m_projectRoot / relativePath).lexically_normal();
        if ( !std::filesystem::is_regular_file(fullPath, filesystemError) ||
             filesystemError ) {
            continue;
        }

        std::string displayName =
            entry.m_name.empty() ? entry.m_filePath : entry.m_name;
        candidates.push_back(DataSourceReplaceCandidate{
            .relativePath = Config::pathToUtf8Generic(relativePath),
            .displayName  = displayName,
        });
    }

    std::sort(candidates.begin(),
              candidates.end(),
              [](const auto& lhs, const auto& rhs) {
                  if ( lhs.displayName != rhs.displayName ) {
                      return lhs.displayName < rhs.displayName;
                  }
                  return lhs.relativePath < rhs.relativePath;
              });
    return candidates;
}

/// @brief 提交数据来源替换请求。
void MainMenuView::submitDataSourceReplaceRequest()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ||
         m_dataSourceReplacePath.empty() ) {
        m_statusMessage      = "没有可用的数据来源谱面";
        m_statusMessageTimer = 3.0f;
        return;
    }

    if ( !m_replaceObjectsFromDataSource && !m_replaceTimelinesFromDataSource &&
         !m_replaceMetadataFromDataSource ) {
        m_statusMessage      = "至少选择一种要替换的数据";
        m_statusMessageTimer = 3.0f;
        return;
    }

    const auto sourcePath =
        (project->m_projectRoot / Config::utf8ToPath(m_dataSourceReplacePath))
            .lexically_normal();
    auto sourceBeatmap =
        std::make_shared<MMM::BeatMap>(MMM::BeatMap::loadFromFile(sourcePath));
    if ( sourceBeatmap->m_baseMapMetadata.map_path.empty() ) {
        m_statusMessage      = "读取数据来源谱面失败";
        m_statusMessageTimer = 3.0f;
        return;
    }

    dispatchCommand(Logic::CmdReplaceBeatmapData{
        .sourceBeatmap    = sourceBeatmap,
        .replaceObjects   = m_replaceObjectsFromDataSource,
        .replaceTimelines = m_replaceTimelinesFromDataSource,
        .replaceMetadata  = m_replaceMetadataFromDataSource,
    });

    m_statusMessage      = "已替换当前谱面数据";
    m_statusMessageTimer = 3.0f;
}

/// @brief 渲染数据来源替换工具窗口。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderDataSourceReplaceWindow(float dpiScale)
{
    constexpr const char* popupId = "数据来源替换工具###DataSourceReplaceModal";
    if ( m_showDataSourceReplaceWindow ) {
        ImGui::OpenPopup(popupId);
    }

    if ( !ImGui::IsPopupOpen(popupId) ) return;

    bool closePopup = false;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_NoCollapse,
                              ImVec2(640.0f * dpiScale, 480.0f * dpiScale),
                              false) ) {
            auto candidates = collectDataSourceReplaceCandidates();
            if ( m_dataSourceReplacePath.empty() && !candidates.empty() ) {
                m_dataSourceReplacePath = candidates.front().relativePath;
            }
            if ( !m_dataSourceReplacePath.empty() &&
                 std::none_of(candidates.begin(),
                              candidates.end(),
                              [&](const auto& candidate) {
                                  return candidate.relativePath ==
                                         m_dataSourceReplacePath;
                              }) ) {
                m_dataSourceReplacePath = candidates.empty()
                                              ? std::string{}
                                              : candidates.front().relativePath;
            }

            ImGui::TextUnformatted("数据来源谱面");
            ImGui::Spacing();
            const float listHeight =
                std::max(120.0f * dpiScale,
                         ImGui::GetContentRegionAvail().y - 126.0f * dpiScale);
            if ( ImGui::BeginChild("DataSourceReplaceBeatmapList",
                                   ImVec2(0.0f, listHeight),
                                   true) ) {
                if ( candidates.empty() ) {
                    ImGui::TextDisabled("没有找到其他项目谱面。");
                } else {
                    for ( const auto& candidate : candidates ) {
                        const bool selected =
                            candidate.relativePath == m_dataSourceReplacePath;
                        std::string label = candidate.displayName + " - " +
                                            candidate.relativePath;
                        if ( ImGui::Selectable(label.c_str(), selected) ) {
                            m_dataSourceReplacePath = candidate.relativePath;
                        }
                    }
                }
            }
            ImGui::EndChild();

            ImGui::Spacing();
            ImGui::Checkbox("物件数据源", &m_replaceObjectsFromDataSource);
            ImGui::SameLine();
            ImGui::Checkbox("时间线源", &m_replaceTimelinesFromDataSource);
            ImGui::SameLine();
            ImGui::Checkbox("元数据源", &m_replaceMetadataFromDataSource);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const bool   canApply = !candidates.empty() &&
                                    !m_dataSourceReplacePath.empty() &&
                                    (m_replaceObjectsFromDataSource ||
                                     m_replaceTimelinesFromDataSource ||
                                     m_replaceMetadataFromDataSource);
            const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
            if ( !canApply ) ImGui::BeginDisabled();
            if ( ImGui::Button("替换", buttonSize) ) {
                submitDataSourceReplaceRequest();
                closePopup = true;
            }
            if ( !canApply ) ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.common.cancel").data(), buttonSize) ) {
                closePopup = true;
            }

            if ( closePopup ) {
                m_showDataSourceReplaceWindow = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

/// @brief 渲染文件、编辑、工具和帮助主菜单。
/// @param sourceManager 当前 UI 管理器，用于菜单项打开对应视图。
void MainMenuView::renderMenus(UIManager* sourceManager)
{
    handleHotkeys(sourceManager);

    Config::SkinManager& skinCfg = Config::SkinManager::instance();

    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(6.0f * dpiScale, ImGui::GetStyle().FramePadding.y));

    auto MenuItemWithFontIcon = [](const char* icon,
                                   const char* label,
                                   const char* shortcut = nullptr,
                                   bool        enabled  = true) -> bool {
        ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

        float gap = ImGui::CalcTextSize(" ").x * 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(gap, 0));

        const char* iconPtr = icon ? icon : "  ";

        bool clicked =
            ImGui::MenuItemEx(label, iconPtr, shortcut, false, enabled);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return clicked;
    };

    ImFont* menuFont = skinCfg.getFont("menu");
    if ( menuFont ) ImGui::PushFont(menuFont, menuFont->LegacySize);

    // ========== File Menu ==========
    if ( m_openFileMenuNextFrame ) {
        ImGui::OpenPopup(TR("ui.file"));
        m_openFileMenuNextFrame = false;
    }
    if ( ImGui::BeginMenu(TR("ui.file")) ) {
        if ( m_closeFileMenuNextFrame ) {
            ImGui::CloseCurrentPopup();
            m_closeFileMenuNextFrame = false;
        }

        auto* project    = Logic::EditorEngine::instance().getCurrentProject();
        bool  hasProject = (project != nullptr);

        if ( MenuItemWithFontIcon(
                 ICON_MMM_BOOK, TR("ui.file.new_pro"), "Ctrl+Shift+N") ) {
            auto* wizard =
                sourceManager->getView<NewProjectWizard>("NewProjectWizard");
            if ( wizard ) wizard->open();
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_FILE, TR("ui.file.new_map"), "Ctrl+N", hasProject) ) {
            auto* wizard =
                sourceManager->getView<NewBeatmapWizard>("NewBeatmapWizard");
            if ( wizard ) wizard->open();
        }
        ImGui::Separator();

        if ( MenuItemWithFontIcon(
                 ICON_MMM_FOLDER_OPEN, TR("ui.file.open_pro"), "Ctrl+O") ) {
            openFolderPicker();
        }

        if ( MenuItemWithFontIcon(ICON_MMM_MUSIC,
                                  TR("ui.audio_manager.import_audio"),
                                  "Ctrl+I",
                                  hasProject) ) {
            openAudioImportPicker();
        }

        if ( ImGui::BeginMenu(TR("ui.file.open_recent")) ) {
            const auto& recent =
                Config::AppConfig::instance().getEditorConfig().recentProjects;
            if ( recent.empty() ) {
                ImGui::MenuItem(TR("ui.file.no_recent"), nullptr, false, false);
            } else {
                for ( size_t i = 0; i < recent.size(); ++i ) {
                    const auto&           path = recent[i];
                    std::filesystem::path p    = Config::utf8ToPath(path);
                    std::string name = Config::pathToUtf8(p.filename());
                    ImGui::PushID(static_cast<int>(i));
                    if ( ImGui::MenuItem(name.c_str(), path.c_str()) ) {
                        Event::OpenProjectEvent ev;
                        ev.m_projectPath = p;
                        Event::EventBus::instance().publish(ev);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndMenu();
        }

        if ( MenuItemWithFontIcon(ICON_MMM_CLOSE,
                                  TR("ui.file.close_pro"),
                                  nullptr,
                                  hasProject) ) {
            Event::EventBus::instance().publish(
                Event::ProjectCloseRequestedEvent{});
        }
        ImGui::Separator();

        if ( MenuItemWithFontIcon(
                 ICON_MMM_SAVE, TR("ui.file.save"), "Ctrl+S") ) {
            dispatchCommand(Logic::CmdSaveBeatmap{});
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_SAVE, TR("ui.file.save_as"), "Ctrl+Shift+S") ) {
            openExportFilePicker("");
        }

        if ( MenuItemWithFontIcon(
                 ICON_MMM_PACK, TR("ui.file.pack"), nullptr, hasProject) ) {
            openPackFilePicker();
        }
        ImGui::EndMenu();
    }

    // ========== Edit Menu ==========
    if ( m_openEditMenuNextFrame ) {
        ImGui::OpenPopup(TR("ui.edit"));
        m_openEditMenuNextFrame = false;
    }
    if ( ImGui::BeginMenu(TR("ui.edit")) ) {
        if ( m_closeEditMenuNextFrame ) {
            ImGui::CloseCurrentPopup();
            m_closeEditMenuNextFrame = false;
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_UNDO, TR("ui.edit.undo"), "Ctrl+Z") ) {
            dispatchCommand(Logic::CmdUndo{});
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_REDO, TR("ui.edit.redo"), "Ctrl+Y / Ctrl+Shift+Z") ) {
            dispatchCommand(Logic::CmdRedo{});
        }
        ImGui::Separator();
        if ( MenuItemWithFontIcon(
                 ICON_MMM_SCISSORS, TR("ui.edit.cut"), "Ctrl+X") ) {
            dispatchCommand(Logic::CmdCut{});
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_COPY, TR("ui.edit.copy"), "Ctrl+C") ) {
            dispatchCommand(Logic::CmdCopy{});
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_PASTE, TR("ui.edit.paste"), "Ctrl+V") ) {
            dispatchCommand(Logic::CmdPaste{ false,
                                             Config::AppConfig::instance()
                                                 .getEditorSettings()
                                                 .selectPastedObjects });
        }
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        std::string mirrorPasteShortcut =
            ShortcutUtils::formatShortcut(shortcutConfig.mirrorPaste);
        if ( MenuItemWithFontIcon(ICON_MMM_MIRROR,
                                  TR("ui.edit.mirror_paste"),
                                  mirrorPasteShortcut.empty()
                                      ? nullptr
                                      : mirrorPasteShortcut.c_str()) ) {
            dispatchCommand(Logic::CmdPaste{ true,
                                             Config::AppConfig::instance()
                                                 .getEditorSettings()
                                                 .selectPastedObjects });
        }
        std::string mirrorShortcut =
            ShortcutUtils::formatShortcut(shortcutConfig.mirror);
        if ( MenuItemWithFontIcon(
                 ICON_MMM_MIRROR,
                 TR("ui.edit.mirror"),
                 mirrorShortcut.empty() ? nullptr : mirrorShortcut.c_str()) ) {
            dispatchCommand(Logic::CmdMirrorSelected{});
        }
        ImGui::Separator();
        if ( MenuItemWithFontIcon(
                 ICON_MMM_SELECT_ALL, TR("ui.edit.select_all"), "Ctrl+A") ) {
            dispatchCommand(Logic::CmdSelectAll{});
        }
        {
            bool hasSelection = false;
            auto engine       = &Logic::EditorEngine::instance();
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine->getSessionMutex());
            auto session = engine->getActiveSession();
            if ( session ) {
                auto selView =
                    session->getContext()
                        .noteRegistry.view<const Logic::InteractionComponent>();
                for ( auto e : selView ) {
                    if ( selView.get<const Logic::InteractionComponent>(e)
                             .isSelected ) {
                        hasSelection = true;
                        break;
                    }
                }
            }
            if ( MenuItemWithFontIcon(ICON_MMM_COG,
                                      TR("ui.edit.note_metadata"),
                                      nullptr,
                                      hasSelection) ) {
                m_showNoteMetadataEditorWindow = true;
            }
        }
        ImGui::Separator();
        bool playing      = Logic::EditorEngine::instance().isPlaybackPlaying();
        const char* pIcon = playing ? ICON_MMM_PAUSE : ICON_MMM_PLAY;
        if ( MenuItemWithFontIcon(pIcon, TR("ui.edit.play_pause"), "Space") ) {
            dispatchCommand(Logic::CmdSetPlayState{ !playing });
        }
        ImGui::Separator();
        if ( MenuItemWithFontIcon(ICON_MMM_FILE,
                                  TR("ui.edit.beatmap_settings")) ) {
            sourceManager->openSettingsWindow(Event::SettingsTab::Beatmap);
        }
        ImGui::EndMenu();
    }

    // ========== Tools Menu ==========
    if ( m_openToolsMenuNextFrame ) {
        ImGui::OpenPopup(TR("ui.tools"));
        m_openToolsMenuNextFrame = false;
    }
    if ( ImGui::BeginMenu(TR("ui.tools")) ) {
        if ( m_closeToolsMenuNextFrame ) {
            ImGui::CloseCurrentPopup();
            m_closeToolsMenuNextFrame = false;
        }

        auto* project    = Logic::EditorEngine::instance().getCurrentProject();
        bool  hasProject = (project != nullptr);

        if ( MenuItemWithFontIcon(ICON_MMM_MUSIC,
                                  TR("ui.tools.bpm_measure"),
                                  nullptr,
                                  hasProject) ) {
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
                tool->openWithAudioTrack("");
            }
        }

        if ( MenuItemWithFontIcon(ICON_MMM_SELECT_ALL,
                                  TR("ui.tools.overlap_check")) ) {
            m_showOverlapCheckWindow = !m_showOverlapCheckWindow;
        }

        if ( MenuItemWithFontIcon(ICON_MMM_COG, "谱面额外元数据编辑") ) {
            m_showMetadataEditorWindow = !m_showMetadataEditorWindow;
        }

        {
            bool  hasActiveBeatmap = false;
            auto& engine           = Logic::EditorEngine::instance();
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();
            hasActiveBeatmap =
                hasProject && session && session->getContext().currentBeatmap;
            if ( MenuItemWithFontIcon(ICON_MMM_BARS,
                                      "数据来源替换工具",
                                      nullptr,
                                      hasActiveBeatmap) ) {
                m_showDataSourceReplaceWindow = true;
            }
        }

        if ( MenuItemWithFontIcon(
                 ICON_MMM_BARS, TR("ui.tools.format"), "Ctrl+F") ) {
            dispatchCommand(Logic::CmdAlignSelectedToCommonBeats{});
        }

        {
            bool  hasBeatmap = false;
            auto& engine     = Logic::EditorEngine::instance();
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();
            hasBeatmap =
                hasProject && session && session->getContext().currentBeatmap;
            if ( MenuItemWithFontIcon(ICON_MMM_MUSIC,
                                      "谱面倍速制作",
                                      nullptr,
                                      hasBeatmap && !m_speedExportRunning) ) {
                openBeatmapSpeedExportPopup();
            }
        }

        ImGui::EndMenu();
    }

    // ========== View Menu ==========
    if ( m_openViewMenuNextFrame ) {
        ImGui::OpenPopup(TR("ui.view"));
        m_openViewMenuNextFrame = false;
    }
    if ( ImGui::BeginMenu(TR("ui.view")) ) {
        if ( m_closeViewMenuNextFrame ) {
            ImGui::CloseCurrentPopup();
            m_closeViewMenuNextFrame = false;
        }

        auto& appConfig      = Config::AppConfig::instance();
        auto& editorSettings = appConfig.getEditorSettings();
        bool  viewChanged    = false;

        viewChanged |= ImGui::MenuItem(TR("ui.view.timeline").data(),
                                       nullptr,
                                       &editorSettings.showTimelineWindow);
        viewChanged |= ImGui::MenuItem(TR("ui.view.preview").data(),
                                       nullptr,
                                       &editorSettings.showPreviewWindow);
        ImGui::Separator();
        viewChanged |= ImGui::MenuItem(TR("ui.view.show_tool_labels").data(),
                                       nullptr,
                                       &editorSettings.showToolLabels);
        viewChanged |= ImGui::MenuItem(TR("ui.view.fixed_tool_window").data(),
                                       nullptr,
                                       &editorSettings.fixedToolWindow);
        viewChanged |= ImGui::MenuItem(TR("ui.view.show_manager_labels").data(),
                                       nullptr,
                                       &editorSettings.showManagerLabels);

        if ( viewChanged ) {
            appConfig.save();
        }
        ImGui::EndMenu();
    }

    // ========== Help Menu ==========
    renderHelpMenu(sourceManager);

    // ========== Popups ==========
    renderAboutPopup();
    renderUpdateCheckingPopup();
    renderUpdatePopup();
    renderUpdateSuccessPopup();
    renderOverlapCheckWindow();
    renderMetadataEditorWindow();
    renderNoteMetadataEditorWindow();
    renderDataSourceReplaceWindow(dpiScale);
    renderSaveConflictWarningPopup(dpiScale);
    renderExportFormatPickerPopup(dpiScale);
    renderExportCompatibilityWarningPopup(dpiScale);
    renderPackageFormatPickerPopup(dpiScale);
    renderPackageFileSelectionWindow(dpiScale);
    renderPackageBeatmapMetadataWindow(dpiScale);
    renderBeatmapSpeedExportPopup(dpiScale);

    if ( menuFont ) ImGui::PopFont();
    ImGui::PopStyleVar(2);  // Pop WindowPadding and FramePadding
}

/// @brief 渲染底部提示文本占位区域。
void MainMenuView::renderInfoText()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("MusicMapMaker(Gamma)");
    ImGui::SameLine();
    ImGui::Text(
        "%.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
}

}  // namespace MMM::UI
