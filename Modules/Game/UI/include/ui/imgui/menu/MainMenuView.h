#pragma once

#include "common/LogicCommands.h"
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

    /// @brief 扫描当前谱面中的重叠音符。
    void performOverlapScan();

    /// @brief 打开项目目录选择器。
    void openFolderPicker();

    /// @brief 打开谱面打包路径选择器。
    void openPackFilePicker();

    /// @brief 打开谱面导出路径选择器。
    /// @param ext 期望导出的文件扩展名；为空时展示全部支持格式。
    void openExportFilePicker(const std::string& ext);

    /// @brief 打开音频导入选择器。
    void openAudioImportPicker();

    /// @brief 发布逻辑命令事件。
    /// @param cmd 需要分发给逻辑层的命令。
    void dispatchCommand(const Logic::LogicCommand& cmd);

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
    /// @brief 下一帧是否打开帮助菜单。
    bool m_openHelpMenuNextFrame = false;
    /// @brief 下一帧是否关闭文件菜单。
    bool m_closeFileMenuNextFrame = false;
    /// @brief 下一帧是否关闭编辑菜单。
    bool m_closeEditMenuNextFrame = false;
    /// @brief 下一帧是否关闭工具菜单。
    bool m_closeToolsMenuNextFrame = false;
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

    /// @brief 是否已完成启动时的自动更新检查。
    bool m_hasCheckedOnStartup = false;
    /// @brief 是否为启动时的静默检查。
    bool m_isSilentCheck = false;
    /// @brief 用户是否取消或关闭了更新弹窗。
    bool m_updatePopupCanceled = false;

    /// @brief 保存提示气泡剩余显示时间。
    float m_saveTooltipTimer = 0.0f;
    /// @brief 状态消息剩余显示时间。
    float m_statusMessageTimer = 0.0f;
    /// @brief 状态栏显示的临时消息。
    std::string m_statusMessage;

    /// @brief 更新检查器实例。
    std::unique_ptr<MMM::Network::UpdateChecker> m_updateChecker;
};

}  // namespace MMM::UI
