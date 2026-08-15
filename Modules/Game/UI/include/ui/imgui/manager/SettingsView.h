#pragma once

#include "config/CreatorIdentity.h"
#include "event/ui/UISettingsTabEvent.h"
#include "graphic/imguivk/VKTexture.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/ITextureLoader.h"
#include "ui/IUIView.h"
#include "ui/layout/box/CLayBox.h"
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::Config
{
struct ShortcutBinding;
}  // namespace MMM::Config

namespace MMM::UI
{

/// @brief 编辑器设置面板视图，负责设置分类导航和各设置页渲染。
class SettingsView : virtual public IUIView, public IParallelUiPreparable
{
public:
    /// @brief 构造设置面板视图。
    /// @param viewName 视图名称。
    SettingsView(const std::string& viewName);

    /// @brief 禁止移动构造，避免 UI 布局缓存和事件订阅状态被搬移。
    SettingsView(SettingsView&&) = delete;

    /// @brief 禁止拷贝构造，避免 UI 布局缓存和事件订阅状态被复制。
    SettingsView(const SettingsView&) = delete;

    /// @brief 禁止移动赋值，避免 UI 布局缓存和事件订阅状态被覆盖。
    SettingsView& operator=(SettingsView&&) = delete;

    /// @brief 禁止拷贝赋值，避免 UI 布局缓存和事件订阅状态被覆盖。
    SettingsView& operator=(const SettingsView&) = delete;

    /// @brief 析构设置面板视图并取消事件订阅。
    ~SettingsView() override;

    /// @brief 打开设置窗口并切换到指定设置页。
    /// @param tab 需要激活的设置页。
    void open(Event::SettingsTab tab);

    /// @brief 请求下一帧将设置窗口停靠到主编辑区中心标签页。
    void requestDockToCenter();

    /// @brief 请求下一帧聚焦设置窗口。
    void requestFocus();

    /// @brief 更新并渲染独立设置窗口。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning 热路径：每帧 UI
    /// 更新时执行；禁止加入文件系统扫描或重型资源重建。
    void update(UIManager* sourceManager) override;

    /// @brief 获取实际实例指针，用于禁用 RTTI 时安全下行转换。
    /// @return 当前设置视图实例。
    void* getActualInstance() override { return this; }

    /// @brief 安全转换为 UI 并行准备接口。
    /// @return 当前设置视图的并行准备接口。
    IParallelUiPreparable* asParallelUiPreparable() override { return this; }

    /// @brief 判断当前帧设置窗口是否需要准备布局数据。
    /// @param snapshot 当前帧 UI 快照。
    /// @return 需要刷新布局缓存时返回 true。
    /// @warning UI 热路径：每帧主线程调用，只检查缓存状态。
    bool needsParallelUiPrepare(const UiFrameSnapshot& snapshot) const override;

    /// @brief 要求布局准备在 UI 主线程执行，避免动态字形烘焙并发写入字体图集。
    /// @return 始终返回 true。
    /// @warning UI 热路径：每帧只返回固定能力标记。
    bool requiresMainThreadUiPrepare() const override { return true; }

    /// @brief 在 UI 主线程准备设置窗口布局数据。
    /// @param snapshot 当前帧 UI 快照。
    /// @warning 文本测量可能触发动态字形烘焙，禁止在线程池执行。
    void prepareUiFrameData(const UiFrameSnapshot& snapshot) override;

    /// @brief 将准备好的布局数据切换给主线程使用。
    void swapPreparedUiFrameData() override;

private:
    /// @brief 设置页快捷键录制目标。
    enum class ShortcutRecordTarget {
        None,
        ToolMove,
        ToolMarquee,
        ToolDraw,
        ToolColorBrush,
        ToolColorEraser,
        Mirror,
        MirrorPaste,
        EditSelectedVolume,
        DeleteSelected,
        TogglePlayback,
        ToggleReverseScroll,
        ToggleScrollSnap,
        ToggleSnapFloor,
        ToggleScrollTimingMapping,
        ToggleBeatLines,
        ToggleStopPlaybackOnScroll,
        ToggleHitSfx,
        ToggleHitEffects,
        ToggleSyncSameMainAudio,
    };

    /// @brief 协作服务器设置最近一次应用结果。
    enum class CollaborationServerApplyState {
        None,
        Succeeded,
        Failed,
    };

    /// @brief 设置窗口布局测量缓存。
    struct LayoutMetricsCache {
        /// @brief 缓存是否可用。
        bool valid{ false };

        /// @brief 缓存对应的设置页。
        Event::SettingsTab tab{ Event::SettingsTab::Software };

        /// @brief 缓存对应的窗口内容缩放。
        float dpiScale{ 1.0f };

        /// @brief 缓存对应的语言。
        std::string language;

        /// @brief 缓存对应的翻译版本。
        uint32_t translationVersion{ 0 };

        /// @brief 缓存对应的 ASCII 字体选择。
        std::string preferredAsciiFont;

        /// @brief 缓存对应的 CJK 字体选择。
        std::string preferredCjkFont;

        /// @brief 缓存对应的字体倍率。
        float fontSizeMultiplier{ 1.0f };

        /// @brief 缓存对应的 UI 缩放倍率。
        float uiScaleMultiplier{ 1.0f };

        /// @brief 缓存对应的窗口内边距。
        float windowPadding{ 0.0f };

        /// @brief 缓存对应的控件间距。
        float itemSpacing{ 0.0f };

        /// @brief 缓存对应的皮肤侧栏基础宽度。
        std::string sidebarWidthConfig;

        /// @brief 分类侧栏宽度。
        float categorySidebarWidth{ 0.0f };

        /// @brief 当前设置页标签列宽度。
        float tabLabelWidth{ 0.0f };

        /// @brief 当前设置页控件列最小宽度。
        float tabWidgetWidth{ 0.0f };

        /// @brief 当前设置页最小窗口尺寸。
        ImVec2 minWindowSize{ 0.0f, 0.0f };
    };

    /// @brief 当前激活的设置分类页。
    Event::SettingsTab m_currentTab = Event::SettingsTab::Software;

    /// @brief 设置页切换事件订阅 ID。
    uint64_t m_tabSubId = 0;

    /// @brief 当前帧 UI 管理器观察指针，不持有所有权。
    UIManager* m_sourceManager{ nullptr };

    /// @brief 根级横向布局缓存。
    CLayHBox m_rootHBox;

    /// @brief 内容区域纵向布局缓存。
    CLayVBox m_contentVBox;

    /// @brief 设置项行布局缓存池，用于减少 UI 热路径堆分配。
    std::deque<CLayHBox> m_settingRows;

    /// @brief 设置段落布局缓存池，用于减少 UI 热路径堆分配。
    std::deque<CLayVBox> m_sectionBoxes;

    /// @brief 当前谱面设置页正在编辑的元数据副本。
    ::MMM::BaseMapMeta m_editingMeta;

    /// @brief 上一次同步到元数据编辑副本的谱面路径。
    std::string m_lastBeatmapPath;

    /// @brief 软件设置页默认 Creator 的固定长度 UTF-8 输入缓冲区。
    std::array<char, Config::MAX_CREATOR_IDENTITY_BYTES + 1>
        m_defaultCreatorInputBuffer{};

    /// @brief 协作服务器地址的固定长度 UTF-8 输入缓冲区。
    std::array<char, 256> m_collaborationServerAddressInputBuffer{};

    /// @brief 协作服务器信令端口的待应用输入值。
    int m_collaborationSignalingPortInput{ 443 };

    /// @brief 协作服务器 TLS/WSS 的待应用输入值。
    bool m_collaborationUseTlsInput{ true };

    /// @brief 协作服务器设置最近一次应用反馈。
    CollaborationServerApplyState m_collaborationServerApplyState{
        CollaborationServerApplyState::None
    };

    /// @brief 新建停靠窗口首帧可能覆盖焦点请求，因此连续请求两帧。
    static constexpr uint8_t FOCUS_REQUEST_FRAME_COUNT = 2;

    /// @brief 设置窗口剩余的聚焦请求帧数。
    uint8_t m_focusRequestFramesRemaining{ 0 };

    /// @brief 下一帧是否尝试将设置窗口停靠到主编辑区中心。
    bool m_dockToCenterNextFrame{ true };

    /// @brief 布局测量缓存，避免设置窗口每帧重复测量大量文本。
    mutable LayoutMetricsCache m_layoutMetricsCache;

    /// @brief 等待切换的布局测量缓存。
    LayoutMetricsCache m_preparedLayoutMetricsCache;

    /// @brief 是否有布局缓存等待主线程切换。
    bool m_hasPreparedLayoutMetrics{ false };

    /// @brief 当前正在录制的快捷键目标。
    ShortcutRecordTarget m_recordingShortcutTarget{
        ShortcutRecordTarget::None
    };

    /// @brief 已扫描到的皮肤目录名缓存。
    std::vector<std::string> m_availableSkinDirectories;

    /// @brief 是否需要重新扫描皮肤目录。
    bool m_availableSkinDirectoriesDirty{ true };

    /// @brief 绘制设置窗口内部内容。
    void drawContent();

    /// @brief 构造设置窗口布局测量缓存。
    /// @param snapshot 当前帧 UI 快照。
    /// @param tab 需要测量的设置页。
    /// @return 设置窗口布局测量结果。
    static LayoutMetricsCache buildLayoutMetrics(
        const UiFrameSnapshot& snapshot, Event::SettingsTab tab);

    /// @brief 判断布局测量缓存是否匹配当前帧状态。
    /// @param cache 需要检查的布局缓存。
    /// @param snapshot 当前帧 UI 快照。
    /// @param tab 当前设置页。
    /// @return 完全匹配时返回 true。
    static bool layoutMetricsMatch(const LayoutMetricsCache& cache,
                                   const UiFrameSnapshot&    snapshot,
                                   Event::SettingsTab        tab);

    /// @brief 获取当前设置页布局测量缓存。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 与当前语言、字体、缩放和设置页匹配的布局测量结果。
    /// @warning UI 热路径：仅在语言、字体、缩放或设置页变化时重新测量。
    const LayoutMetricsCache& getLayoutMetrics(float dpiScale) const;

    /// @brief 获取当前设置页标签列宽度。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 当前设置页标签列宽度。
    float getCurrentTabLabelWidth(float dpiScale) const;

    /// @brief 计算设置窗口当前内容所需的最小整窗尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return ImGui 窗口最小尺寸。
    ImVec2 getMinWindowSize(float dpiScale) const;

    /// @brief 计算设置分类侧栏中不可再换行文本所需的宽度。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 分类侧栏宽度。
    float getCategorySidebarWidth(float dpiScale) const;

    /// @brief 刷新可选皮肤目录名缓存。
    /// @warning 低频文件系统路径：只在设置窗口打开或缓存标脏时扫描
    /// AppPaths::skinsRootPath()，禁止每帧无条件调用。
    void refreshAvailableSkinDirectories();

    /// @brief 从持久化设置刷新默认 Creator 输入缓冲区。
    void refreshDefaultCreatorInputBuffer();

    /// @brief 从持久化设置刷新协作服务器输入缓冲区。
    void refreshCollaborationServerInputBuffer();

    /// @brief 应用皮肤选择并请求图形/音频资源热重载。
    /// @param skinDirectoryName skins 根目录下的皮肤目录名。
    /// @param skinLuaPath 皮肤入口脚本路径。
    /// @return 切换成功时返回 true。
    /// @warning 低频资源重载路径：会加载 Lua、清理音效池并请求 Vulkan
    /// 资源重建，只能由设置页皮肤选择触发。
    bool applySkinSelection(const std::string&           skinDirectoryName,
                            const std::filesystem::path& skinLuaPath);

    /// @brief 在系统文件管理器中打开用户 skins 目录。
    /// @warning 低频用户操作路径：会创建目录并启动系统文件管理器。
    void openSkinDirectory();

    /// @brief 打开 MSK 皮肤包导入选择器。
    /// @return 原生选择器立即完成导入并切换皮肤时返回 true。
    /// @warning 低频用户操作路径：原生选择器会阻塞当前 UI 操作直至关闭。
    bool openSkinImportFilePicker();

    /// @brief 打开当前皮肤的 MSK 导出选择器。
    /// @warning 低频用户操作路径：原生选择器会阻塞当前 UI 操作直至关闭。
    void openSkinExportFilePicker();

    /// @brief 处理统一 MSK 导入与导出选择器。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 导入并切换到新皮肤时返回 true。
    bool renderSkinPackageFileDialogs(float dpiScale);

    /// @brief 从指定 MSK 文件导入并切换到新皮肤。
    /// @param packagePath 待导入的 MSK 文件。
    /// @return 导入及皮肤切换均成功时返回 true。
    /// @warning 低频资源路径：会解压文件、写入用户 skins
    /// 目录并触发皮肤资源热重载。
    bool importSkinPackage(const std::filesystem::path& packagePath);

    /// @brief 将当前皮肤导出到指定 MSK 文件。
    /// @param outputPath 文件选择器返回的输出路径。
    /// @warning 低频资源路径：会遍历当前皮肤目录并压缩全部普通文件。
    void exportCurrentSkinPackage(const std::filesystem::path& outputPath);

    /// @brief 绘制软件设置页。
    void drawSoftwareSettings();

    /// @brief 绘制视觉设置页。
    void drawVisualSettings();

    /// @brief 绘制多人协作设置页。
    /// @warning UI 热路径：设置窗口打开且当前页为协作页时每帧执行；仅在
    /// 用户确认应用时修改目录连接并持久化配置。
    void drawCollaborationSettings();

    /// @brief 绘制项目设置页。
    void drawProjectSettings();

    /// @brief 绘制谱面设置页。
    void drawBeatmapSettings();

    /// @brief 绘制编辑器设置页。
    void drawEditorSettings();

    /// @brief 绘制快捷键设置页。
    /// @warning UI 热路径：设置窗口打开且当前页为快捷键页时每帧执行；
    /// 只查询当前帧键盘状态并更新配置。
    void drawShortcutSettings();

    /// @brief 绘制单个快捷键录制控件。
    /// @param binding 正在编辑的快捷键绑定。
    /// @param target 当前控件对应的录制目标。
    /// @param id ImGui 控件 ID 后缀。
    /// @param width 控件可用宽度。
    /// @param changed 发生修改时写入 true。
    void drawShortcutBindingControl(Config::ShortcutBinding& binding,
                                    ShortcutRecordTarget target, const char* id,
                                    float width, bool& changed);

    /// @brief 绘制调试设置页。
    /// @warning UI 热路径：设置窗口打开且当前页为调试页时每帧执行。
    /// 禁止加入文件系统扫描或重型资源重建。
    void drawDebugSettings();

    /// @brief 获取或创建一个行布局。
    /// @param index 行布局缓存索引。
    /// @return 可复用的横向行布局。
    CLayHBox& getRow(size_t index);

    /// @brief 获取或创建一个段落容器布局。
    /// @param index 段落布局缓存索引。
    /// @return 可复用的纵向段落布局。
    CLayVBox& getSection(size_t index);

    /// @brief 添加一个设置项行（标签 + 控件）。
    /// @param parent 接收设置行的父级布局。
    /// @param rowIndex 当前行索引，会在添加时递增。
    /// @param label 设置项标签。
    /// @param labelWidth 标签列宽度。
    /// @param widget 控件绘制回调。
    void addSettingItem(CLayVBox& parent, size_t& rowIndex, const char* label,
                        float labelWidth, CLayBox::DrawFunc widget);

    /// @brief 添加一个带自动换行的 RadioButton 组。
    /// @param parent 接收设置行的父级布局。
    /// @param rowIndex 当前行索引，会在添加时递增。
    /// @param sectionIndex 当前段落索引，会在添加时递增。
    /// @param label 设置项标签。
    /// @param labelWidth 标签列宽度。
    /// @param options 单选项文本和值列表。
    /// @param current 当前选中的值。
    /// @param changed 发生修改时写入 true。
    void addRadioSetting(
        CLayVBox& parent, size_t& rowIndex, size_t& sectionIndex,
        const char* label, float labelWidth,
        const std::vector<std::pair<std::string, int>>& options, int& current,
        bool& changed);
};

}  // namespace MMM::UI
