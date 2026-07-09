#pragma once

#include "ui/imgui/menu/MainMenuInterfaces.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace MMM::UI
{
class UIManager;

/// @brief ImGui 顶部主菜单视图，负责注册菜单遍历、快捷键分发和公共反馈。
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

    /// @brief 渲染由主菜单触发但必须位于菜单栏窗口外的弹窗和辅助窗口。
    /// @param sourceManager 当前 UI 管理器，用于消费菜单延迟动作。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：每帧执行；只允许消费已置位菜单动作并渲染可见弹窗，
    /// 文件选择器等阻塞操作只能来自用户明确点击。
    void renderDeferredPopups(UIManager* sourceManager, float dpiScale);

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

    /// @brief 显示状态栏临时消息。
    /// @param message 状态消息文本。
    /// @param durationSeconds 显示时长，单位秒。
    void showStatusMessage(std::string message, float durationSeconds);

private:
    /// @brief 请求下一帧打开指定一级菜单。
    /// @param id 一级菜单标识。
    void requestMenuOpen(MainMenuId id);

    /// @brief 请求下一帧关闭指定一级菜单。
    /// @param id 一级菜单标识。
    void requestMenuClose(MainMenuId id);

    /// @brief 消费指定一级菜单的打开请求。
    /// @param id 一级菜单标识。
    /// @return 本帧存在打开请求时返回 true。
    bool consumeMenuOpenRequest(MainMenuId id);

    /// @brief 消费指定一级菜单的关闭请求。
    /// @param id 一级菜单标识。
    /// @return 本帧存在关闭请求时返回 true。
    bool consumeMenuCloseRequest(MainMenuId id);

    /// @brief 渲染首次启动 PGO 性能数据上传授权弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderPgoUploadConsentWindow(float dpiScale);

    /// @brief 渲染保存提示气泡。
    void renderSaveTooltip();

    /// @brief 已注册的一级主菜单绘制接口。
    std::vector<std::unique_ptr<IMainMenu>> m_registeredMenus;
    /// @brief 下一帧需要打开的一级菜单标志。
    std::array<bool, MAIN_MENU_ID_COUNT> m_openMenuNextFrame{};
    /// @brief 下一帧需要关闭的一级菜单标志。
    std::array<bool, MAIN_MENU_ID_COUNT> m_closeMenuNextFrame{};
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
};

}  // namespace MMM::UI
