#pragma once

#include "common/LogicCommands.h"
#include "ui/IUIView.h"
#include <array>
#include <glm/glm.hpp>
#include <optional>
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
    /// @brief 是否显示音符颜色调色盘弹窗。
    bool m_showColorPopup{ false };
    /// @brief 上一帧颜色按钮的屏幕 Y 坐标，用于定位弹窗。
    float m_lastColorBtnY{ 0.0f };
    /// @brief 音符颜色弹窗上一帧宽度，用于防止视口越界。
    float m_colorPopupWidth{ 360.0f };
    /// @brief 音符颜色弹窗上一帧高度，用于防止视口越界。
    float m_colorPopupHeight{ 360.0f };
    /// @brief 当前调色盘正在编辑的颜色槽位。
    Logic::NoteColorSlot m_activeColorSlot{ Logic::NoteColorSlot::Tap };
    /// @brief 当前调色盘缓存颜色。
    std::array<glm::vec4, Logic::NOTE_COLOR_SLOT_COUNT> m_paletteColors{};
    /// @brief 调色盘颜色是否已从皮肤初始化。
    bool m_colorPaletteInitialized{ false };
    /// @brief 颜色选择器是否使用 HSV 显示模式。
    bool m_colorPickerUseHsv{ false };
    /// @brief 当前活动颜色的 HEX 输入缓冲区。
    std::array<char, 16> m_colorHexBuffer{};
    /// @brief HEX 输入缓冲区当前对应的颜色槽位。
    Logic::NoteColorSlot m_colorHexBufferSlot{ Logic::NoteColorSlot::Tap };
    /// @brief HEX 输入框是否正处于编辑状态。
    bool m_colorHexInputActive{ false };
    /// @brief 当前选中的持久化调色盘方案索引；-1 表示未使用保存方案。
    int m_activePaletteSchemeIndex{ -1 };
    /// @brief 当前调色盘方案名称编辑缓冲区。
    std::array<char, 96> m_paletteSchemeNameBuffer{};

    /**
     * @brief 绘制工具按钮
     * @param icon 图标字符串
     * @param tool 对应的工具类型
     * @param tooltip 悬停提示
     * @param width 按钮宽度
     */
    void drawToolButton(const char* icon, Logic::EditTool tool,
                        const char* tooltip, float width);

    /// @brief 从当前皮肤初始化调色盘默认颜色。
    void initializeColorPalette();

    /// @brief 将当前调色盘颜色发送为画笔自定义颜色。
    void pushPaletteToBrush();

    /// @brief 将当前调色盘颜色应用到选中物件。
    void pushPaletteToSelection();

    /// @brief 加载一个已保存的调色盘方案。
    /// @param schemeIndex 方案索引。
    void loadPaletteScheme(std::size_t schemeIndex);

    /// @brief 保存当前调色盘方案。
    /// @param createNew 是否创建新方案。
    void savePaletteScheme(bool createNew);

    /// @brief 重命名当前选中的调色盘方案。
    void renamePaletteScheme();

    /// @brief 将方案名写入编辑缓冲区。
    /// @param name 方案名。
    void setPaletteSchemeNameBuffer(const std::string& name);

    /// @brief 将颜色写入 HEX 输入缓冲区。
    /// @param slot 颜色槽位。
    /// @param color 当前颜色。
    void setColorHexBuffer(Logic::NoteColorSlot slot, glm::vec4 color);

    /// @brief 读取当前方案名输入框内容。
    /// @return 合法方案名。
    std::string currentPaletteSchemeName() const;

    /// @brief 将颜色命令发送到逻辑线程。
    /// @param slot 颜色槽位。
    /// @param color 自定义颜色；空值表示清除并使用皮肤默认色。
    /// @param applyToSelection 是否同时应用到当前选中物件。
    void pushColorCommands(Logic::NoteColorSlot     slot,
                           std::optional<glm::vec4> color,
                           bool                     applyToSelection);

    /// @brief 绘制音符颜色调色盘弹窗。
    /// @param dpiScale 当前 DPI 缩放。
    void renderColorPalettePopup(float dpiScale);

    /**
     * @brief 绘制左侧偏移的提示框
     * @param text 提示文本
     */
    void drawTooltip(const char* text);
};

}  // namespace MMM::UI
