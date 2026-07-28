#pragma once

#include "audio/AudioManager.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/IUIView.h"
#include "ui/layout/box/CLayBox.h"
#include <cstdint>
#include <deque>
#include <string>

namespace MMM::UI
{

/**
 * @brief 音轨控制器 UI，绑定一个音频音轨（BGM 或 SFX 池）
 */
class AudioTrackControllerUI : virtual public IUIView,
                               public IParallelUiPreparable
{
public:
    enum class TrackType { Main, Effect };

    /// @brief 构造项目工作区使用的稳定视图名称。
    /// @param trackId 音轨标识符。
    /// @return UIManager 中注册音轨控制器使用的视图名。
    static std::string makeViewName(const std::string& trackId);

    /// @brief 将音轨类型转换为项目工作区中的稳定文本。
    /// @param type 音轨类型。
    /// @return Main 或 Effect。
    static const char* trackTypeToWorkspaceName(TrackType type);

    /// @brief 从项目工作区稳定文本恢复音轨类型。
    /// @param name 工作区保存的音轨类型文本。
    /// @return 对应的音轨类型；未知文本按主音轨处理。
    static TrackType workspaceNameToTrackType(const std::string& name);

    /// @param trackId 音轨标识符
    /// @param trackName 显示名称
    /// @param type 音轨类型
    AudioTrackControllerUI(const std::string& trackId,
                           const std::string& trackName, TrackType type);

    ~AudioTrackControllerUI() override = default;

    void update(UIManager* sourceManager) override;

    void* getActualInstance() override { return this; }

    /// @brief 安全转换为 UI 并行准备接口。
    /// @return 当前音轨控制器的并行准备接口。
    IParallelUiPreparable* asParallelUiPreparable() override { return this; }

    /// @brief 判断当前帧音轨控制器是否需要准备布局测量数据。
    /// @param snapshot 当前帧 UI 快照。
    /// @return 需要刷新布局缓存时返回 true。
    /// @warning UI 热路径：每帧主线程调用，只检查缓存状态。
    bool needsParallelUiPrepare(const UiFrameSnapshot& snapshot) const override;

    /// @brief 要求布局准备在 UI 主线程执行，避免动态字形烘焙并发写入字体图集。
    /// @return 始终返回 true。
    /// @warning UI 热路径：每帧只返回固定能力标记。
    bool requiresMainThreadUiPrepare() const override { return true; }

    /// @brief 在 UI 主线程准备音轨控制器布局测量数据。
    /// @param snapshot 当前帧 UI 快照。
    /// @warning 文本测量可能触发动态字形烘焙，禁止在线程池执行。
    void prepareUiFrameData(const UiFrameSnapshot& snapshot) override;

    /// @brief 将准备好的布局测量数据切换给主线程使用。
    void swapPreparedUiFrameData() override;

    const std::string& getTrackId() const { return m_trackId; }

    /// @brief 请求下一次显示时停靠到指定 Dock 节点。
    /// @param dockId 目标 ImGui Dock 节点 ID，0 表示不改变停靠位置。
    void requestDockTo(ImGuiID dockId);

    /// @brief 请求下一次更新时将音轨控制器窗口聚焦到前台。
    void requestFocus();

    /// @brief 获取音轨控制器显示名称。
    /// @return 当前窗口显示名称。
    const std::string& getTrackName() const { return m_trackName; }

    /// @brief 获取音轨类型。
    /// @return 当前音轨控制器绑定的类型。
    TrackType getTrackType() const { return m_type; }

private:
    /// @brief 音轨控制器布局测量缓存。
    struct LayoutMetricsCache {
        /// @brief 缓存是否可用。
        bool valid{ false };

        /// @brief 缓存对应的音轨类型。
        TrackType trackType{ TrackType::Main };

        /// @brief 缓存对应的窗口标题。
        std::string trackName;

        /// @brief 缓存对应的窗口内容缩放。
        float dpiScale{ 1.0f };

        /// @brief 缓存对应的字体尺寸。
        float fontSize{ 0.0f };

        /// @brief 缓存对应的 ImGui FramePadding。
        ImVec2 framePadding{ 0.0f, 0.0f };

        /// @brief 缓存对应的 ImGui 单帧控件高度。
        float frameHeight{ 0.0f };

        /// @brief 缓存对应的含间距控件高度。
        float frameHeightWithSpacing{ 0.0f };

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

        /// @brief 标签列宽度。
        float labelWidth{ 0.0f };

        /// @brief Clay 内容容器内边距。
        float contentPadding{ 0.0f };

        /// @brief Clay 内容容器纵向间距。
        float contentSpacing{ 0.0f };

        /// @brief 设置项行水平内边距。
        float rowPaddingX{ 0.0f };

        /// @brief 设置项行垂直内边距。
        float rowPaddingY{ 0.0f };

        /// @brief 设置项标签和控件之间的水平间距。
        float rowSpacing{ 0.0f };

        /// @brief 普通设置项行高。
        float rowHeight{ 0.0f };

        /// @brief 普通按钮高度。
        float buttonHeight{ 0.0f };

        /// @brief 静音按钮宽度。
        float muteButtonWidth{ 0.0f };

        /// @brief 主音轨声道按钮宽度。
        float channelButtonWidth{ 0.0f };

        /// @brief 预设按钮换行后的行内间距。
        float presetSpacing{ 0.0f };

        /// @brief EQ 关闭时的最小窗口尺寸。
        ImVec2 minWindowSize{ 0.0f, 0.0f };

        /// @brief EQ 打开时的最小窗口尺寸。
        ImVec2 minWindowSizeWithEq{ 0.0f, 0.0f };
    };

    /// @brief 构建音量区域的 Clay 布局
    /// @warning UI 每帧绘制路径：仅允许轻量 ImGui 控件测量与样式栈操作。
    void buildVolumeSection(CLayVBox& parent, size_t& rowIndex,
                            float labelWidth, float& volume, bool& muted,
                            bool& changed);
    /// @brief 构建速度和音高区域的 Clay 布局
    void buildSpeedAndPitchSection(CLayVBox& parent, size_t& rowIndex,
                                   float labelWidth, float availWidgetW,
                                   float& speed, float& pitch, bool& changed);
    /// @brief 渲染项目音频资源自身的 EQ 配置。
    /// @param config 当前项目音频资源配置。
    /// @param changed 任一持久化字段发生变化时置为 true。
    void renderEQSection(AudioTrackConfig& config, bool& changed);
    /// @brief 构建音效预览区域的 Clay 布局
    void buildEffectPreviewSection(CLayVBox& parent, size_t& rowIndex,
                                   float labelWidth);
    /// @brief 构建波形/频谱按钮的 Clay 布局
    void buildAnalysisButtons(CLayVBox& parent, size_t& rowIndex,
                              UIManager* sourceManager);

    /// @brief 获取或创建一个行布局
    CLayHBox& getRow(size_t index);
    /// @brief 获取或创建一个段落容器布局
    CLayVBox& getSection(size_t index);
    /// @brief 测量标签文本的像素宽度
    float measureLabelWidth(const char* label);
    /// @brief 计算音轨控制器当前音轨类型所需的最小整窗尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return ImGui 窗口最小尺寸。
    ImVec2 getMinWindowSize(float dpiScale) const;
    /// @brief 获取音轨控制器布局测量缓存。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 与当前语言、字体、缩放和音轨类型匹配的布局测量结果。
    /// @warning UI 热路径：仅在缓存未命中时同步测量；通常由并行准备提前填充。
    const LayoutMetricsCache& getLayoutMetrics(float dpiScale) const;
    /// @brief 构造音轨控制器布局测量缓存。
    /// @param snapshot 当前帧 UI 快照。
    /// @param trackType 当前音轨类型。
    /// @param trackName 当前窗口标题。
    /// @return 音轨控制器布局测量结果。
    static LayoutMetricsCache buildLayoutMetrics(
        const UiFrameSnapshot& snapshot, TrackType trackType,
        const std::string& trackName);
    /// @brief 判断布局测量缓存是否匹配当前帧状态。
    /// @param cache 需要检查的布局缓存。
    /// @param snapshot 当前帧 UI 快照。
    /// @param trackType 当前音轨类型。
    /// @param trackName 当前窗口标题。
    /// @return 完全匹配时返回 true。
    static bool layoutMetricsMatch(const LayoutMetricsCache& cache,
                                   const UiFrameSnapshot&    snapshot,
                                   TrackType                 trackType,
                                   const std::string&        trackName);
    /// @brief 添加一个设置项行（标签 + 控件）
    void addSettingItem(CLayVBox& parent, size_t& rowIndex, const char* label,
                        float labelWidth, CLayBox::DrawFunc widget,
                        float heightOverride = 0.0f);

private:
    std::string m_trackId;
    std::string m_trackName;
    TrackType   m_type;

    Audio::EQPreset m_currentPreset{ Audio::EQPreset::None };

    /// @brief Clay 布局容器池
    CLayVBox             m_contentVBox;
    std::deque<CLayHBox> m_rows;
    std::deque<CLayVBox> m_sections;

    /// @brief 下一帧需要停靠到的目标 Dock 节点。
    ImGuiID m_pendingDockId{ 0 };

    /// @brief 下一帧是否请求窗口聚焦。
    bool m_shouldFocusNextFrame{ false };

    /// @brief 布局测量缓存，避免每帧重复测量大量文本。
    mutable LayoutMetricsCache m_layoutMetricsCache;

    /// @brief 等待切换的布局测量缓存。
    LayoutMetricsCache m_preparedLayoutMetricsCache;

    /// @brief 是否有布局缓存等待主线程切换。
    bool m_hasPreparedLayoutMetrics{ false };
};

}  // namespace MMM::UI
