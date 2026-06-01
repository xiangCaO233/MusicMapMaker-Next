#pragma once

#include "common/LogicCommands.h"
#include "ui/IUIView.h"
#include <string>

namespace MMM::UI
{

/**
 * @brief 编辑工具栏视图
 * 提供移动、选取等核心编辑工具的快速切换。
 * 停靠在主画布与预览区之间，采用 Flat 风格。
 */
class ToolbarView : public IUIView
{
public:
    ToolbarView(const std::string& name);
    ~ToolbarView() override = default;

    /// @brief 绘制工具栏主界面。
    /// @param sourceManager 当前 UI 管理器。
    void update(UIManager* sourceManager) override;

private:
    Logic::EditTool m_currentTool      = Logic::EditTool::Move;
    bool            m_showDivisorPopup = false;
    float           m_lastBtnY         = 0.0f;
    float           m_popupWidth       = 160.0f;
    float           m_popupHeight      = 120.0f;
    bool            m_showKeyPopup     = false;
    float           m_lastKeyBtnY      = 0.0f;
    float           m_keyPopupWidth    = 160.0f;
    float           m_keyPopupHeight   = 120.0f;
    /// @brief 是否显示主音轨倍速详细调整弹窗。
    bool m_showSpeedPopup{ false };
    /// @brief 上一帧倍速按钮的屏幕 Y 坐标，用于定位弹窗。
    float m_lastSpeedBtnY{ 0.0f };
    /// @brief 主音轨倍速弹窗上一帧宽度，用于防止视口越界。
    float m_speedPopupWidth{ 160.0f };
    /// @brief 主音轨倍速弹窗上一帧高度，用于防止视口越界。
    float m_speedPopupHeight{ 120.0f };

    /**
     * @brief 绘制工具按钮
     * @param icon 图标字符串
     * @param tool 对应的工具类型
     * @param tooltip 悬停提示
     * @param width 按钮宽度
     */
    void drawToolButton(const char* icon, Logic::EditTool tool,
                        const char* tooltip, float width);

    /**
     * @brief 绘制左侧偏移的提示框
     * @param text 提示文本
     */
    void drawTooltip(const char* text);
};

}  // namespace MMM::UI
