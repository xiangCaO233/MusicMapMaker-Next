#include "logic/EditorClipboard.h"
#include "logic/EditorClipboardProtocol.h"
#include <utility>

namespace MMM::Logic
{

/// @brief 更新编辑器级剪贴板内容。
void EditorClipboard::set(std::vector<ClipboardItem> items,
                          const SessionContext* sourceContext, bool isCut)
{
    setChartObjects(std::move(items), {}, sourceContext, isCut);
}

/// @brief 更新编辑器级混合谱面物件剪贴板内容。
void EditorClipboard::setChartObjects(std::vector<ClipboardItem>       notes,
                                      std::vector<SampleClipboardItem> samples,
                                      const SessionContext* sourceContext,
                                      bool                  isCut)
{
    /// @brief 保护本次剪贴板写入的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items       = std::move(notes);
    m_sampleItems = std::move(samples);
    m_timelineItems.clear();
    m_sourceContext = sourceContext;
    m_isCut         = isCut && (!m_items.empty() || !m_sampleItems.empty());
    m_pendingSystemText =
        EditorClipboardProtocol::serializeChartObjects(m_items, m_sampleItems);
}

/// @brief 更新编辑器级 Timeline 剪贴板内容。
void EditorClipboard::setTimelines(std::vector<TimelineClipboardItem> items,
                                   const SessionContext* sourceContext,
                                   bool                  isCut)
{
    /// @brief 保护本次 Timeline 剪贴板写入的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.clear();
    m_sampleItems.clear();
    m_timelineItems = std::move(items);
    m_sourceContext = sourceContext;
    m_isCut         = isCut && !m_timelineItems.empty();
    m_pendingSystemText =
        EditorClipboardProtocol::serializeTimelines(m_timelineItems);
}

/// @brief 获取编辑器级剪贴板内容副本。
std::vector<ClipboardItem> EditorClipboard::get() const
{
    /// @brief 保护本次剪贴板读取的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_items;
}

/// @brief 获取编辑器级自动采样剪贴板内容副本。
std::vector<SampleClipboardItem> EditorClipboard::getSamples() const
{
    /// @brief 保护本次自动采样剪贴板读取的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sampleItems;
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

/// @brief 消费需要发布到系统剪贴板的文本载荷。
std::optional<std::string> EditorClipboard::consumePendingSystemText()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if ( !m_pendingSystemText ) {
        return std::nullopt;
    }

    std::string text = std::move(*m_pendingSystemText);
    m_pendingSystemText.reset();
    m_lastExportedSystemText = text;
    return text;
}

/// @brief 从系统剪贴板文本导入 MMM 剪贴板载荷。
bool EditorClipboard::importSystemText(std::string_view text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if ( text == m_lastExportedSystemText ) {
        return true;
    }

    auto parsed = EditorClipboardProtocol::parse(text);
    if ( !parsed ) {
        return false;
    }

    m_items         = std::move(parsed->notes);
    m_sampleItems   = std::move(parsed->samples);
    m_timelineItems = std::move(parsed->timelines);
    m_sourceContext = nullptr;
    m_isCut         = false;
    return true;
}

}  // namespace MMM::Logic
