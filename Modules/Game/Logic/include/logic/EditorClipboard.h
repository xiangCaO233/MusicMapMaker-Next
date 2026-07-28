#pragma once

#include "logic/session/context/SessionContext.h"
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Logic
{

/// @brief 编辑器级共享剪贴板，封装跨 Session 复制/剪切状态。
class EditorClipboard
{
public:
    /// @brief 构造一个空的编辑器级剪贴板。
    EditorClipboard() = default;

    /// @brief 析构编辑器级剪贴板。
    ~EditorClipboard() = default;

    /// @brief 禁止拷贝构造，避免复制互斥量和剪贴板状态。
    EditorClipboard(const EditorClipboard&) = delete;

    /// @brief 禁止拷贝赋值，避免复制互斥量和剪贴板状态。
    EditorClipboard& operator=(const EditorClipboard&) = delete;

    /// @brief 禁止移动构造，保持剪贴板状态地址稳定。
    EditorClipboard(EditorClipboard&&) = delete;

    /// @brief 禁止移动赋值，保持剪贴板状态地址稳定。
    EditorClipboard& operator=(EditorClipboard&&) = delete;

    /// @brief 更新编辑器级剪贴板内容。
    /// @param items 新的剪贴板条目列表。
    /// @param sourceContext 剪贴板来源 Session
    /// 上下文，仅用于身份比较，不拥有生命周期。
    /// @param isCut 当前剪贴板内容是否来自剪切操作。
    void set(std::vector<ClipboardItem> items,
             const SessionContext* sourceContext, bool isCut);

    /// @brief 更新编辑器级混合谱面物件剪贴板内容。
    /// @param notes 新的音符剪贴板条目列表。
    /// @param samples 新的自动采样剪贴板条目列表。
    /// @param sourceContext 剪贴板来源 Session 上下文，仅用于身份比较。
    /// @param isCut 当前剪贴板内容是否来自剪切操作。
    void setChartObjects(std::vector<ClipboardItem>       notes,
                         std::vector<SampleClipboardItem> samples,
                         const SessionContext* sourceContext, bool isCut);

    /// @brief 更新编辑器级 Timeline 剪贴板内容。
    /// @param items 新的 Timeline 剪贴板条目列表。
    /// @param sourceContext 剪贴板来源 Session
    /// 上下文，仅用于身份比较，不拥有生命周期。
    /// @param isCut 当前剪贴板内容是否来自剪切操作。
    void setTimelines(std::vector<TimelineClipboardItem> items,
                      const SessionContext* sourceContext, bool isCut);

    /// @brief 获取编辑器级剪贴板内容副本。
    /// @return 当前剪贴板条目列表副本。
    std::vector<ClipboardItem> get() const;

    /// @brief 获取编辑器级自动采样剪贴板内容副本。
    /// @return 当前自动采样剪贴板条目列表副本。
    std::vector<SampleClipboardItem> getSamples() const;

    /// @brief 获取编辑器级 Timeline 剪贴板内容副本。
    /// @return 当前 Timeline 剪贴板条目列表副本。
    std::vector<TimelineClipboardItem> getTimelines() const;

    /// @brief 判断当前剪贴板是否是指定 Session 的剪切内容。
    /// @param context 待比较的 Session 上下文。
    /// @return 当前剪贴板是否来自该 Session 的剪切操作。
    bool isCutFrom(const SessionContext* context) const;

    /// @brief 获取需要跨 Session 消费的剪切来源上下文。
    /// @param pasteContext 正在执行粘贴的目标 Session 上下文。
    /// @return 需要被消费的来源 Session 上下文；没有跨 Session 剪切时返回
    /// nullptr。
    const SessionContext* getCrossSessionCutSource(
        const SessionContext* pasteContext) const;

    /// @brief 将当前剪切剪贴板标记为已经消费。
    void markCutConsumed();

    /// @brief 消费需要发布到系统剪贴板的文本载荷。
    /// @return 复制或剪切改变编辑器剪贴板后待发布的系统剪贴板文本。
    std::optional<std::string> consumePendingSystemText();

    /// @brief 从系统剪贴板文本导入 MMM 剪贴板载荷。
    /// @param text 待解析的系统剪贴板文本。
    /// @return 文本属于 MMM 剪贴板协议时返回 true。
    bool importSystemText(std::string_view text);

private:
    /// @brief 保护剪贴板内容、来源 Session 和剪切状态的互斥量。
    mutable std::mutex m_mutex;

    /// @brief 编辑器级共享剪贴板条目列表。
    std::vector<ClipboardItem> m_items;

    /// @brief 编辑器级共享自动采样剪贴板条目列表。
    std::vector<SampleClipboardItem> m_sampleItems;

    /// @brief 编辑器级共享 Timeline 剪贴板条目列表。
    std::vector<TimelineClipboardItem> m_timelineItems;

    /// @brief 当前剪贴板内容是否来自剪切操作。
    bool m_isCut{ false };

    /// @brief 剪切来源 Session 上下文，仅用于身份比较，不拥有生命周期。
    const SessionContext* m_sourceContext{ nullptr };

    /// @brief 等待 UI 线程发布到系统剪贴板的文本。
    std::optional<std::string> m_pendingSystemText;

    /// @brief 本进程上次导出的文本，用于保留本地剪切状态。
    std::string m_lastExportedSystemText;
};

}  // namespace MMM::Logic
