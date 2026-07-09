#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"

#include <ImGuiFileDialog.h>
#include <filesystem>
#include <imgui.h>
#include <mutex>
#include <nfd.h>
#include <string>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 打开另存为流程动作，拥有导出格式和文件选择相关状态。
class SaveBeatmapAsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在存在活跃谱面时允许另存为。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return MenuUtil::hasActiveBeatmap(false);
    }

    /// @brief 打开另存为流程。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        openExportFilePicker("");
    }

    /// @brief 消费 Ctrl+Shift+S 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        (void)context;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S) ) {
            openExportFilePicker("");
            return true;
        }
        return false;
    }

    /// @brief 渲染另存为动作拥有的延迟窗口，并消费快捷键请求。
    /// @warning UI 热路径：每帧检查布尔状态；文件选择器只由用户触发打开。
    void renderDeferred(MainMenuContext& context) override
    {
        renderExportFormatPickerPopup(context.dpiScale);
        renderSaveAsFileDialog(context.dpiScale);
        renderOverwriteWarningPopup(context.dpiScale);
        renderExportCompatibilityWarningPopup(context.dpiScale);
    }

private:
    /// @brief 直接分发谱面导出命令。
    /// @param path 目标导出路径。
    /// @param addStoreModeExtForMalodyExport 是否为 MC 导出写入上架皮肤
    /// mode_ext。
    void dispatchSaveBeatmapAs(const std::string& path,
                               bool addStoreModeExtForMalodyExport = false)
    {
        MenuUtil::dispatchCommand(Logic::CmdSaveBeatmapAs{
            .addStoreModeExtForMalodyExport = addStoreModeExtForMalodyExport,
            .path                           = path,
        });
    }

    /// @brief 请求导出当前谱面，必要时先展示格式兼容性警告。
    /// @param path 目标导出路径。
    void requestSaveBeatmapAs(std::string path)
    {
        auto warnings = MenuUtil::collectExportCompatibilityWarnings(path);
        const bool showStoreModeExtOption =
            MenuUtil::shouldOfferMalodyStoreModeExtForCurrentExport(path);
        if ( warnings.empty() && !showStoreModeExtOption ) {
            dispatchSaveBeatmapAs(path);
            return;
        }

        const std::string ext   = MenuUtil::lowerExtension(path);
        m_pendingExportPath     = std::move(path);
        m_pendingExportWarnings = std::move(warnings);
        m_pendingExportFormatName =
            (ext == ".osu") ? "osu!"
                            : ((ext == ".imd") ? "RM" : "Malody Chart");
        m_pendingExportShowStoreModeExtOption = showStoreModeExtOption;
        m_pendingExportAddStoreModeExt =
            Config::AppConfig::instance()
                .getEditorSettings()
                .autoAddStoreModeExtForMalodyExport;
        m_showExportCompatibilityWarning = true;
    }

    /// @brief 根据用户选择路径处理覆盖确认或导出请求。
    /// @param path 文件选择器返回的 UTF-8 路径。
    void handleSelectedExportPath(std::string path)
    {
        if ( path.empty() ) return;
        if ( std::filesystem::exists(Config::utf8ToPath(path)) ) {
            m_pendingOverwritePath = std::move(path);
            m_showOverwriteWarning = true;
            return;
        }
        requestSaveBeatmapAs(std::move(path));
    }

    /// @brief 打开谱面导出保存路径选择器。
    /// @param ext 期望导出的文件扩展名；为空时展示全部支持格式。
    /// @warning 用户触发的低频路径：原生选择器可能阻塞。
    void openExportFilePicker(const std::string& ext)
    {
        auto& config = Config::AppConfig::instance().getEditorSettings();
        if ( config.filePickerStyle == Config::FilePickerStyle::Native &&
             ext.empty() ) {
            m_showExportFormatPicker = true;
            return;
        }

        const std::string defaultPath = MenuUtil::getSaveAsPickerDefaultPath();

        std::string defaultName = "map" + (ext.empty() ? ".mmm" : ext);
        auto&       engine      = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( session && session->getContext().currentBeatmap ) {
            auto& meta =
                session->getContext().currentBeatmap->m_baseMapMetadata;
            if ( ext == ".imd" ) {
                defaultName = MenuUtil::makeExportFileNameForExtension(
                    ".imd", defaultName);
            } else {
                defaultName = meta.name + (ext.empty() ? ".mmm" : ext);
            }
        }

        if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
            nfdu8char_t*      outPath = nullptr;
            nfdu8filteritem_t filters[4];
            int               filterCount = 0;

            if ( ext == ".mmm" || ext == "" ) {
                filters[filterCount++] = { "MusicMapMaker Beatmap", "mmm" };
            }
            if ( ext == ".osu" || ext == "" ) {
                filters[filterCount++] = { "osu!mania Beatmap", "osu" };
            }
            if ( ext == ".imd" || ext == "" ) {
                filters[filterCount++] = { "RM Beatmap", "imd" };
            }
            if ( ext == ".mc" || ext == "" ) {
                filters[filterCount++] = { "Malody Chart", "mc" };
            }

            nfdresult_t result = NFD_SaveDialogU8(&outPath,
                                                  filters,
                                                  filterCount,
                                                  defaultPath.c_str(),
                                                  defaultName.c_str());

            if ( result == NFD_OKAY ) {
                handleSelectedExportPath(outPath);
                NFD_FreePath(outPath);
            }
            return;
        }

        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = defaultPath;
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = defaultName;
        fdConfig.flags =
            ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;

        std::string filterStr;
        if ( ext == ".mmm" )
            filterStr = ".mmm";
        else if ( ext == ".osu" )
            filterStr = ".osu";
        else if ( ext == ".imd" )
            filterStr = ".imd";
        else if ( ext == ".mc" )
            filterStr = ".mc";
        else
            filterStr = ".mmm,.osu,.imd,.mc";

        ImGuiFileDialog::Instance()->OpenDialog("SaveAsFilePicker",
                                                TR("ui.file.save_as"),
                                                filterStr.c_str(),
                                                fdConfig);
    }

    /// @brief 渲染统一文件选择器并消费另存为路径。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：仅在统一文件选择器打开时绘制。
    void renderSaveAsFileDialog(float dpiScale)
    {
        Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
        if ( ImGuiFileDialog::Instance()->IsOpened("SaveAsFilePicker") ) {
            Utils::prepareCenteredModalWindow({ 600, 400 });
        }
        if ( ImGuiFileDialog::Instance()->Display(
                 "SaveAsFilePicker",
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings,
                 { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                std::string filePath =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                filePath = MenuUtil::applySaveAsSelectedFormatToPath(filePath);
                handleSelectedExportPath(std::move(filePath));
            }
            ImGuiFileDialog::Instance()->Close();
        }
    }

    /// @brief 渲染原生另存为对话框前的导出格式选择弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderExportFormatPickerPopup(float dpiScale)
    {
        constexpr const char* popupId =
            "选择导出格式###ExportFormatPickerWindow";
        if ( !m_showExportFormatPicker ) {
            return;
        }

        std::string selectedExtension;
        bool        closeWindow = false;
        {
            Utils::CenteredModalPopupScope popupStyle(dpiScale);
            bool                           isOpen = true;
            if ( popupStyle.beginWindow(
                     popupId,
                     &isOpen,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking,
                     ImVec2(360.0f * dpiScale, 0.0f)) ) {
                ImGui::TextUnformatted("选择另存为格式：");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                const ImVec2 buttonSize(300.0f * dpiScale, 0.0f);
                if ( MenuUtil::drawCenteredButton(
                         "MusicMapMaker Beatmap (.mmm)", buttonSize) ) {
                    selectedExtension = ".mmm";
                }
                if ( MenuUtil::drawCenteredButton("osu!mania Beatmap (.osu)",
                                                  buttonSize) ) {
                    selectedExtension = ".osu";
                }
                if ( MenuUtil::drawCenteredButton("RM Beatmap (.imd)",
                                                  buttonSize) ) {
                    selectedExtension = ".imd";
                }
                if ( MenuUtil::drawCenteredButton("Malody Chart (.mc)",
                                                  buttonSize) ) {
                    selectedExtension = ".mc";
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if ( MenuUtil::drawCenteredButton(
                         TR("ui.common.cancel").data(),
                         ImVec2(120.0f * dpiScale, 0.0f)) ) {
                    closeWindow = true;
                }

                if ( !selectedExtension.empty() ) {
                    closeWindow = true;
                }
            }
            ImGui::End();

            if ( !isOpen ) {
                closeWindow = true;
            }
        }

        if ( closeWindow ) {
            m_showExportFormatPicker = false;
        }
        if ( !selectedExtension.empty() ) {
            openExportFilePicker(selectedExtension);
        }
    }

    /// @brief 渲染导出覆盖确认弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderOverwriteWarningPopup(float dpiScale)
    {
        constexpr const char* popupId =
            "确认覆盖导出文件###SaveAsOverwriteWarningModal";
        if ( m_showOverwriteWarning ) {
            ImGui::OpenPopup(popupId);
            m_showOverwriteWarning = false;
        }

        if ( !ImGui::IsPopupOpen(popupId) ) return;

        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(540.0f * dpiScale, 0.0f)) ) {
            ImGui::TextWrapped("目标文件已经存在，是否覆盖？");
            if ( !m_pendingOverwritePath.empty() ) {
                ImGui::Spacing();
                ImGui::TextWrapped("目标文件：%s",
                                   m_pendingOverwritePath.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
            if ( ::MMM::UI::FeedbackButton("确认覆盖", buttonSize) ) {
                requestSaveBeatmapAs(m_pendingOverwritePath);
                m_pendingOverwritePath.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                m_pendingOverwritePath.clear();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    /// @brief 清空当前导出兼容性确认弹窗状态。
    void clearCompatibilityWarningState()
    {
        m_pendingExportPath.clear();
        m_pendingExportFormatName.clear();
        m_pendingExportWarnings.clear();
        m_pendingExportShowStoreModeExtOption = false;
        m_pendingExportAddStoreModeExt        = false;
    }

    /// @brief 渲染导出兼容性警告弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderExportCompatibilityWarningPopup(float dpiScale)
    {
        constexpr const char* popupId = "谱面兼容性警告###ExportWarningModal";
        if ( m_showExportCompatibilityWarning ) {
            ImGui::OpenPopup(popupId);
            m_showExportCompatibilityWarning = false;
        }

        if ( !ImGui::IsPopupOpen(popupId) ) return;

        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0f * dpiScale, 0.0f)) ) {
            if ( m_pendingExportWarnings.empty() &&
                 m_pendingExportShowStoreModeExtOption ) {
                ImGui::Text("%s %s 前可以选择附加上架元数据：",
                            "导出",
                            m_pendingExportFormatName.c_str());
            } else {
                ImGui::Text("%s %s 前需要确认以下兼容性变化：",
                            "导出",
                            m_pendingExportFormatName.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            for ( const auto& warning : m_pendingExportWarnings ) {
                MenuUtil::drawWrappedBulletText(warning);
            }
            if ( m_pendingExportShowStoreModeExtOption ) {
                if ( !m_pendingExportWarnings.empty() ) {
                    ImGui::Spacing();
                }
                bool addStoreModeExt = m_pendingExportAddStoreModeExt;
                if ( ::MMM::UI::FeedbackCheckbox("自动添加上架皮肤 mode_ext",
                                                 &addStoreModeExt) ) {
                    m_pendingExportAddStoreModeExt = addStoreModeExt;
                    auto& settings =
                        Config::AppConfig::instance().getEditorSettings();
                    settings.autoAddStoreModeExtForMalodyExport =
                        addStoreModeExt;
                    Config::AppConfig::instance().save();
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip(
                        "%s",
                        "会替换导出 MC 的 mode_ext，用于 EX Rhythm Master VI "
                        "皮肤上架提示。");
                }
            }

            ImGui::Spacing();
            MenuUtil::drawWrappedLabelValue("目标文件：", m_pendingExportPath);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 actionButtonSize(120.0f * dpiScale, 0.0f);
            const float  actionButtonRowWidth =
                actionButtonSize.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            MenuUtil::centerNextItem(actionButtonRowWidth);
            if ( ::MMM::UI::FeedbackButton("继续导出", actionButtonSize) ) {
                dispatchSaveBeatmapAs(m_pendingExportPath,
                                      m_pendingExportAddStoreModeExt);
                clearCompatibilityWarningState();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           actionButtonSize) ) {
                clearCompatibilityWarningState();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    /// @brief 是否在下一帧打开原生另存为格式选择弹窗。
    bool m_showExportFormatPicker = false;
    /// @brief 是否在下一帧打开导出兼容性警告弹窗。
    bool m_showExportCompatibilityWarning = false;
    /// @brief 是否在下一帧打开覆盖确认弹窗。
    bool m_showOverwriteWarning = false;
    /// @brief 待确认覆盖的导出路径。
    std::string m_pendingOverwritePath;
    /// @brief 待确认导出的目标路径。
    std::string m_pendingExportPath;
    /// @brief 待确认导出的格式名称。
    std::string m_pendingExportFormatName;
    /// @brief 待确认导出的兼容性警告消息。
    std::vector<std::string> m_pendingExportWarnings;
    /// @brief 待确认导出是否显示上架 mode_ext 选项。
    bool m_pendingExportShowStoreModeExtOption = false;
    /// @brief 待确认导出是否写入上架 mode_ext。
    bool m_pendingExportAddStoreModeExt = false;
};
}  // namespace

/// @brief 创建打开另存为流程的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSaveBeatmapAsAction()
{
    return std::make_unique<SaveBeatmapAsAction>();
}

}  // namespace MMM::UI
