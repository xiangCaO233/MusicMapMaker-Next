#pragma once

#include "audio/AudioManager.h"
#include "event/core/EventBus.h"
#include "mmm/project/AudioResource.h"
#include "ui/ISubView.h"
#include "ui/layout/box/CLayBox.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace MMM::UI
{
class AudioManagerView : public ISubView, public IParallelUiPreparable
{
public:
    /// @brief 创建音频管理器并订阅项目目录刷新事件。
    /// @param subViewName 子视图名称。
    AudioManagerView(const std::string& subViewName);
    AudioManagerView(AudioManagerView&&)                 = delete;
    AudioManagerView(const AudioManagerView&)            = delete;
    AudioManagerView& operator=(AudioManagerView&&)      = delete;
    AudioManagerView& operator=(const AudioManagerView&) = delete;
    /// @brief 取消目录刷新订阅并销毁音频管理器。
    ~AudioManagerView() override;

    /// @brief 内部绘制逻辑 (Clay/ImGui)
    /// @param layoutContext 当前 Clay/ImGui 布局上下文。
    /// @param sourceManager 打开音频控制器所需的 UI 管理器。
    /// @warning UI 热路径：音频管理器可见时每帧执行；
    /// 避免额外增加项目音频资源遍历和所有权复制。
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

    /// @brief 获取音频管理器中不可再换行控件所需的最小内容尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 音频管理器的动态最小内容尺寸。
    ImVec2 getMinContentSize(float dpiScale) const override;

    /// @brief 安全转换为 UI 并行准备接口。
    /// @return 当前音频管理器子视图的并行准备接口。
    IParallelUiPreparable* asParallelUiPreparable() override { return this; }

    /// @brief 判断当前帧是否需要准备音频管理器布局数据。
    /// @param snapshot 当前帧 UI 快照。
    /// @return 需要刷新布局缓存时返回 true。
    /// @warning UI 热路径：每帧主线程调用，只捕获音频资源数量和缓存状态。
    bool needsParallelUiPrepare(const UiFrameSnapshot& snapshot) const override;

    /// @brief 要求布局准备在 UI 主线程执行，避免动态字形烘焙并发写入字体图集。
    /// @return 始终返回 true。
    /// @warning UI 热路径：每帧只返回固定能力标记。
    bool requiresMainThreadUiPrepare() const override { return true; }

    /// @brief 在 UI 主线程准备音频管理器布局测量数据。
    /// @param snapshot 当前帧 UI 快照。
    /// @warning 文本测量可能触发动态字形烘焙，禁止在线程池执行。
    void prepareUiFrameData(const UiFrameSnapshot& snapshot) override;

    /// @brief 将准备好的布局测量数据切换给主线程使用。
    void swapPreparedUiFrameData() override;

private:
    /// @brief 音频管理器布局输入快照。
    struct LayoutInputSnapshot {
        /// @brief 当前是否有打开的项目。
        bool hasProject{ false };

        /// @brief 当前皮肤常驻音效数量。
        size_t permanentSfxCount{ 0 };

        /// @brief 当前项目全部音频资源数量。
        size_t projectAudioCount{ 0 };

        /// @brief 当前是否展开全局设置段落。
        bool showGlobalSettings{ true };
    };

    /// @brief 音频管理器布局测量缓存。
    struct LayoutMetricsCache {
        /// @brief 缓存是否可用。
        bool valid{ false };

        /// @brief 缓存对应的布局输入快照。
        LayoutInputSnapshot input;

        /// @brief 缓存对应的窗口内容缩放。
        float dpiScale{ 1.0f };

        /// @brief 缓存对应的字体尺寸。
        float fontSize{ 0.0f };

        /// @brief 缓存对应的 FramePadding。
        ImVec2 framePadding{ 0.0f, 0.0f };

        /// @brief 缓存对应的单帧控件高度。
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

        /// @brief 页脚控制标签列宽度。
        float footerLabelWidth{ 0.0f };

        /// @brief 音频管理器内容的 Clay 布局外侧留白。
        float rootPadding{ 0.0f };

        /// @brief 音频管理器分区之间的主纵向间距。
        float sectionSpacing{ 0.0f };

        /// @brief 紧凑音频列表行之间的间距。
        float listRowSpacing{ 0.0f };

        /// @brief 普通列表行和音频条目的高度。
        float audioItemHeight{ 0.0f };

        /// @brief 空项目提示间隔行高度。
        float hintSpacerHeight{ 0.0f };

        /// @brief 空项目提示文本行高度。
        float hintRowHeight{ 0.0f };

        /// @brief 全局控制页脚横向留白。
        float footerPaddingX{ 0.0f };

        /// @brief 全局控制页脚内部纵向间距。
        float footerSpacing{ 0.0f };

        /// @brief 全局控制标题行高度。
        float footerHeaderHeight{ 0.0f };

        /// @brief 每个全局音频控制行的高度。
        float controlRowHeight{ 0.0f };

        /// @brief 控制行中标签、按钮和滑块之间的间距。
        float controlColumnGap{ 0.0f };

        /// @brief 标签列中文本和填充区之间的间距。
        float labelColumnGap{ 0.0f };

        /// @brief 控制行中静音图标按钮的尺寸。
        float muteButtonSize{ 0.0f };

        /// @brief 全宽导入音频按钮高度。
        float importButtonHeight{ 0.0f };

        /// @brief 导入音频按钮上方间距。
        float importButtonGap{ 0.0f };

        /// @brief 页脚总高度，包含导入音频按钮。
        float footerHeight{ 0.0f };

        /// @brief 导入按钮前全局控制区域消耗的高度。
        float globalControlsHeight{ 0.0f };

        /// @brief 子视图内容最小尺寸。
        ImVec2 minContentSize{ 0.0f, 0.0f };
    };

    /// @brief 音频资源表格排序字段。
    enum class AudioTableSortKey {
        /// @brief 按音频资源 ID 排序。
        Id,

        /// @brief 按音频资源类型排序。
        Type,

        /// @brief 按音频资源路径排序。
        Path,

        /// @brief 按音频文件大小排序。
        Size,

        /// @brief 按音频文件最后修改时间排序。
        ModifiedTime
    };

    /// @brief 音频资源表格排序方向。
    enum class SortDirection {
        /// @brief 升序。
        Ascending,

        /// @brief 降序。
        Descending
    };

    /// @brief 音频资源表格行来源。
    enum class AudioTableRowKind {
        /// @brief 皮肤界面交互音效。
        InteractionSfx,

        /// @brief 皮肤常驻音效。
        PermanentSfx,

        /// @brief 项目主音轨。
        MainTrack,

        /// @brief 项目音效音轨。
        ProjectSfx
    };

    /// @brief 音频资源表格缓存行。
    struct AudioTableRow {
        /// @brief 音频资源 ID。
        std::string m_id;

        /// @brief 音频资源路径。
        std::string m_path;

        /// @brief 用于按需读取元数据的音频文件绝对路径。
        std::filesystem::path m_absolutePath;

        /// @brief 音频资源类型。
        AudioTrackType m_type{ AudioTrackType::Effect };

        /// @brief 表格行来源。
        AudioTableRowKind m_kind{ AudioTableRowKind::ProjectSfx };

        /// @brief 音频文件大小字节数；读取失败时保持为 0。
        std::uintmax_t m_size{ 0 };

        /// @brief 是否成功读取音频文件大小。
        bool m_hasSize{ false };

        /// @brief 音频文件最后修改时间；读取失败时不参与精确排序。
        std::filesystem::file_time_type m_lastWriteTime{};

        /// @brief 是否成功读取最后修改时间。
        bool m_hasLastWriteTime{ false };

        /// @brief 是否已经尝试读取文件大小和修改时间。
        bool m_metadataLoaded{ false };
    };

    /// @brief 当前是否展开全局设置段落。
    bool m_showGlobalSettings = true;

    /// @brief 当前音频表格排序字段。
    AudioTableSortKey m_audioTableSortKey{ AudioTableSortKey::Id };

    /// @brief 当前音频表格排序方向。
    SortDirection m_audioTableSortDirection{ SortDirection::Ascending };

    /// @brief 音频资源表格行缓存。
    std::vector<AudioTableRow> m_audioTableRows;

    /// @brief 各类音频资源分组的展开状态，索引与分组排序等级一致。
    std::array<bool, 4> m_audioTableGroupExpanded{ false, false, true, true };

    /// @brief 各类音频资源在排序行缓存中的起始索引。
    std::array<size_t, 4> m_audioTableGroupStarts{};

    /// @brief 各类音频资源在排序行缓存中的资源数量。
    std::array<size_t, 4> m_audioTableGroupSizes{};

    /// @brief 上次构造音频表格缓存时的皮肤常驻音效数量。
    size_t m_cachedPermanentSfxCount{ 0 };

    /// @brief 上次构造音频表格缓存时的项目音频数量。
    size_t m_cachedProjectAudioCount{ 0 };

    /// @brief 上次构造音频表格缓存时的项目指针，仅用于识别项目切换。
    const void* m_cachedAudioTableProject{ nullptr };

    /// @brief 音频表格排序缓存是否需要重建。
    bool m_audioTableSortCacheDirty{ true };

    /// @brief 外部目录重扫后等待 UI 线程消费的音频缓存刷新标记。
    /// @warning 目录重扫由逻辑线程写入、UI 热路径读取；为避免跨线程直接修改
    /// 表格缓存，只使用单个原子脏标记传递低频刷新请求。
    std::atomic<bool> m_projectDirectoryRefreshPending{ false };

    /// @brief 项目目录重扫事件订阅 ID。
    Event::SubscriptionID m_projectDirectoryRefreshedSubId{ 0 };

    /// @brief 当前后端缓存的输出设备列表。
    std::vector<Audio::AudioOutputDevice> m_cachedOutputDevices;

    /// @brief 输出设备缓存对应的播放后端。
    Config::AudioPlaybackBackend m_cachedOutputDeviceBackend{
        Config::AudioPlaybackBackend::SDL
    };

    /// @brief 输出设备列表是否需要重新枚举。
    bool m_outputDevicesDirty{ true };

    /// @brief 等待移除确认的项目音轨 ID。
    std::string m_removeTrackId;

    /// @brief 下一帧是否打开移除音轨确认窗口。
    bool m_openRemoveModal{ false };

    // --- 布局池 (用于避免热路径堆分配) ---
    std::deque<CLayHBox> m_settingRows;
    std::deque<CLayHBox> m_subHBoxes;
    std::deque<CLayVBox> m_subVBoxes;

    /// @brief 布局测量缓存，避免每帧重复测量大量文本。
    mutable LayoutMetricsCache m_layoutMetricsCache;

    /// @brief 主线程捕获并传给布局准备阶段的布局输入。
    mutable LayoutInputSnapshot m_prepareLayoutInput;

    /// @brief 等待切换的布局测量缓存。
    LayoutMetricsCache m_preparedLayoutMetricsCache;

    /// @brief 是否有布局缓存等待主线程切换。
    bool m_hasPreparedLayoutMetrics{ false };

    /// @brief 捕获当前音频管理器布局输入。
    /// @return 当前项目、皮肤音效数量和展开状态。
    /// @warning UI 热路径：每帧只读取容器大小，禁止遍历或拷贝资源内容。
    LayoutInputSnapshot captureLayoutInput() const;

    /// @brief 获取音频管理器布局测量缓存。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 与当前语言、字体、缩放和资源数量匹配的布局测量结果。
    const LayoutMetricsCache& getLayoutMetrics(float dpiScale) const;

    /// @brief 构造音频管理器布局测量缓存。
    /// @param snapshot 当前帧 UI 快照。
    /// @param input 当前布局输入。
    /// @return 音频管理器布局测量结果。
    static LayoutMetricsCache buildLayoutMetrics(
        const UiFrameSnapshot& snapshot, const LayoutInputSnapshot& input);

    /// @brief 判断布局测量缓存是否匹配当前帧状态。
    /// @param cache 需要检查的布局缓存。
    /// @param snapshot 当前帧 UI 快照。
    /// @param input 当前布局输入。
    /// @return 完全匹配时返回 true。
    static bool layoutMetricsMatch(const LayoutMetricsCache&  cache,
                                   const UiFrameSnapshot&     snapshot,
                                   const LayoutInputSnapshot& input);

    // 辅助方法：获取或创建一个行布局
    CLayHBox& getRow(size_t index)
    {
        while ( m_settingRows.size() <= index ) {
            m_settingRows.emplace_back();
        }
        return m_settingRows[index];
    }

    CLayHBox& getSubHBox(size_t index)
    {
        while ( m_subHBoxes.size() <= index ) {
            m_subHBoxes.emplace_back();
        }
        return m_subHBoxes[index];
    }

    CLayVBox& getSubVBox(size_t index)
    {
        while ( m_subVBoxes.size() <= index ) {
            m_subVBoxes.emplace_back();
        }
        return m_subVBoxes[index];
    }
};

}  // namespace MMM::UI
