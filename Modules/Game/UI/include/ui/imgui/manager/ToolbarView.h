#pragma once

#include "common/EditTool.h"
#include "ui/IUIView.h"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace MMM::Config
{
struct ColorPaletteScheme;
struct EditorConfig;
}  // namespace MMM::Config

namespace MMM::Logic
{
enum class NoteColorSlot;
}  // namespace MMM::Logic

namespace MMM::UI
{
class IEditorApplicationService;
class ICanvasView;

/**
 * @brief 编辑工具栏视图
 * 提供移动、选取等核心编辑工具的快速切换。
 * 停靠在主画布与预览区之间，采用 Flat 风格。
 */
class ToolbarView : public IUIView
{
public:
    /// @brief 建立工具栏及其跨帧的编辑状态。
    ToolbarView(const std::string& name);
    /// @brief 回收工具栏独占的音效草稿和调色盘会话。
    ~ToolbarView() override;

    /// @brief 绘制工具栏主界面。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI 热路径：每帧执行，禁止引入阻塞或文件系统操作。
    void update(UIManager* sourceManager) override;

    /// @brief 每份布局面板独占尺寸、控件选择和连续操作状态。
    struct LayoutPanelState {
        /// @brief 布局组件弹层上一帧宽度。
        float m_layoutPopupWidth{ 260.0f };
        /// @brief 布局组件弹层上一帧高度。
        float m_layoutPopupHeight{ 100.0f };
        /// @brief 布局组件颜色是否有尚未写入配置文件的修改。
        bool m_layoutComponentColorDirty{ false };
        /// @brief 背景电平图折叠菜单是否有尚未写入配置文件的连续调整。
        bool m_layoutSpectrumConfigDirty{ false };
        /// @brief 物件与背景折叠菜单是否有尚未写入配置文件的连续调整。
        bool m_layoutVisualConfigDirty{ false };
        /// @brief 布局面板当前编辑的尺寸类别，只属于本地界面状态。
        int m_noteSizePart{ 0 };
        /// @brief 上一帧布局组件颜色选择器是否打开。
        bool m_layoutComponentColorPickerOpen{ false };
    };

    /// @brief 对比视图在本帧把独立配置、锚点和返回标记借给布局面板。
    struct ComparisonLayoutPanel {
        ICanvasView&          canvas;        ///< 当前对比画布。
        Config::EditorConfig& config;        ///< 临时编辑配置，不写入应用。
        LayoutPanelState&     state;         ///< 对比面板独立控件状态。
        const char*           windowName;    ///< 包含每个视口的稳定 ID。
        glm::vec2             railPosition;  ///< 右侧工具条左上角。
        glm::vec2             railSize;      ///< 右侧工具条实际尺寸。
        bool                  mobile;        ///< 移动密度和触控布局。
        bool&                 changed;       ///< 发生修改时由画布发布快照。
    };

    /// @brief 在对比视图右侧工具按钮旁绘制完整布局面板。
    /// @warning 每帧只在打开时绘制；对比配置不会进入全局保存流程。
    void renderComparisonLayout(float dpiScale, ComparisonLayoutPanel& panel);

    /// @brief 绘制带可选短标签的图标按钮。
    /// @param icon 图标字符串。
    /// @param id 不显示的 ImGui ID。
    /// @param shortLabel 图标下方显示的短标签。
    /// @param width 按钮宽度。
    /// @param height 按钮高度。
    /// @param showLabel 是否显示短标签。
    /// @return 被点击时返回 true。
    static bool drawIconButton(const char* icon, const char* id,
                               const char* shortLabel, float width,
                               float height, bool showLabel);

    /// @brief 在皮肤切换完成后刷新仍依赖皮肤的当前调色盘。
    void refreshPaletteAfterSkinChange();

    /// @brief 绑定编辑器应用服务观察指针。
    /// @param service 服务由 UIManager 持有，生命周期覆盖 ToolbarView。
    void setEditorApplicationService(IEditorApplicationService* service);

    /// @brief 移动工作台可复用的工具内容，不包含桌面按钮布局。
    enum class MobileTool { Layout, Magnet, SoundEffect, Palette };
    /// @brief 从明确触摸入口切换工具，锚点为该按钮的屏幕位置。
    void toggleMobileTool(MobileTool tool, glm::vec2 anchor);
    /// @brief 查询浮层选中态，不读取会话或复制配置。
    /// @warning 每帧固定布尔查询，不允许阻塞或分配。
    bool mobileToolOpen(MobileTool tool) const;
    /// @brief 独立推进移动工具内容，隐藏工作台时收起浮层并提交未完成设置。
    /// @warning 每帧仅调用已打开内容；不能运行桌面工具栏的完整更新流程。
    void renderMobileTools(UIManager* manager, float scale, bool visible);

private:
    /// @brief 音效草稿、配色会话和分拍线切换历史的私有布局。
    struct Impl;
    /// @brief 仅在窗口构造时分配，绘制期间复用原有状态与缓冲。
    std::unique_ptr<Impl> m_impl;

    /// @brief 仅移动入口设为真，工具内容以工作台而非桌面工具栏定位。
    bool m_mobileTools{};
    /// @brief 移动调色页只修改画笔；选区需要显式确认，不复用桌面调色布局。
    /// @warning 每帧只读本地颜色和应用标量，不读取可变 SessionContext。
    void renderMobileColorPalette(float scale);
    /// @brief 编辑器状态读取与简单命令提交服务。
    IEditorApplicationService* m_editorApplicationService{ nullptr };

    /// @brief 仅供调色盘实现使用的编辑页标识。
    enum class PaletteTab;

    Logic::EditTool m_currentTool = Logic::EditTool::Marquee;
    /// @brief 进入布局模式前使用的工具，用于再次点击按钮时恢复。
    Logic::EditTool m_toolBeforeLayout = Logic::EditTool::Marquee;
    /// @brief 是否显示布局组件管理弹层。
    bool m_showLayoutPopup{ false };
    /// @brief 本帧布局工具按钮的屏幕左上角坐标，用于定位弹层。
    glm::vec2 m_lastLayoutBtnPos{ 0.0f, 0.0f };
    /// @brief 主工具栏保留自己的弹层尺寸与连续编辑状态。
    LayoutPanelState m_layoutPanel;
    /// @brief 时间线缩放浮层的显示状态，不存储缩放值副本。
    bool m_showZoomPopup{ false };
    /// @brief 本帧缩放按钮屏幕坐标，随横竖工具栏重新定位。
    glm::vec2 m_lastZoomBtnPos{ 0.0f, 0.0f };
    /// @brief 缩放浮层实际尺寸缓存，用于四边停靠时朝内展开。
    glm::vec2 m_zoomPopupSize{ 220.0f, 180.0f };
    bool      m_showDivisorPopup = false;
    /// @brief 本帧分拍数量按钮的完整屏幕锚点。
    glm::vec2 m_lastBtnPos{ 0.0f, 0.0f };
    float     m_popupWidth   = 160.0f;
    float     m_popupHeight  = 120.0f;
    bool      m_showKeyPopup = false;
    /// @brief 本帧轨道数按钮的完整屏幕锚点。
    glm::vec2 m_lastKeyBtnPos{ 0.0f, 0.0f };
    float     m_keyPopupWidth  = 160.0f;
    float     m_keyPopupHeight = 120.0f;
    /// @brief 是否显示磁铁工具设置弹窗。
    bool m_showMagnetPopup{ false };
    /// @brief 本帧磁铁工具按钮的屏幕左上角坐标，用于定位弹窗。
    glm::vec2 m_lastMagnetBtnPos{ 0.0f, 0.0f };
    /// @brief 磁铁工具弹窗上一帧宽度，用于防止视口越界。
    float m_magnetPopupWidth{ 280.0f };
    /// @brief 磁铁工具弹窗上一帧高度，用于防止视口越界。
    float m_magnetPopupHeight{ 360.0f };
    /// @brief 是否显示分拍线模式设置弹窗。
    bool m_showBeatLinePopup{ false };
    /// @brief 本帧分拍线模式按钮的屏幕左上角坐标，用于定位弹窗。
    glm::vec2 m_lastBeatLineBtnPos{ 0.0f, 0.0f };
    /// @brief 分拍线模式弹窗上一帧宽度。
    float m_beatLinePopupWidth{ 260.0f };
    /// @brief 分拍线模式弹窗上一帧高度。
    float m_beatLinePopupHeight{ 220.0f };
    /// @brief 自动显示比例是否有尚未写入配置文件的修改。
    bool m_beatLinePopupConfigDirty{ false };
    /// @brief 是否显示主音轨倍速详细调整弹窗。
    bool m_showSpeedPopup{ false };
    /// @brief 本帧倍速按钮的屏幕左上角坐标，用于定位弹窗。
    glm::vec2 m_lastSpeedBtnPos{ 0.0f, 0.0f };
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
     * @param height 按钮高度
     * @param shortLabel 图标下方显示的短标签
     * @param showLabel 是否显示短标签
     * @param sourceManager 当前 UI 管理器，用于恢复工具切换前的画布焦点
     */
    void drawToolButton(const char* icon, Logic::EditTool tool,
                        const char* tooltip, float width, float height,
                        const char* shortLabel, bool showLabel,
                        UIManager* sourceManager);

    /// @brief 绘制可再次点击退出的布局调整按钮。
    /// @param width 按钮宽度。
    /// @param height 按钮高度。
    /// @param showLabel 是否显示短标签。
    /// @param sourceManager 提供编辑器个性化引导状态。
    void drawLayoutButton(float width, float height, bool showLabel,
                          UIManager* sourceManager);

    /// @brief 获取当前逻辑编辑器配置，服务缺失时回退本地用户配置。
    /// @warning UI 热路径：只复制已有配置值，不执行文件 I/O。
    [[nodiscard]] Config::EditorConfig currentEditorConfig() const;

    /// @brief 将编辑器配置同步到逻辑线程。
    /// @warning UI 热路径低频分支：只在用户变更控件时调用。
    void updateEditorConfig(const Config::EditorConfig& config) const;

    /// @brief 绘制布局组件显隐管理弹层。
    /// @param dpiScale 当前 DPI 缩放。
    /// @param sourceManager 提供当前引导目标的突出层。
    /// @warning UI 热路径：仅在布局工具激活时绘制固定数量控件。
    void renderLayoutPopup(float dpiScale, UIManager* sourceManager,
                           ComparisonLayoutPanel* comparison = nullptr);

    /// @brief 绘制分拍线显示模式与自动渐隐范围弹窗。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI 热路径：仅在弹窗打开时绘制固定数量控件。
    void renderBeatLinePopup(float dpiScale);

    /// @brief 绘制滚动画布与物件放置磁吸设置弹窗。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI 热路径：仅在弹窗打开时绘制固定数量控件。
    void renderMagnetPopup(float dpiScale);

    /// @brief 绘制锚定在工具栏按钮旁的音效逐轨与分类控制弹层。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI
    /// 热路径：弹层打开时每帧执行，仅绘制滚动区可见轨道行并提交命令。
    void renderSoundEffectTool(float dpiScale);

    /// @brief 从当前皮肤默认方案初始化调色盘颜色。
    void initializeColorPalette();

    /// @brief 将调色盘重置为当前皮肤默认配色。
    void loadSkinDefaultPalette();

    /// @brief 载入软件默认调色方案。
    void loadSoftwareDefaultPalette();

    /// @brief 按方案名称加载软件级调色盘方案。
    /// @param schemeName 方案名称或皮肤默认方案标识。
    /// @return 成功应用方案时返回 true。
    bool loadPaletteSchemeByName(const std::string& schemeName);

    /// @brief 项目切换或默认方案变化后应用项目调色方案偏好。
    void applyProjectPalettePreference();

    /// @brief 同步颜色画笔方案；选择新方案时清除普通绘制的显式色。
    /// @param resetDrawColors 是否清除用户手动选择的新建物件颜色。
    void pushPaletteToBrush(bool resetDrawColors = true);

    /// @brief 将当前调色盘颜色应用到选中物件。
    void pushPaletteToSelection(bool explicitTouch = false);

    /// @brief 将当前分拍线配色写入运行时渲染配置。
    void pushBeatLinePaletteToRenderer();

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

    /// @brief 打开当前配色方案的导出文件选择器。
    /// @warning 用户触发的低频路径：原生文件选择器可能阻塞。
    void openPaletteExportFilePicker();

    /// @brief 打开完整调色方案的导入文件选择器。
    /// @warning 用户触发的低频路径：原生文件选择器可能阻塞。
    void openPaletteImportFilePicker();

    /// @brief 绘制并消费统一风格的配色方案导出文件选择器。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI 热路径：仅在统一文件选择器打开时绘制。
    void renderPaletteExportFileDialog(float dpiScale);

    /// @brief 绘制并消费统一风格的调色方案导入文件选择器。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI 热路径：仅在统一文件选择器打开时绘制。
    void renderPaletteImportFileDialog(float dpiScale);

    /// @brief 将当前配色方案导出到指定 UTF-8 路径。
    /// @param path 用户选择的目标路径。
    /// @warning 用户触发的低频路径：允许执行文件系统写入。
    void exportCurrentPaletteToPath(const std::string& path);

    /// @brief 读取调色方案并打开当场重命名确认框。
    /// @param path 用户选择的来源路径。
    /// @warning 用户触发的低频路径：允许执行文件系统读取。
    void preparePaletteImportFromPath(const std::string& path);

    /// @brief 以重命名后的名称确认导入完整调色方案。
    void confirmPaletteImport();

    /// @brief 构造当前弹窗中实际显示的完整配色方案。
    /// @return 包含物件颜色和可选分拍线覆盖颜色的方案。
    Config::ColorPaletteScheme buildCurrentPaletteScheme() const;

    /// @brief 重命名当前选中的调色盘方案。
    void renamePaletteScheme();

    /// @brief 删除指定自定义调色盘方案。
    /// @param schemeIndex 要删除的自定义方案索引。
    void deletePaletteScheme(std::size_t schemeIndex);

    /// @brief 将方案名写入编辑缓冲区。
    /// @param name 方案名。
    void setPaletteSchemeNameBuffer(const std::string& name);

    /// @brief 将颜色写入 HEX 输入缓冲区。
    /// @param tab 颜色所属标签页。
    /// @param slotIndex 颜色槽位索引。
    /// @param color 当前颜色。
    void setColorHexBuffer(PaletteTab tab, std::size_t slotIndex,
                           glm::vec4 color);

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

    /// @brief 绘制调色盘弹窗。
    /// @param dpiScale 当前 DPI 缩放。
    void renderColorPalettePopup(float dpiScale);

    /**
     * @brief 绘制左侧偏移的提示框
     * @param text 提示文本
     */
    void drawTooltip(const char* text);
};

}  // namespace MMM::UI
