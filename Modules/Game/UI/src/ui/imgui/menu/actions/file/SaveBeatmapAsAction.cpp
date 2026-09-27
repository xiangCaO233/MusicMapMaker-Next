#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"

#include <ImGuiFileDialog.h>
#include <filesystem>
#include <imgui.h>
#include <mutex>
#include <nfd.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 打开另存为流程动作，拥有导出格式和文件选择相关状态。
/// @details 统一原生与内置选择器、覆盖确认、格式选项及兼容性警告。
/// @warning 文件对话框和导出均为用户触发的低频路径，逐帧阶段只绘制状态。
class SaveBeatmapAsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在存在活跃谱面时允许另存为。
    /// @param context 单帧主菜单上下文，本判断无需读取。
    /// @return 当前存在活动谱面时返回 true。
    /// @warning UI 热路径：只查询会话状态，不得打开选择器或访问文件系统。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return MenuUtil::hasActiveBeatmap(false);
    }

    /// @brief 打开另存为流程。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变默认格式选择流程。
    /// @warning 用户触发的低频路径：原生选择器可能阻塞 UI 线程。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 空扩展名表示先提供全部支持格式，而不是预设单一格式。
        openExportFilePicker("");
    }

    /// @brief 消费 Ctrl+Shift+S 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note 只接受 Ctrl+Shift+S，避免与普通保存 Ctrl+S 重叠。
    bool handleShortcut(MainMenuContext& context) override
    {
        (void)context;
        // 输入状态只在当前 ImGui 帧读取，不跨帧缓存。
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S) ) {
            // 快捷键复用菜单点击入口的全格式选择流程。
            openExportFilePicker("");
            return true;
        }
        return false;
    }

    /// @brief 渲染另存为动作拥有的延迟窗口，并消费快捷键请求。
    /// @warning UI 热路径：每帧检查布尔状态；文件选择器只由用户触发打开。
    /// @param context 提供本帧 DPI 缩放。
    /// @note 弹窗按格式选择、路径选择、覆盖和兼容性确认顺序分别推进。
    void renderDeferred(MainMenuContext& context) override
    {
        // 原生选择器需要先在 ImGui 弹窗中确定单一导出格式。
        renderExportFormatPickerPopup(context.dpiScale);
        // 内置文件对话框跨帧绘制并在完成时返回路径。
        renderSaveAsFileDialog(context.dpiScale);
        // 已存在目标必须先获得明确覆盖确认。
        renderOverwriteWarningPopup(context.dpiScale);
        // 覆盖确认后仍需根据格式能力展示最终兼容性选项。
        renderExportCompatibilityWarningPopup(context.dpiScale);
    }

private:
    /// @brief 直接分发谱面导出命令。
    /// @param path 目标导出路径。
    /// @param malodyExportMode MC 导出时临时使用的 Malody 模式。
    /// @param addStoreModeExtForMalodyExport 是否为 MC 导出写入上架皮肤
    /// mode_ext。
    /// @note 命令按值携带路径和临时选项，菜单不直接执行序列化。
    /// @warning 只能在覆盖与兼容性确认全部完成后调用。
    /// @pre path 必须是经过选择器格式规范化并由用户确认的 UTF-8 路径。
    void dispatchSaveBeatmapAs(const std::string&        path,
                               std::optional<MalodyMode> malodyExportMode,
                               bool addStoreModeExtForMalodyExport = false)
    {
        // 逻辑命令统一承担导出、错误反馈和会话状态处理。
        MenuUtil::dispatchCommand(Logic::CmdSaveBeatmapAs{
            .malodyExportMode               = malodyExportMode,
            .addStoreModeExtForMalodyExport = addStoreModeExtForMalodyExport,
            .path                           = path,
        });
    }

    /// @brief 请求导出当前谱面，必要时先展示格式兼容性警告。
    /// @param path 目标导出路径。
    /// @warning 低频路径：会锁定活动 Session 并检查 timing 与物件元数据。
    /// @note 无任何选项或警告的格式直接分发命令，其余先保存弹窗状态。
    /// @pre path 必须具有可识别或由导出服务支持的扩展名。
    void requestSaveBeatmapAs(std::string path)
    {
        // 扩展名按 ASCII 小写规范化，作为格式分支的唯一依据。
        const std::string ext = MenuUtil::lowerExtension(path);
        // 只有 MC 格式允许用户临时选择 Malody Key 或 Slide 模式。
        const bool showMalodyModeOption = ext == ".mc";
        // 非 MC 使用占位 Slide 值，但不会把该值传入最终命令。
        const auto malodyMode = showMalodyModeOption
                                    ? MenuUtil::currentMalodyExportMode()
                                    : MalodyMode::Slide;
        // 兼容性警告必须使用当前对话框中实际选择的 Malody 模式。
        auto warnings = MenuUtil::collectExportCompatibilityWarnings(
            path,
            showMalodyModeOption ? std::optional<MalodyMode>(malodyMode)
                                 : std::nullopt);
        // mode_ext 只对包含 Flick 或折线的 MC 导出有意义。
        const bool hasStoreModeExtEligibleElements =
            MenuUtil::shouldOfferMalodyStoreModeExtForCurrentExport(path);
        const bool showStoreModeExtOption = showMalodyModeOption &&
                                            malodyMode == MalodyMode::Slide &&
                                            hasStoreModeExtEligibleElements;
        // 原生格式或无需交互的普通导出可直接进入逻辑命令。
        if ( warnings.empty() && !showStoreModeExtOption &&
             !showMalodyModeOption ) {
            dispatchSaveBeatmapAs(path, std::nullopt);
            return;
        }

        // 路径和警告按值保存，保证弹窗跨帧期间不引用局部变量。
        m_pendingExportPath     = std::move(path);
        m_pendingExportWarnings = std::move(warnings);
        m_pendingExportFormatName =
            // 可见格式名只用于确认文案，不参与格式判断。
            (ext == ".osu") ? "osu!"
                            : ((ext == ".imd") ? "RM" : "Malody Chart");
        m_pendingExportShowStoreModeExtOption = showStoreModeExtOption;
        // 保留元素能力标志，以便用户切换 Malody 模式时重新计算选项可见性。
        m_pendingExportHasStoreModeExtEligibleElements =
            hasStoreModeExtEligibleElements;
        m_pendingExportShowMalodyModeOption = showMalodyModeOption;
        // 当前模式在弹窗中作为单选按钮权威状态。
        m_pendingExportMalodyMode = malodyMode;
        // mode_ext 默认值来自持久化用户偏好。
        m_pendingExportAddStoreModeExt =
            Config::AppConfig::instance()
                .getEditorSettings()
                .autoAddStoreModeExtForMalodyExport;
        // 一次性标志由延迟渲染转换为 ImGui 弹窗状态。
        m_showExportCompatibilityWarning = true;
    }

    /// @brief 根据用户选择路径处理覆盖确认或导出请求。
    /// @param path 文件选择器返回的 UTF-8 路径。
    /// @warning 低频用户路径：同步查询目标路径是否存在。
    /// @note 空路径和取消操作保持无副作用。
    /// @note 路径存在性只决定是否显示覆盖确认，不在此创建或写入文件。
    void handleSelectedExportPath(std::string path)
    {
        // 防止空选择进入 filesystem 或导出流程。
        if ( path.empty() ) return;
        // 已存在目标不直接覆盖，先保存路径并请求确认。
        if ( std::filesystem::exists(Config::utf8ToPath(path)) ) {
            m_pendingOverwritePath = std::move(path);
            m_showOverwriteWarning = true;
            return;
        }
        // 新路径无需覆盖确认，继续检查目标格式兼容性。
        requestSaveBeatmapAs(std::move(path));
    }

    /// @brief 打开谱面导出保存路径选择器。
    /// @param ext 期望导出的文件扩展名；为空时展示全部支持格式。
    /// @warning 用户触发的低频路径：原生选择器可能阻塞。
    /// @note 原生选择器不支持跨平台一致的多格式默认命名，空格式先显示格式弹窗。
    /// @pre ext 应为空或为 .mmm、.osu、.imd、.mc 之一。
    void openExportFilePicker(const std::string& ext)
    {
        // 引用编辑器设置以读取选择器类型和最近目录。
        auto& config = Config::AppConfig::instance().getEditorSettings();
        if ( config.filePickerStyle == Config::FilePickerStyle::Native &&
             ext.empty() ) {
            // 原生多过滤器无法可靠获知用户最终过滤器，先要求显式选择格式。
            m_showExportFormatPicker = true;
            return;
        }

        // 默认目录优先当前项目根，否则使用最近选择器路径。
        const std::string defaultPath = MenuUtil::getSaveAsPickerDefaultPath();

        // 无谱面元数据时仍提供可用的 map 文件名。
        std::string defaultName = "map" + (ext.empty() ? ".mmm" : ext);
        // 活动谱面读取受 Session 互斥锁保护。
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( session && session->getContext().currentBeatmap ) {
            // 元数据引用只在持锁作用域内用于生成默认名称。
            auto& meta =
                session->getContext().currentBeatmap->m_baseMapMetadata;
            if ( ext == ".imd" ) {
                // IMD 文件名包含标题、键数和版本，使用统一格式 helper。
                defaultName = MenuUtil::makeExportFileNameForExtension(
                    ".imd", defaultName);
            } else {
                // 其余格式沿用谱面内部名称和已选择扩展名。
                defaultName = meta.name + (ext.empty() ? ".mmm" : ext);
            }
        }

        if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
            // 原生选择器在当前调用内阻塞，显示前播放统一弹窗反馈。
            ::MMM::UI::PlayPopupOpenFeedback();
            nfdu8char_t*      outPath = nullptr;
            nfdu8filteritem_t filters[4];
            int               filterCount = 0;

            // 指定扩展名时只加入对应过滤器，空值则依次加入全部格式。
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

            // NFD 接收 UTF-8 默认目录和文件名，并返回其分配的 UTF-8 路径。
            nfdresult_t result =
                NativeFileDialog::saveFile(&outPath,
                                           filters,
                                           filterCount,
                                           defaultPath.c_str(),
                                           defaultName.c_str());

            if ( result == NFD_OKAY ) {
                // 选择成功后先复制路径进入覆盖与兼容性流程。
                handleSelectedExportPath(outPath);
                // NFD 路径必须在复制完成后用配套函数释放。
                NFD_FreePathU8(outPath);
            }
            // 取消和错误当前均保持窗口关闭；错误报告由 NFD 层处理。
            return;
        }

        // 内置文件对话框跨帧保持配置，限制单一保存目标。
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = defaultPath;
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = defaultName;
        fdConfig.flags =
            ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;

        // 过滤器字符串与指定扩展名一一对应，空值允许全部支持格式。
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

        // 记录打开前状态，仅在新打开时播放声音反馈。
        const bool wasOpen =
            ImGuiFileDialog::Instance()->IsOpened("SaveAsFilePicker");
        ImGuiFileDialog::Instance()->OpenDialog("SaveAsFilePicker",
                                                TR("ui.file.save_as").data(),
                                                filterStr.c_str(),
                                                fdConfig);
        // 重复调用已打开对话框时不重复播放反馈。
        if ( !wasOpen &&
             ImGuiFileDialog::Instance()->IsOpened("SaveAsFilePicker") ) {
            ::MMM::UI::PlayPopupOpenFeedback();
        }
    }

    /// @brief 渲染统一文件选择器并消费另存为路径。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：仅在统一文件选择器打开时绘制。
    /// @note 用户确认路径后根据当前过滤器重新生成格式对应文件名。
    void renderSaveAsFileDialog(float dpiScale)
    {
        // 居中样式作用域只影响当前内置文件对话框。
        Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
        if ( ImGuiFileDialog::Instance()->IsOpened("SaveAsFilePicker") ) {
            // 对话框打开期间固定逻辑尺寸并按公共 helper 居中。
            Utils::prepareCenteredModalWindow({ 600, 400 });
        }
        // Display 返回 true 表示用户完成确认或取消，本帧应关闭对话框。
        if ( ImGuiFileDialog::Instance()->Display(
                 "SaveAsFilePicker",
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings,
                 { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                // 先取得完整 UTF-8 路径，再根据活动过滤器规范化文件名。
                std::string filePath =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                filePath = MenuUtil::applySaveAsSelectedFormatToPath(filePath);
                // 路径按值移交覆盖检查，避免继续依赖对话框内部缓冲。
                handleSelectedExportPath(std::move(filePath));
            }
            // 确认与取消都结束本轮选择器生命周期。
            ImGuiFileDialog::Instance()->Close();
        }
    }

    /// @brief 渲染原生另存为对话框前的导出格式选择弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：每帧只检查状态并绘制固定数量按钮。
    /// @note 选择结果在弹窗关闭后才打开原生选择器，避免模态嵌套。
    void renderExportFormatPickerPopup(float dpiScale)
    {
        // 固定 ### ID 保证可见标题调整时弹窗状态稳定。
        constexpr const char* popupId =
            "选择导出格式###ExportFormatPickerWindow";
        if ( m_showExportFormatPicker ) {
            // 一次性请求转换为 ImGui 弹窗状态后立即清除。
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showExportFormatPicker = false;
            m_exportFormatPickerOpen = true;
        }
        // 自有可见性关闭时不构造选项或样式作用域。
        if ( !m_exportFormatPickerOpen ) return;

        // 扩展名为空表示本帧尚未选择格式。
        std::string selectedExtension;
        // 取消和成功选择共用关闭路径。
        bool closeWindow = false;
        {
            // 居中样式的 RAII 生命周期严格包围模态弹窗。
            Utils::CenteredModalPopupScope popupStyle(dpiScale);
            if ( popupStyle.begin(
                     popupId,
                     &m_exportFormatPickerOpen,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking,
                     ImVec2(360.0f * dpiScale, 0.0f)) ) {
                // 提示与按钮组使用分隔线形成明确选择层次。
                ImGui::TextUnformatted("选择另存为格式：");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // 所有格式按钮使用相同逻辑宽度并居中。
                const ImVec2 buttonSize(300.0f * dpiScale, 0.0f);
                if ( MenuUtil::drawCenteredButton(
                         "MusicMapMaker Beatmap (.mmm)", buttonSize) ) {
                    // 原生项目格式使用 .mmm。
                    selectedExtension = ".mmm";
                }
                if ( MenuUtil::drawCenteredButton("osu!mania Beatmap (.osu)",
                                                  buttonSize) ) {
                    // osu!mania 导出使用 .osu。
                    selectedExtension = ".osu";
                }
                if ( MenuUtil::drawCenteredButton("RM Beatmap (.imd)",
                                                  buttonSize) ) {
                    // Rhythm Master 单文件导出使用 .imd。
                    selectedExtension = ".imd";
                }
                if ( MenuUtil::drawCenteredButton("Malody Chart (.mc)",
                                                  buttonSize) ) {
                    // Malody Chart 导出使用 .mc。
                    selectedExtension = ".mc";
                }

                // 取消区与格式选项用分隔线隔开。
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if ( MenuUtil::drawCenteredButton(
                         TR("ui.common.cancel").data(),
                         ImVec2(120.0f * dpiScale, 0.0f)) ) {
                    // 取消只关闭格式选择，不打开原生文件对话框。
                    closeWindow = true;
                }

                if ( !selectedExtension.empty() ) {
                    // 成功选择同样先关闭当前模态弹窗。
                    closeWindow = true;
                }

                if ( closeWindow ) {
                    // 同步自有可见性和 ImGui 当前弹窗状态。
                    m_exportFormatPickerOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                // 与成功 popupStyle.begin 配对。
                ImGui::EndPopup();
            }
        }

        if ( !selectedExtension.empty() ) {
            // 离开 ImGui 弹窗作用域后再进入可能阻塞的原生选择器。
            openExportFilePicker(selectedExtension);
        }
    }

    /// @brief 渲染导出覆盖确认弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：只在已有目标路径待确认时绘制固定内容。
    /// @note 确认覆盖后继续执行格式兼容性检查，不直接导出。
    void renderOverwriteWarningPopup(float dpiScale)
    {
        // 固定内部 ID 避免可见文本调整影响模态状态。
        constexpr const char* popupId =
            "确认覆盖导出文件###SaveAsOverwriteWarningModal";
        if ( m_showOverwriteWarning ) {
            // 一次性请求转换为 ImGui 状态后立即清除。
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showOverwriteWarning = false;
        }

        // 没有活动弹窗时跳过样式和文本布局。
        if ( !ImGui::IsPopupOpen(popupId) ) return;

        // 居中样式作用域负责恢复临时窗口设置。
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(540.0f * dpiScale, 0.0f)) ) {
            // 明确告知操作将覆盖已有文件，要求用户再次确认。
            ImGui::TextWrapped("目标文件已经存在，是否覆盖？");
            if ( !m_pendingOverwritePath.empty() ) {
                // 完整目标路径帮助用户确认实际覆盖对象。
                ImGui::Spacing();
                ImGui::TextWrapped("目标文件：%s",
                                   m_pendingOverwritePath.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 两个按钮共享按 DPI 缩放的固定尺寸。
            const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
            if ( ::MMM::UI::FeedbackButton("确认覆盖", buttonSize) ) {
                // 路径仍按值传入下一阶段，兼容性弹窗可跨帧保存。
                requestSaveBeatmapAs(m_pendingOverwritePath);
                // 请求阶段已复制路径，覆盖临时状态可立即清空。
                m_pendingOverwritePath.clear();
                ImGui::CloseCurrentPopup();
            }
            // 取消按钮与确认按钮同行，不触发任何命令。
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                // 取消时清除路径，避免下次弹窗误用旧目标。
                m_pendingOverwritePath.clear();
                ImGui::CloseCurrentPopup();
            }

            // 与成功 popupStyle.begin 配对。
            ImGui::EndPopup();
        }
    }

    /// @brief 清空当前导出兼容性确认弹窗状态。
    /// @note 确认导出和取消都调用，保证下一次流程从默认值开始。
    /// @warning 仅清理 UI 临时状态，不修改持久化 mode_ext 偏好。
    void clearCompatibilityWarningState()
    {
        // 路径、格式名和警告文本结束流程后不再保留。
        m_pendingExportPath.clear();
        m_pendingExportFormatName.clear();
        m_pendingExportWarnings.clear();
        // 所有可见性能力标志恢复为无选项状态。
        m_pendingExportShowStoreModeExtOption          = false;
        m_pendingExportHasStoreModeExtEligibleElements = false;
        m_pendingExportShowMalodyModeOption            = false;
        // Malody 模式回到最能保留扩展物件的 Slide 默认值。
        m_pendingExportMalodyMode = MalodyMode::Slide;
        // 本次临时写入选择清除，持久化设置仍由 AppConfig 保留。
        m_pendingExportAddStoreModeExt = false;
    }

    /// @brief 渲染导出兼容性警告弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：只绘制缓存警告；切换 Malody 模式时低频重算兼容性。
    /// @note 所有兼容性选项确认后才发布最终导出命令。
    /// @note 用户关闭流程后临时警告状态不会影响下次另存为。
    void renderExportCompatibilityWarningPopup(float dpiScale)
    {
        // 固定 ### ID 维持模态窗口身份稳定。
        constexpr const char* popupId = "谱面兼容性警告###ExportWarningModal";
        if ( m_showExportCompatibilityWarning ) {
            // 一次性请求转换为 ImGui 弹窗状态后立即清除。
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showExportCompatibilityWarning = false;
        }

        // 未打开时不绘制警告列表或读取选项状态。
        if ( !ImGui::IsPopupOpen(popupId) ) return;

        // 居中作用域限制弹窗样式并负责离开时恢复。
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0f * dpiScale, 0.0f)) ) {
            if ( m_pendingExportWarnings.empty() &&
                 (m_pendingExportShowStoreModeExtOption ||
                  m_pendingExportShowMalodyModeOption) ) {
                // 无损但仍需选择 Malody 选项时使用中性提示文案。
                ImGui::Text("%s %s 前请选择导出选项：",
                            "导出",
                            m_pendingExportFormatName.c_str());
            } else {
                // 存在格式降级时明确要求确认兼容性变化。
                ImGui::Text("%s %s 前需要确认以下兼容性变化：",
                            "导出",
                            m_pendingExportFormatName.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 警告保持收集顺序，并使用统一可换行项目符号布局。
            for ( const auto& warning : m_pendingExportWarnings ) {
                MenuUtil::drawWrappedBulletText(warning);
            }
            if ( m_pendingExportShowMalodyModeOption ) {
                // 有警告时增加间距，避免模式控件紧贴最后一条文案。
                if ( !m_pendingExportWarnings.empty() ) {
                    ImGui::Spacing();
                }
                // Key 与 Slide 使用单选语义，共享一个枚举权威状态。
                ImGui::TextUnformatted("Malody 模式：");
                ImGui::SameLine();
                bool       malodyModeChanged = false;
                const bool selectedKey =
                    m_pendingExportMalodyMode == MalodyMode::Key;
                if ( ::MMM::UI::FeedbackRadioButton("Key 模式", selectedKey) ) {
                    // 只在用户激活时更新模式并登记重新计算。
                    m_pendingExportMalodyMode = MalodyMode::Key;
                    malodyModeChanged         = true;
                }
                // Slide 选项与 Key 同行显示。
                ImGui::SameLine();
                const bool selectedSlide =
                    m_pendingExportMalodyMode == MalodyMode::Slide;
                if ( ::MMM::UI::FeedbackRadioButton("Slide 模式",
                                                    selectedSlide) ) {
                    m_pendingExportMalodyMode = MalodyMode::Slide;
                    malodyModeChanged         = true;
                }

                if ( malodyModeChanged ) {
                    // 模式能力不同，必须用新模式重新生成兼容性警告。
                    m_pendingExportWarnings =
                        MenuUtil::collectExportCompatibilityWarnings(
                            m_pendingExportPath, m_pendingExportMalodyMode);
                    // mode_ext 只在 Slide 且谱面含扩展物件时重新显示。
                    m_pendingExportShowStoreModeExtOption =
                        m_pendingExportMalodyMode == MalodyMode::Slide &&
                        m_pendingExportHasStoreModeExtEligibleElements;
                }
            }
            if ( m_pendingExportShowStoreModeExtOption ) {
                // 选项与警告列表之间保持标准间距。
                if ( !m_pendingExportWarnings.empty() ) {
                    ImGui::Spacing();
                }
                // 使用局部值承接 ImGui 修改，只有变更时写回和保存配置。
                bool addStoreModeExt = m_pendingExportAddStoreModeExt;
                if ( ::MMM::UI::FeedbackCheckbox("自动添加上架皮肤 mode_ext",
                                                 &addStoreModeExt) ) {
                    // 本次导出状态立即更新。
                    m_pendingExportAddStoreModeExt = addStoreModeExt;
                    // 同时保存为下次 MC 导出的默认偏好。
                    auto& settings =
                        Config::AppConfig::instance().getEditorSettings();
                    settings.autoAddStoreModeExtForMalodyExport =
                        addStoreModeExt;
                    Config::AppConfig::instance().save();
                }
                if ( ImGui::IsItemHovered() ) {
                    // Tooltip 解释 mode_ext 会覆盖目标 MC 字段及其用途。
                    ImGui::SetTooltip(
                        "%s",
                        "会替换导出 MC 的 mode_ext，用于 EX Rhythm Master VI "
                        "皮肤上架提示。");
                }
            }

            // 目标完整路径在按钮前显示，供最终确认。
            ImGui::Spacing();
            MenuUtil::drawWrappedLabelValue("目标文件：", m_pendingExportPath);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 两个动作按钮以完整行宽计算后整体居中。
            const ImVec2 actionButtonSize(120.0f * dpiScale, 0.0f);
            const float  actionButtonRowWidth =
                actionButtonSize.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            MenuUtil::centerNextItem(actionButtonRowWidth);
            if ( ::MMM::UI::FeedbackButton("继续导出", actionButtonSize) ) {
                // 只有 MC 流程传递临时模式，其他格式使用空 optional。
                dispatchSaveBeatmapAs(
                    m_pendingExportPath,
                    m_pendingExportShowMalodyModeOption
                        ? std::optional<MalodyMode>(m_pendingExportMalodyMode)
                        : std::nullopt,
                    // mode_ext 写入同时要求选项可见且用户已勾选。
                    m_pendingExportShowStoreModeExtOption &&
                        m_pendingExportAddStoreModeExt);
                // 命令已复制所需字段，随后清空全部临时状态。
                clearCompatibilityWarningState();
                ImGui::CloseCurrentPopup();
            }
            // 取消按钮与继续按钮同行，并同样清理临时状态。
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           actionButtonSize) ) {
                clearCompatibilityWarningState();
                ImGui::CloseCurrentPopup();
            }

            // 与成功 popupStyle.begin 配对。
            ImGui::EndPopup();
        }
    }

    /// @brief 是否在下一帧打开原生另存为格式选择弹窗。
    /// @note 由空扩展名原生选择流程置位，并在渲染时消费。
    bool m_showExportFormatPicker = false;
    /// @brief 原生另存为格式选择弹窗当前是否保持打开。
    /// @note 用户取消、选择格式或窗口关闭按钮会清除。
    bool m_exportFormatPickerOpen = false;
    /// @brief 是否在下一帧打开导出兼容性警告弹窗。
    /// @note requestSaveBeatmapAs 置位，渲染阶段消费。
    bool m_showExportCompatibilityWarning = false;
    /// @brief 是否在下一帧打开覆盖确认弹窗。
    /// @note 只在目标文件已存在时置位。
    bool m_showOverwriteWarning = false;
    /// @brief 待确认覆盖的导出路径。
    /// @note 确认和取消都会在关闭弹窗前清空。
    std::string m_pendingOverwritePath;
    /// @brief 待确认导出的目标路径。
    /// @note 兼容性弹窗结束后由 clearCompatibilityWarningState 清空。
    std::string m_pendingExportPath;
    /// @brief 待确认导出的格式名称。
    /// @note 只用于可见提示，真实格式始终由路径扩展名决定。
    std::string m_pendingExportFormatName;
    /// @brief 待确认导出的兼容性警告消息。
    /// @note Malody 模式变化时按当前模式整体重建。
    std::vector<std::string> m_pendingExportWarnings;
    /// @brief 待确认导出是否显示上架 mode_ext 选项。
    /// @note 仅在 MC Slide 模式且存在 Flick 或折线时为 true。
    bool m_pendingExportShowStoreModeExtOption = false;
    /// @brief 当前谱面是否含可使用上架 mode_ext 的 Flick/折线。
    /// @note 缓存能力判断，供模式切换时恢复选项可见性。
    bool m_pendingExportHasStoreModeExtEligibleElements = false;
    /// @brief 待确认导出是否显示 Malody 模式选项。
    /// @note 目标扩展名为 .mc 时为 true。
    bool m_pendingExportShowMalodyModeOption = false;
    /// @brief 待确认 MC 导出选中的 Malody 模式。
    /// @note 弹窗单选按钮直接更新，默认沿用当前谱面模式。
    MalodyMode m_pendingExportMalodyMode{ MalodyMode::Slide };
    /// @brief 待确认导出是否写入上架 mode_ext。
    /// @note 初值来自持久化配置，用户切换时同步保存偏好。
    bool m_pendingExportAddStoreModeExt = false;
};
}  // namespace

/// @brief 创建打开另存为流程的菜单项业务处理器。
/// @return 独占所有权的另存为流程处理器。
/// @warning 处理器必须在 EditorEngine、ImGui 和文件对话框所属 UI 线程使用。
std::unique_ptr<IMainMenuItemActionHandler> createSaveBeatmapAsAction()
{
    return std::make_unique<SaveBeatmapAsAction>();
}

}  // namespace MMM::UI
