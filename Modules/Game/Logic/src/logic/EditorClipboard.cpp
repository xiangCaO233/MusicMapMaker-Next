#include "logic/EditorClipboard.h"
#include "logic/EditorClipboardProtocol.h"
#include "logic/session/context/SessionContext.h"
#include <utility>

namespace MMM::Logic
{

/// @brief 更新编辑器级剪贴板内容。
/// @param items 转交给剪贴板持有的音符快照。
/// @param sourceContext 来源会话，用于剪切归属和隔离判定。
/// @param isCut 是否为剪切，而非普通复制。
void EditorClipboard::set(std::vector<ClipboardItem> items,
                          const SessionContext* sourceContext, bool isCut)
{
    // 纯音符入口复用混合物件存储，避免两套剪贴板状态不同步。
    setChartObjects(std::move(items), {}, sourceContext, isCut);
}

/// @brief 更新编辑器级混合谱面物件剪贴板内容。
/// @param notes 音符快照。
/// @param samples 使用相对 BGM 轨道的样本快照。
/// @param sourceContext 来源会话，空指针表示无会话归属。
/// @param isCut 是否需要后续消费来源的剪切状态。
/// @warning 用户复制/剪切路径，锁内移动容器并序列化文本，不用于逐帧调用。
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
    // 新载荷取代另一领域的数据，不能混用上次复制留下的时间线条目。
    m_sourceContext = sourceContext;
    m_isCut         = isCut && (!m_items.empty() || !m_sampleItems.empty());
    // 空载荷不产生待消费的剪切，即使调用方请求剪切也不保留该状态。
    m_sessionOnly =
        sourceContext && sourceContext->collaborationClipboardIsolated;
    m_sessionScopeId =
        m_sessionOnly ? sourceContext->collaborationClipboardScopeId : 0U;
    if ( m_sessionOnly ) {
        // 隔离内容不导出到系统，同时撤销尚未被 UI 消费的旧导出文本。
        m_pendingSystemText.reset();
    } else {
        m_pendingSystemText = EditorClipboardProtocol::serializeChartObjects(
            m_items, m_sampleItems);
    }
}

/// @brief 更新编辑器级 Timeline 剪贴板内容。
/// @param items 时间线条目值快照。
/// @param sourceContext 来源会话及其当前隔离作用域。
/// @param isCut 是否为需要消费的剪切操作。
void EditorClipboard::setTimelines(std::vector<TimelineClipboardItem> items,
                                   const SessionContext* sourceContext,
                                   bool                  isCut)
{
    /// @brief 保护本次 Timeline 剪贴板写入的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.clear();
    m_sampleItems.clear();
    // 时间线与谱面物件是互斥载荷，一次复制只保留当前选择的领域。
    m_timelineItems = std::move(items);
    m_sourceContext = sourceContext;
    m_isCut         = isCut && !m_timelineItems.empty();
    m_sessionOnly =
        sourceContext && sourceContext->collaborationClipboardIsolated;
    m_sessionScopeId =
        m_sessionOnly ? sourceContext->collaborationClipboardScopeId : 0U;
    if ( m_sessionOnly ) {
        m_pendingSystemText.reset();
    } else {
        m_pendingSystemText =
            EditorClipboardProtocol::serializeTimelines(m_timelineItems);
    }
}

/// @brief 获取目标 Session 可访问的编辑器级剪贴板内容副本。
/// @param targetContext 粘贴目标，决定是否有权读取当前隔离载荷。
/// @return 独立音符副本；无权读取时返回空集合。
std::vector<ClipboardItem> EditorClipboard::get(
    const SessionContext* targetContext) const
{
    /// @brief 保护本次剪贴板读取的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    return canReadFrom(targetContext) ? m_items : std::vector<ClipboardItem>{};
}

/// @brief 获取目标 Session 可访问的自动采样剪贴板内容副本。
/// @return 独立样本副本，释放锁后不再依赖内部容器存储。
std::vector<SampleClipboardItem> EditorClipboard::getSamples(
    const SessionContext* targetContext) const
{
    /// @brief 保护本次自动采样剪贴板读取的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    return canReadFrom(targetContext) ? m_sampleItems
                                      : std::vector<SampleClipboardItem>{};
}

/// @brief 获取目标 Session 可访问的 Timeline 剪贴板内容副本。
/// @return 可访问的时间线副本；隔离策略不通过时不暴露条目。
std::vector<TimelineClipboardItem> EditorClipboard::getTimelines(
    const SessionContext* targetContext) const
{
    /// @brief 保护本次 Timeline 剪贴板读取的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    return canReadFrom(targetContext) ? m_timelineItems
                                      : std::vector<TimelineClipboardItem>{};
}

/// @brief 判断当前剪贴板是否是指定 Session 的剪切内容。
/// @param context 候选来源会话。
/// @return 来源身份、剪切标志和当前读取权限同时匹配才返回 true。
bool EditorClipboard::isCutFrom(const SessionContext* context) const
{
    /// @brief 保护本次剪切来源比较的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_isCut && m_sourceContext == context && canReadFrom(context);
}

/// @brief 获取需要跨 Session 消费的剪切来源上下文。
/// @param pasteContext 当前粘贴目标。
/// @return 非拥有的来源指针；不满足跨会话剪切条件时返回空指针。
/// @note 来源关闭时必须调用 clearForContext，返回指针不延长会话生命周期。
const SessionContext* EditorClipboard::getCrossSessionCutSource(
    const SessionContext* pasteContext) const
{
    /// @brief 保护本次跨 Session 剪切来源查询的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    if ( !canReadFrom(pasteContext) || m_sessionOnly || !m_isCut ||
         !m_sourceContext || m_sourceContext == pasteContext ) {
        // 同会话剪切另行处理，隔离载荷绝不跨会话转移来源删除责任。
        return nullptr;
    }
    return m_sourceContext;
}

/// @brief 将当前剪切剪贴板标记为已经消费。
/// @note 保留载荷以便再次粘贴，但不再触发来源删除或重复剪切消费。
void EditorClipboard::markCutConsumed()
{
    /// @brief 保护本次剪切消费状态更新的临界区。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_isCut         = false;
    m_sourceContext = nullptr;
}

/// @brief 消费需要发布到系统剪贴板的文本载荷。
/// @return 待发布文本的所有权；没有新文本时为空，不用空字符串代表无请求。
std::optional<std::string> EditorClipboard::consumePendingSystemText()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if ( !m_pendingSystemText ) {
        return std::nullopt;
    }

    std::string text = std::move(*m_pendingSystemText);
    // 导出是一次性请求，记录文本用于识别系统剪贴板回读产生的回环。
    m_pendingSystemText.reset();
    m_lastExportedSystemText = text;
    return text;
}

/// @brief 从系统剪贴板文本导入 MMM 剪贴板载荷。
/// @param text 当前系统剪贴板文本，在本次调用期间保持有效。
/// @return 识别到自身导出文本或成功解析协议时返回 true。
/// @note 有效协议可能解析为空载荷，此时仍替换原内容，不沿用旧的非空条目。
bool EditorClipboard::importSystemText(std::string_view text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if ( text == m_lastExportedSystemText ) {
        // 自己导出的文本无需重新导入，否则会丢失内部剪切来源与状态。
        return true;
    }

    auto parsed = EditorClipboardProtocol::parse(text);
    if ( !parsed ) {
        // 普通文本或非法协议不覆盖内部数据，调用方可使用其他粘贴路径。
        return false;
    }

    m_items         = std::move(parsed->notes);
    m_sampleItems   = std::move(parsed->samples);
    m_timelineItems = std::move(parsed->timelines);
    m_sourceContext = nullptr;
    // 外部文本没有可信的会话身份，只导入为普通复制内容而非跨会话剪切。
    m_isCut          = false;
    m_sessionOnly    = false;
    m_sessionScopeId = 0U;
    return true;
}

/// @brief 来源会话销毁或重置时清除其拥有的剪贴板状态。
/// @param context 即将失效的来源会话，空值或无关会话不影响当前内容。
void EditorClipboard::clearForContext(const SessionContext* context)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if ( !context || m_sourceContext != context ) return;
    // 先确认归属再清理，关闭其他标签不能删除当前会话刚复制的数据。

    m_items.clear();
    m_sampleItems.clear();
    m_timelineItems.clear();
    m_isCut          = false;
    m_sourceContext  = nullptr;
    m_sessionOnly    = false;
    m_sessionScopeId = 0U;
    m_pendingSystemText.reset();
    m_lastExportedSystemText.clear();
    // 同时失效导出回环记录，后续相同系统文本可以作为无来源的外部复制导入。
}

/// @brief 检查目标是否处于允许读取当前载荷的作用域。
/// @param targetContext 候选目标会话。
/// @return 普通载荷只对非隔离目标开放，隔离载荷要求同会话同作用域。
/// @pre 调用方已持有 m_mutex；本 helper 不重复加锁。
/// @note 读取的是目标会话当前隔离状态，而不是复制时对目标状态的缓存。
bool EditorClipboard::canReadFrom(const SessionContext* targetContext) const
{
    const bool targetIsolated =
        targetContext && targetContext->collaborationClipboardIsolated;
    if ( !m_sessionOnly ) return !targetIsolated;
    // 会话指针相同仍不够，重新进入隔离后 scope ID 改变，旧内容必须不可读。
    return targetIsolated && targetContext == m_sourceContext &&
           targetContext->collaborationClipboardScopeId == m_sessionScopeId;
}

}  // namespace MMM::Logic
