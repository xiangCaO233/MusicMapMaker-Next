#include "logic/EditorClipboard.h"
#include <utility>

namespace MMM::Logic
{

/// @brief 更新编辑器级剪贴板内容。
void EditorClipboard::set(std::vector<ClipboardItem> items,
                          const SessionContext* sourceContext, bool isCut)
{
    /// @brief 保护本次剪贴板写入的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items = std::move(items);
    m_timelineItems.clear();
    m_sourceContext = sourceContext;
    m_isCut         = isCut && !m_items.empty();
}

/// @brief 更新编辑器级 Timeline 剪贴板内容。
void EditorClipboard::setTimelines(std::vector<TimelineClipboardItem> items,
                                   const SessionContext* sourceContext,
                                   bool                  isCut)
{
    /// @brief 保护本次 Timeline 剪贴板写入的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.clear();
    m_timelineItems = std::move(items);
    m_sourceContext = sourceContext;
    m_isCut         = isCut && !m_timelineItems.empty();
}

/// @brief 获取编辑器级剪贴板内容副本。
std::vector<ClipboardItem> EditorClipboard::get() const
{
    /// @brief 保护本次剪贴板读取的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_items;
}

/// @brief 获取编辑器级 Timeline 剪贴板内容副本。
std::vector<TimelineClipboardItem> EditorClipboard::getTimelines() const
{
    /// @brief 保护本次 Timeline 剪贴板读取的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_timelineItems;
}

/// @brief 判断当前剪贴板是否是指定 Session 的剪切内容。
bool EditorClipboard::isCutFrom(const SessionContext* context) const
{
    /// @brief 保护本次剪切来源比较的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_isCut && m_sourceContext == context;
}

/// @brief 获取需要跨 Session 消费的剪切来源上下文。
const SessionContext* EditorClipboard::getCrossSessionCutSource(
    const SessionContext* pasteContext) const
{
    /// @brief 保护本次跨 Session 剪切来源查询的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    if ( !m_isCut || !m_sourceContext || m_sourceContext == pasteContext ) {
        return nullptr;
    }
    return m_sourceContext;
}

/// @brief 将当前剪切剪贴板标记为已经消费。
void EditorClipboard::markCutConsumed()
{
    /// @brief 保护本次剪切消费状态更新的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_isCut         = false;
    m_sourceContext = nullptr;
}

}  // namespace MMM::Logic
