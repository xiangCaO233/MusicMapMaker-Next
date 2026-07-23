#pragma once

#include "common/LogicCommands.h"
#include "ui/IUIView.h"
#include <array>
#include <glm/glm.hpp>
#include <optional>
#include <string>

namespace MMM::Config
{
struct NoteColorPaletteScheme;
}

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
    /// @brief 当前物件调色盘选择来源。
    enum class NotePaletteSelectionKind {
        InheritSoftwareDefault,  ///< 继承软件默认物件调色方案。
        SkinDefault,             ///< 使用当前皮肤默认色盘。
        Custom                   ///< 使用用户保存的自定义色盘。
    };

    Logic::EditTool m_currentTool = Logic::EditTool::Move;
    /// @brief 进入轨道布局模式前使用的工具，用于再次点击按钮时恢复。
    Logic::EditTool m_toolBeforeTrackLayout = Logic::EditTool::Move;
    bool            m_showDivisorPopup      = false;
    float           m_lastBtnY              = 0.0f;
    float           m_popupWidth            = 160.0f;
    float           m_popupHeight           = 120.0f;
    bool            m_showKeyPopup          = false;
    float           m_lastKeyBtnY           = 0.0f;
    float           m_keyPopupWidth         = 160.0f;
    float           m_keyPopupHeight        = 120.0f;
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
    /// @brief 当前调色盘选择来源，用于区分内置项和可管理的自定义方案。
    NotePaletteSelectionKind m_activePaletteSelection{
        NotePaletteSelectionKind::SkinDefault
    };
    /// @brief 等待删除确认的自定义调色盘方案索引。
    std::optional<std::size_t> m_pendingDeletePaletteSchemeIndex;
    /// @brief 当前方案名称校验错误的翻译键；为空表示无错误。
    std::string m_paletteSchemeErrorKey;
    /// @brief 当前配色方案导出结果的翻译键；为空表示尚无结果。
    std::string m_paletteExportStatusKey;
    /// @brief 最近一次配色方案导出是否成功。
    bool m_paletteExportSucceeded{ false };
    /// @brief 当前调色盘方案名称编辑缓冲区。
    std::array<char, 96> m_paletteSchemeNameBuffer{};
    /// @brief 上次已应用项目调色方案的项目与方案组合键。
    std::string m_lastAppliedProjectPaletteKey;

    /**
     * @brief 绘制工具按钮
     * @param icon 图标字符串
     * @param tool 对应的工具类型
     * @param tooltip 悬停提示
     * @param width 按钮宽度
     * @param height 按钮高度
     * @param shortLabel 图标下方显示的短标签
     * @param showLabel 是否显示短标签
     * @param sourceManager 当前 UI 管理器，用于恢复工具切换前的画布焦点
     */
    void drawToolButton(const char* icon, Logic::EditTool tool,
                        const char* tooltip, float width, float height,
                        const char* shortLabel, bool showLabel,
                        UIManager* sourceManager);

    /// @brief 绘制可再次点击退出的轨道布局调整按钮。
    /// @param width 按钮宽度。
    /// @param height 按钮高度。
    /// @param showLabel 是否显示短标签。
    void drawTrackLayoutButton(float width, float height, bool showLabel);

    /// @brief 绘制带可选短标签的图标按钮。
    /// @param icon 图标字符串。
    /// @param id 不显示的 ImGui ID。
    /// @param shortLabel 图标下方显示的短标签。
    /// @param width 按钮宽度。
    /// @param height 按钮高度。
    /// @param showLabel 是否显示短标签。
    /// @return 被点击时返回 true。
    bool drawIconButton(const char* icon, const char* id,
                        const char* shortLabel, float width, float height,
                        bool showLabel) const;

    /// @brief 从当前皮肤初始化调色盘默认颜色。
    void initializeColorPalette();

    /// @brief 将调色盘重置为当前皮肤默认颜色。
    void loadSkinDefaultPalette();

    /// @brief 载入软件默认物件调色方案。
    void loadSoftwareDefaultPalette();

    /// @brief 按方案名称加载软件级调色盘方案。
    /// @param schemeName 方案名称或皮肤默认方案标识。
    /// @return 成功应用方案时返回 true。
    bool loadPaletteSchemeByName(const std::string& schemeName);

    /// @brief 项目切换或默认方案变化后应用项目调色方案偏好。
    void applyProjectPalettePreference();

    /// @brief 将当前调色盘颜色发送为画笔自定义颜色。
    void pushPaletteToBrush();

    /// @brief 将当前调色盘颜色应用到选中物件。
    void pushPaletteToSelection();

    /// @brief 加载一个已保存的调色盘方案。
    /// @param schemeIndex 方案索引。
    void loadPaletteScheme(std::size_t schemeIndex);

    /// @brief 判断当前选中的调色盘方案是否允许保存、重命名或删除。
    /// @return 当前选择为有效自定义方案时返回 true。
    bool canManageActivePaletteScheme() const;

    /// @brief 检查指定名称是否与已有自定义调色盘方案冲突。
    /// @param name 待检查的方案名称。
    /// @param ignoredIndex 允许同名的当前方案索引；为空时不忽略任何方案。
    /// @return 存在同名自定义方案时返回 true。
    bool hasPaletteSchemeNameConflict(
        const std::string& name, std::optional<std::size_t> ignoredIndex) const;

    /// @brief 校验方案名称是否可用于保存或重命名。
    /// @param name 待校验的方案名称。
    /// @param ignoredIndex 允许同名的当前方案索引；为空时不忽略任何方案。
    /// @return 名称可用时返回 true；失败时写入错误提示键。
    bool validatePaletteSchemeNameForSave(
        const std::string& name, std::optional<std::size_t> ignoredIndex);

    /// @brief 保存当前调色盘方案。
    /// @param createNew 是否创建新方案。
    void savePaletteScheme(bool createNew);

    /// @brief 打开当前物件配色方案的导出文件选择器。
    /// @warning 用户触发的低频路径：原生文件选择器可能阻塞。
    void openPaletteExportFilePicker();

    /// @brief 绘制并消费统一风格的配色方案导出文件选择器。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI 热路径：仅在统一文件选择器打开时绘制。
    void renderPaletteExportFileDialog(float dpiScale);

    /// @brief 将当前物件配色方案导出到指定 UTF-8 路径。
    /// @param path 用户选择的目标路径。
    /// @warning 用户触发的低频路径：允许执行文件系统写入。
    void exportCurrentPaletteToPath(const std::string& path);

    /// @brief 构造当前弹窗中实际显示的物件配色方案。
    /// @return 包含当前名称和六个颜色槽的方案。
    Config::NoteColorPaletteScheme buildCurrentPaletteScheme() const;

    /// @brief 重命名当前选中的调色盘方案。
    void renamePaletteScheme();

    /// @brief 删除指定自定义调色盘方案。
    /// @param schemeIndex 要删除的自定义方案索引。
    void deletePaletteScheme(std::size_t schemeIndex);

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
