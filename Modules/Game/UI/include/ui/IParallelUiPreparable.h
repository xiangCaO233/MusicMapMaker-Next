#pragma once

#include "imgui.h"
#include <cstdint>
#include <string>

namespace MMM::UI
{

/// @brief UI 每帧并行准备阶段使用的只读快照。
struct UiFrameSnapshot {
    /// @brief 当前窗口内容缩放，已经至少为 1。
    float dpiScale{ 1.0f };

    /// @brief 当前 ImGui FramePadding。
    ImVec2 framePadding{ 0.0f, 0.0f };

    /// @brief 当前 ImGui 单帧控件高度。
    float frameHeight{ 0.0f };

    /// @brief 当前 ImGui 含间距的单帧控件高度。
    float frameHeightWithSpacing{ 0.0f };

    /// @brief 当前内容字体观察指针，只允许并行阶段只读测量。
    ImFont* contentFont{ nullptr };

    /// @brief 当前菜单字体观察指针，只允许并行阶段只读测量。
    ImFont* menuFont{ nullptr };

    /// @brief 当前文件管理器字体观察指针，只允许并行阶段只读测量。
    ImFont* fileManagerFont{ nullptr };

    /// @brief 当前 ImGui 默认字体观察指针，只允许并行阶段只读测量。
    ImFont* fallbackFont{ nullptr };

    /// @brief 当前 ImGui 字体尺寸。
    float fontSize{ 0.0f };

    /// @brief 当前翻译缓存版本。
    uint32_t translationVersion{ 0 };

    /// @brief 当前语言。
    std::string language;

    /// @brief 当前 ASCII 字体选择。
    std::string preferredAsciiFont;

    /// @brief 当前 CJK 字体选择。
    std::string preferredCjkFont;

    /// @brief 当前字体倍率。
    float fontSizeMultiplier{ 1.0f };

    /// @brief 当前 UI 缩放倍率。
    float uiScaleMultiplier{ 1.0f };

    /// @brief 当前窗口内边距。
    float windowPadding{ 0.0f };

    /// @brief 当前控件间距。
    float itemSpacing{ 0.0f };

    /// @brief 皮肤侧栏基础宽度配置。
    std::string sidebarWidthConfig;
};

/// @brief 可选的 UI 并行准备接口。
class IParallelUiPreparable
{
public:
    /// @brief 默认析构。
    virtual ~IParallelUiPreparable() = default;

    /// @brief 判断当前帧是否需要进入并行准备队列。
    /// @param snapshot 当前帧只读快照。
    /// @return 需要准备时返回 true。
    /// @warning UI 热路径：每帧主线程调用，只允许检查脏位和轻量状态。
    virtual bool needsParallelUiPrepare(
        const UiFrameSnapshot& snapshot) const = 0;

    /// @brief 在线程池中准备本帧 UI 数据。
    /// @param snapshot 当前帧只读快照。
    /// @warning 后台线程路径：禁止调用 ImGui API，禁止修改全局 UI 状态。
    virtual void prepareUiFrameData(const UiFrameSnapshot& snapshot) = 0;

    /// @brief 将准备好的数据切换到主线程可读状态。
    /// @warning UI 热路径：线程池任务全部结束后在主线程调用，禁止阻塞。
    virtual void swapPreparedUiFrameData() = 0;
};

}  // namespace MMM::UI
