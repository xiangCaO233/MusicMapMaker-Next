#pragma once

#include "common/LogicCommands.h"
#include "mmm/project/PackageFileTypes.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MMM::Network
{
class UpdateChecker;
}

namespace MMM::UI
{
class UIManager;

/// @brief ImGui 顶部主菜单视图，负责菜单渲染、快捷键、弹窗和编辑辅助窗口。
class MainMenuView
{
public:
    /// @brief 构造主菜单视图。
    MainMenuView();

    /// @brief 默认移动构造主菜单视图。
    MainMenuView(MainMenuView&&) = default;

    /// @brief 禁止拷贝构造，避免复制更新检查器和窗口状态。
    MainMenuView(const MainMenuView&) = delete;

    /// @brief 默认移动赋值主菜单视图。
    MainMenuView& operator=(MainMenuView&&) = default;

    /// @brief 禁止拷贝赋值，避免复制更新检查器和窗口状态。
    MainMenuView& operator=(const MainMenuView&) = delete;

    /// @brief 销毁主菜单视图。
    ~MainMenuView();

    /// @brief 更新主菜单计时器和弹窗状态。
    /// @param sourceManager 当前 UI 管理器。
    void update(UIManager* sourceManager);

    /// @brief 渲染顶部主菜单。
    /// @param sourceManager 当前 UI 管理器。
    void renderMenus(UIManager* sourceManager);

    /// @brief 渲染重叠检测结果窗口。
    void renderOverlapCheckWindow();

    /// @brief 渲染谱面元数据编辑窗口。
    void renderMetadataEditorWindow();

    /// @brief 渲染选中音符元数据编辑窗口。
    void renderNoteMetadataEditorWindow();

    /// @brief 渲染底部提示文本占位区域。
    void renderInfoText();

    /// @brief 请求导出当前谱面，必要时先展示格式兼容性警告。
    /// @param path 目标导出路径。
    void requestSaveBeatmapAs(std::string path);

    /// @brief 按统一导出文件选择器当前格式规范化保存路径。
    /// @param path 文件选择器返回的路径。
    /// @return 应实际导出的目标路径。
    std::string applySaveAsSelectedFormatToPath(const std::string& path) const;

    /// @brief 按当前打包目标格式规范化输出包路径。
    /// @param path 文件选择器返回的输出路径。
    /// @return 补齐目标打包扩展名后的输出路径。
    std::string applyPackSelectedFormatToPath(const std::string& path) const;

    /// @brief 请求打包当前已选择的项目文件。
    /// @param path 输出包路径。
    void requestPackBeatmapTo(std::string path);

    /// @brief 处理主菜单相关的全局快捷键。
    /// @param sourceManager 当前 UI 管理器。
    void handleHotkeys(UIManager* sourceManager);

    /// @brief 获取状态信息 (用于状态栏显示)
    /// @return 状态消息仍在显示时返回消息文本，否则返回空字符串。
    std::string getStatusMessage() const
    {
        return m_statusMessageTimer > 0.0f ? m_statusMessage : "";
    }

    /// @brief 获取重叠检测工具窗口是否打开。
    /// @return 重叠检测工具窗口是否打开。
    bool isOverlapCheckWindowOpen() const { return m_showOverlapCheckWindow; }

    /// @brief 设置重叠检测工具窗口打开状态。
    /// @param open 是否打开窗口。
    void setOverlapCheckWindowOpen(bool open)
    {
        m_showOverlapCheckWindow = open;
    }

    /// @brief 获取谱面额外元数据编辑窗口是否打开。
    /// @return 谱面额外元数据编辑窗口是否打开。
    bool isMetadataEditorWindowOpen() const
    {
        return m_showMetadataEditorWindow;
    }

    /// @brief 设置谱面额外元数据编辑窗口打开状态。
    /// @param open 是否打开窗口。
    void setMetadataEditorWindowOpen(bool open)
    {
        m_showMetadataEditorWindow = open;
    }

    /// @brief 获取音符元数据编辑窗口是否打开。
    /// @return 音符元数据编辑窗口是否打开。
    bool isNoteMetadataEditorWindowOpen() const
    {
        return m_showNoteMetadataEditorWindow;
    }

    /// @brief 设置音符元数据编辑窗口打开状态。
    /// @param open 是否打开窗口。
    void setNoteMetadataEditorWindowOpen(bool open)
    {
        m_showNoteMetadataEditorWindow = open;
    }

private:
    /// @brief 单条重叠检测结果。
    struct OverlapResult {
        /// @brief 是否为确定重叠；false 表示疑似重叠。
        bool is_definite;
        /// @brief 重叠发生的时间戳。
        double timestamp;
        /// @brief 重叠发生的轨道编号。
        uint32_t track;
        /// @brief 第一枚音符的描述文本。
        std::string note1_desc;
        /// @brief 第二枚音符的描述文本。
        std::string note2_desc;
    };

    /// @brief 打包候选文件条目。
    struct PackageCandidateFile {
        /// @brief 项目相对路径，使用 UTF-8 编码和通用分隔符。
        std::string relativePath;

        /// @brief 用户界面显示的资源分类名称。
        std::string typeLabel;

        /// @brief 是否将该文件写入最终包。
        bool selected{ true };
    };

    /// @brief 打包转换到 MC 前临时编辑的谱面元数据。
    struct PackageBeatmapMetadataEdit {
        /// @brief 项目相对谱面路径，使用 UTF-8 编码和通用分隔符。
        std::string relativePath;

        /// @brief 当前编辑中的基础谱面元数据。
        BaseMapMeta baseMeta;

        /// @brief MC 标题输入缓存。
        std::array<char, 192> titleBuffer{};

        /// @brief MC 原标题输入缓存。
        std::array<char, 192> titleUnicodeBuffer{};

        /// @brief MC 艺术家输入缓存。
        std::array<char, 192> artistBuffer{};

        /// @brief MC 原艺术家输入缓存。
        std::array<char, 192> artistUnicodeBuffer{};

        /// @brief MC 谱师输入缓存。
        std::array<char, 192> creatorBuffer{};

        /// @brief MC 难度名输入缓存。
        std::array<char, 192> versionBuffer{};
    };

    /// @brief 扫描当前谱面中的重叠音符。
    void performOverlapScan();

    /// @brief 打开项目目录选择器。
    void openFolderPicker();

    /// @brief 打开谱面打包路径选择器。
    void openPackFilePicker();

    /// @brief 打开打包输出路径选择器。
    void openPackageOutputFilePicker();

    /// @brief 打开谱面导出路径选择器。
    /// @param ext 期望导出的文件扩展名；为空时展示全部支持格式。
    void openExportFilePicker(const std::string& ext);

    /// @brief 根据导出格式生成推荐文件名。
    /// @param extension 目标扩展名。
    /// @param currentFileName 当前文件名，用于保留非 RM/IMD 格式的主文件名。
    /// @return 推荐文件名。
    std::string makeExportFileNameForExtension(
        const std::string& extension, const std::string& currentFileName) const;

    /// @brief 打开音频导入选择器。
    void openAudioImportPicker();

    /// @brief 发布逻辑命令事件。
    /// @param cmd 需要分发给逻辑层的命令。
    void dispatchCommand(const Logic::LogicCommand& cmd);

    /// @brief 直接分发谱面导出命令并显示保存提示。
    /// @param path 目标导出路径。
    void dispatchSaveBeatmapAs(const std::string& path);

    /// @brief 收集当前谱面导出到指定格式时需要提醒用户的兼容性问题。
    /// @param path 目标导出路径。
    /// @return 需要展示的警告消息列表。
    std::vector<std::string> collectExportCompatibilityWarnings(
        const std::string& path) const;

    /// @brief 渲染导出兼容性警告弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderExportCompatibilityWarningPopup(float dpiScale);

    /// @brief 渲染原生另存为对话框前的导出格式选择弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderExportFormatPickerPopup(float dpiScale);

    /// @brief 渲染打包目标格式选择弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderPackageFormatPickerPopup(float dpiScale);

    /// @brief 渲染打包文件复选列表窗口。
    /// @param dpiScale 当前窗口内容缩放。
    void renderPackageFileSelectionWindow(float dpiScale);

    /// @brief 渲染打包前补充 Malody 元数据的窗口。
    /// @param dpiScale 当前窗口内容缩放。
    void renderPackageMalodyMetadataWindow(float dpiScale);

    /// @brief 打开谱面倍速制作弹窗。
    void openBeatmapSpeedExportPopup();

    /// @brief 渲染谱面倍速制作弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderBeatmapSpeedExportPopup(float dpiScale);

    /// @brief 启动谱面倍速制作后台任务。
    void startBeatmapSpeedExport();

    /// @brief 消费谱面倍速制作后台任务消息。
    void consumeBeatmapSpeedExportQueues();

    /// @brief 按当前目标打包格式重建候选文件列表。
    void rebuildPackageCandidateFiles();

    /// @brief 为选中的 IMD 谱面准备打包到 MC 前的元数据补充项。
    /// @param selectedRelativePaths 当前已选的项目相对路径列表。
    /// @return 需要展示补充窗口时返回 true。
    bool preparePackageMalodyMetadataEdits(
        const std::vector<std::string>& selectedRelativePaths);

    /// @brief 从补充窗口缓存收集打包元数据覆盖项。
    /// @return 元数据覆盖项列表。
    std::vector<Logic::PackageBeatmapMetadataOverride>
    collectPackageMetadataOverridesFromEdits();

    /// @brief 收集当前已勾选的项目相对文件路径。
    /// @return 已勾选的项目相对文件路径列表。
    std::vector<std::string> collectSelectedPackageRelativePaths() const;

    /// @brief 生成当前打包目标格式的默认输出文件名。
    /// @return 默认输出文件名。
    std::string makePackageDefaultFileName() const;

    /// @brief 渲染帮助菜单。
    /// @param sourceManager 当前 UI 管理器。
    void renderHelpMenu(UIManager* sourceManager);

    /// @brief 渲染关于弹窗。
    void renderAboutPopup();

    /// @brief 渲染更新下载弹窗。
    void renderUpdatePopup();

    /// @brief 渲染更新检查中弹窗。
    void renderUpdateCheckingPopup();

    /// @brief 渲染更新下载成功弹窗。
    void renderUpdateSuccessPopup();

    /// @brief 渲染保存提示气泡。
    void renderSaveTooltip();

    /// @brief 启动更新检查。
    void startUpdateCheck();

    /// @brief 下一帧是否打开文件菜单。
    bool m_openFileMenuNextFrame = false;
    /// @brief 下一帧是否打开编辑菜单。
    bool m_openEditMenuNextFrame = false;
    /// @brief 下一帧是否打开工具菜单。
    bool m_openToolsMenuNextFrame = false;
    /// @brief 下一帧是否打开查看菜单。
    bool m_openViewMenuNextFrame = false;
    /// @brief 下一帧是否打开帮助菜单。
    bool m_openHelpMenuNextFrame = false;
    /// @brief 下一帧是否关闭文件菜单。
    bool m_closeFileMenuNextFrame = false;
    /// @brief 下一帧是否关闭编辑菜单。
    bool m_closeEditMenuNextFrame = false;
    /// @brief 下一帧是否关闭工具菜单。
    bool m_closeToolsMenuNextFrame = false;
    /// @brief 下一帧是否关闭查看菜单。
    bool m_closeViewMenuNextFrame = false;
    /// @brief 下一帧是否关闭帮助菜单。
    bool m_closeHelpMenuNextFrame = false;

    /// @brief 是否显示重叠检测窗口。
    bool m_showOverlapCheckWindow = false;
    /// @brief 是否显示谱面元数据编辑窗口。
    bool m_showMetadataEditorWindow = false;
    /// @brief 是否显示音符元数据编辑窗口。
    bool m_showNoteMetadataEditorWindow = false;
    /// @brief 当前重叠检测结果是否已生成。
    bool m_hasOverlapScan = false;
    /// @brief 当前缓存的重叠检测结果。
    std::vector<OverlapResult> m_overlapResults;

    /// @brief 是否显示关于弹窗。
    bool m_showAboutPopup = false;
    /// @brief 是否显示更新下载弹窗。
    bool m_showUpdatePopup = false;
    /// @brief 是否显示更新检查中弹窗。
    bool m_showCheckingPopup = false;
    /// @brief 是否显示更新成功弹窗。
    bool m_showUpdateSuccessPopup = false;
    /// @brief 是否在下一帧打开导出兼容性警告弹窗。
    bool m_showExportCompatibilityWarning = false;
    /// @brief 是否在下一帧打开原生另存为格式选择弹窗。
    bool m_showExportFormatPicker = false;
    /// @brief 是否在下一帧打开打包格式选择弹窗。
    bool m_showPackageFormatPicker = false;
    /// @brief 是否显示打包文件复选列表窗口。
    bool m_showPackageFileSelectionWindow = false;
    /// @brief 是否显示打包前 Malody 元数据补充窗口。
    bool m_showPackageMalodyMetadataWindow = false;
    /// @brief 是否显示谱面倍速制作弹窗。
    bool m_showBeatmapSpeedExportPopup = false;
    /// @brief 谱面倍速制作后台任务是否运行中。
    bool m_speedExportRunning = false;
    /// @brief 谱面倍速制作倍率。
    float m_speedExportFactor = 1.2f;
    /// @brief 谱面倍速音频是否保留原音高。
    bool m_speedExportPreservePitch = true;
    /// @brief 谱面倍速音频输出格式索引；0 表示跟随源音频。
    int m_speedExportAudioFormatIndex = 0;
    /// @brief 输出名称是否已被用户手动编辑。
    bool m_speedExportNameEdited = false;
    /// @brief 谱面倍速制作输出名称输入缓存。
    std::array<char, 192> m_speedExportNameBuffer{};
    /// @brief 当前自动生成的输出名称，用于判断是否跟随倍率刷新。
    std::string m_speedExportAutoName;
    /// @brief 谱面倍速制作进度。
    float m_speedExportProgress = 0.0f;
    /// @brief 谱面倍速制作状态文本。
    std::string m_speedExportStatus;

    /// @brief 是否已完成启动时的自动更新检查。
    bool m_hasCheckedOnStartup = false;
    /// @brief 是否为启动时的静默检查。
    bool m_isSilentCheck = false;
    /// @brief 用户是否取消或关闭了更新弹窗。
    bool m_updatePopupCanceled = false;

    /// @brief 保存提示气泡剩余显示时间。
    float m_saveTooltipTimer = 0.0f;
    /// @brief 保存提示气泡是否为成功状态。
    bool m_saveTooltipSuccess = true;
    /// @brief 保存提示气泡显示文本。
    std::string m_saveTooltipMessage;
    /// @brief 状态消息剩余显示时间。
    float m_statusMessageTimer = 0.0f;
    /// @brief 状态栏显示的临时消息。
    std::string m_statusMessage;
    /// @brief 待确认导出的目标路径。
    std::string m_pendingExportPath;
    /// @brief 待确认导出的格式名称。
    std::string m_pendingExportFormatName;
    /// @brief 待确认导出的兼容性警告消息。
    std::vector<std::string> m_pendingExportWarnings;

    /// @brief 当前打包目标格式。
    PackageFileType m_selectedPackageFileType{ PackageFileType::Osz };
    /// @brief 当前打包格式下可选择的候选文件。
    std::vector<PackageCandidateFile> m_packageCandidateFiles;
    /// @brief 等待输出路径确认的已选项目相对文件路径。
    std::vector<std::string> m_pendingPackageRelativePaths;
    /// @brief 等待打包命令使用的元数据覆盖项。
    std::vector<Logic::PackageBeatmapMetadataOverride>
        m_pendingPackageMetadataOverrides;
    /// @brief 打包前正在编辑的 Malody 元数据项。
    std::vector<PackageBeatmapMetadataEdit> m_packageMalodyMetadataEdits;
    /// @brief 是否将打包转换产物保存回项目目录。
    bool m_saveConvertedPackageBeatmapsToProject{ false };
    /// @brief MCZ 打包时是否额外在包内写入旧皮肤兼容的 IMD 谱面。
    bool m_includeLegacyImdPackageBeatmaps{ false };

    /// @brief 更新检查器实例。
    std::unique_ptr<MMM::Network::UpdateChecker> m_updateChecker;
};

}  // namespace MMM::UI
