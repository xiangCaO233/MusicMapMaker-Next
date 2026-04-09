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

    void update(UIManager* sourceManager) override;

private:
    Logic::EditTool m_currentTool      = Logic::EditTool::Move;
    bool            m_showDivisorPopup = false;
    float           m_lastBtnY         = 0.0f;

    /**
     * @brief 绘制工具按钮
     * @param icon 图标字符串
     * @param tool 对应的工具类型
     * @param tooltip 悬停提示
     * @param width 按钮宽度
     */
    void drawToolButton(const char* icon, Logic::EditTool tool,
                        const char* tooltip, float width);
};

}  // namespace MMM::UI
