#include "ui/imgui/manager/SettingsView.h"

#include "config/skin/translation/Translation.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/manager/SettingsSearchIndex.h"
#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"

#include <algorithm>
#include <optional>

namespace MMM::UI
{
/// @brief 绘制常驻搜索栏，查询缓存和设置正文共享同一语言版本。
/// @details 搜索只改变导航状态，不能调用 open() 重置窗口尺寸或停靠节点。
/// 输入框位于设置窗口根部，不随正文或结果 Child 滚动。
/// 同一个查询可以从结果返回正文，再返回结果，过程中保留原关键词。
/// 搜索输入不读取或应用任何设置值，原页仍负责权限和持久化。
/// 进入搜索结束快捷键录制，避免查询文本误录为新的快捷键绑定。
/// @warning UI 热路径常态只复用索引；输入和翻译版本变化才过滤排序。
void SettingsView::drawSettingsSearchBar()
{
    auto& translator = Translation::getActiveTranslator();
    // Translate 只借用稳定字符串池，转换为拥有文本仅发生在缓存失效时。
    const auto translate = [&translator](std::string_view key) {
        return translator.translate(Hash::hashString(key), key.data()).view();
    };
    m_settingsSearch->update(
        m_settingsSearchInput.data(), translator.getVersion(), translate);
    // 搜索占用自己的整行，窗口窄时操作按钮放在下一行，避免挤掉输入框。
    ImGui::SetNextItemWidth(-1.0f);
    if ( ImGui::InputTextWithHint("##SettingsSearch",
                                  TR_CACHE("ui.settings.search.hint").data(),
                                  m_settingsSearchInput.data(),
                                  m_settingsSearchInput.size()) ) {
        m_settingsSearch->update(
            m_settingsSearchInput.data(), translator.getVersion(), translate);
        m_showSettingsSearchResults = m_settingsSearch->hasQuery();
        // 新查询取消旧定位，不能让旧高亮或滚动抢占新结果列表。
        m_settingsSearchTarget.reset();
        m_settingsSearchScrollPending     = false;
        m_settingsSearchHighlightUntil    = 0.0;
        m_settingsSearchTargetUnavailable = false;
        // 搜索框占用键盘期间停止录制，避免输入查询变成快捷键绑定。
        m_recordingShortcutTarget = ShortcutRecordTarget::None;
        ShortcutUtils::setShortcutRecordingActive(false);
    }
    if ( m_settingsSearch->hasQuery() ) {
        // 导航保留查询，用户可以再次返回命中列表，无需重输关键词。
        if ( !m_showSettingsSearchResults ) {
            if ( FeedbackSmallButton(
                     TR_CACHE("ui.settings.search.back").data()) ) {
                m_showSettingsSearchResults = true;
                m_settingsSearchTarget.reset();
                m_settingsSearchScrollPending     = false;
                m_settingsSearchTargetUnavailable = false;
            }
            ImGui::SameLine();
        }
        if ( FeedbackSmallButton(TR_CACHE("ui.settings.search.clear").data()) )
            cancelSettingsSearchNavigation();
    }
    // 不显示不存在的可编辑控件，也不替用户修改依赖开关或创建工程。
    if ( m_settingsSearchTargetUnavailable && !m_showSettingsSearchResults )
        ImGui::TextWrapped("%s",
                           TR_CACHE("ui.settings.search.unavailable").data());
    ImGui::Separator();
}

/// @brief 展示缓存搜索结果，点击后在原页使用现有设置控件。
/// @details 名称之外展示所属分类和分组，区分不同页中的同名字段。
/// 结果只携带稳定目录索引；不复制原控件，不执行隐藏分类的回调。
/// 点击导航推迟到结果循环结束，避免迭代期间清空当前结果。
/// 原设置页将在下一帧绘制，与分类缓存和 Child 滚动状态保持一致。
/// @warning UI 热路径只遍历命中缓存，不执行其他分类的布局或设置副作用。
void SettingsView::drawSettingsSearchResults()
{
    const auto& entries = m_settingsSearch->entries();
    const auto& results = m_settingsSearch->results();
    ImGui::Text(TR_CACHE("ui.settings.search.count").data(), results.size());
    ImGui::TextWrapped("%s",
                       TR_CACHE("ui.settings.search.instructions").data());
    if ( results.empty() ) {
        // 空命中保留搜索框，不把没有结果误认为当前分类没有设置。
        ImGui::TextWrapped("%s", TR_CACHE("ui.settings.search.empty").data());
        return;
    }
    std::optional<std::size_t> selected;
    for ( const auto& result : results ) {
        const auto& entry = entries[result.m_index];
        ImGui::PushID(static_cast<int>(result.m_index));
        // 对齐名称与完整分类路径；相同名称在不同分类中保持不同控件 ID。
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5f));
        if ( FeedbackButton(entry.m_label.c_str(), { -1.0f, 0.0f }) )
            selected = result.m_index;
        ImGui::PopStyleVar();
        ImGui::TextDisabled(
            "%s / %s", entry.m_category.c_str(), entry.m_section.c_str());
        ImGui::PopID();
    }
    // 迭代结束后再改变导航状态，不能在循环中清除当前结果缓存。
    if ( selected ) navigateToSetting(*selected);
}

/// @brief 返回普通分类浏览，不改窗口停靠、录制之外的设置状态。
/// @details 外部打开指定分类、清空查询和点击分类按钮复用此取消入口。
/// 取消只清导航身份和计时，不恢复或回滚用户已经修改的设置。
/// 命中索引将在下一次搜索栏更新时清空，调用点不能继续按旧目标滚动。
void SettingsView::cancelSettingsSearchNavigation()
{
    m_settingsSearchInput.fill('\0');
    m_showSettingsSearchResults = false;
    m_settingsSearchTarget.reset();
    m_settingsSearchScrollPending     = false;
    m_settingsSearchHighlightUntil    = 0.0;
    m_settingsSearchTargetUnavailable = false;
}

/// @brief 搜索结果进入原设置页，保留查询供返回命中列表。
/// @details 导航与打开窗口不同，不重新停靠或刷新用户输入草稿。
/// 新目标清掉旧高亮截止时间，实际可见后才开始新的三秒高亮。
/// 只请求一次滚动，不跟随播放、布局变化或后续手动滚动。
/// @param index 稳定目录索引，非法索引不改变当前页面。
void SettingsView::navigateToSetting(std::size_t index)
{
    if ( index >= m_settingsSearch->entries().size() ) return;
    // 导航只切分类和设置行身份，不重置设置窗口或保存任何配置。
    m_currentTab                   = m_settingsSearch->entries()[index].m_tab;
    m_settingsSearchTarget         = index;
    m_showSettingsSearchResults    = false;
    m_settingsSearchScrollPending  = true;
    m_settingsSearchHighlightUntil = 0.0;
    m_settingsSearchTargetUnavailable = false;
    m_recordingShortcutTarget         = ShortcutRecordTarget::None;
    ShortcutUtils::setShortcutRecordingActive(false);
}

/// @brief 设置当前行分组并展开导航目标对应的折叠标题。
/// @param label 真实分组标签，字符串由翻译池保活。
/// @param storageId 原标题使用的状态 ID。
/// @return 需要强制展开实际 ImGui 标题时返回 true。
/// @details 每个 header 都登记分组身份，即使没有搜索也保持行匹配上下文。
/// 返回值用于同步标题控件状态；只修改 StateStorage 不能补建 Clay 子树。
/// storageId 为零表示页面没有折叠标题，只登记身份而不访问存储。
/// @warning UI 热路径只比较身份，常态帧不修改 StateStorage。
bool SettingsView::revealSettingsSearchSection(const char* label,
                                               ImGuiID     storageId)
{
    m_settingsSearchSection = label;
    if ( !m_settingsSearchTarget || !m_settingsSearchScrollPending )
        return false;
    const auto& target = m_settingsSearch->entries()[*m_settingsSearchTarget];
    if ( target.m_tab != m_currentTab || target.m_section != label )
        return false;
    // Clay 子树和 ImGui TreeNode 必须在同帧展开，避免只改视觉箭头。
    if ( storageId != 0 ) ImGui::GetStateStorage()->SetInt(storageId, 1);
    return true;
}

/// @brief 登记分组回退边界，实际设置行出现时优先使用该行。
/// @param label 分组实际标签。
/// @param bounds 标题的真实屏幕矩形。
/// @details 不在标题回调里滚动，因为后续回调可能还会登记目标设置行。
/// @warning UI 热路径只记录本帧矩形，不获取会话或配置锁。
void SettingsView::reportSettingsSearchHeader(const char*      label,
                                              Clay_BoundingBox bounds)
{
    if ( !m_settingsSearchTarget ) return;
    const auto& target = m_settingsSearch->entries()[*m_settingsSearchTarget];
    if ( target.m_tab == m_currentTab && target.m_section == label )
        m_settingsSearchHeaderBounds = bounds;
}

/// @brief 用分类、分组和标签共同匹配导航目标，不混淆同名自动备份字段。
/// @param label 真实设置行标签。
/// @return 只有当前正在浏览的原设置行身份完全一致时返回 true。
/// @details 在 Clay 登记阶段调用；回调值捕获此判断，避免之后分组改变。
/// @warning UI 热路径只比较已本地化的缓存文本，不重新查询翻译器。
bool SettingsView::isSettingsSearchTarget(const char* label) const
{
    if ( !m_settingsSearchTarget || m_showSettingsSearchResults ) return false;
    const auto& target = m_settingsSearch->entries()[*m_settingsSearchTarget];
    return target.m_tab == m_currentTab && target.m_label == label &&
           target.m_section == m_settingsSearchSection;
}

/// @brief 控件回调登记本帧整行边界，矩形不跨帧持久化。
/// @param bounds 实际设置行矩形。
/// @details 标准行与自动换行单选组都报告完整名称和控件区域。
/// @warning UI 热路径只写本帧值语义矩形，不保存任何控件对象地址。
void SettingsView::reportSettingsSearchRow(Clay_BoundingBox bounds)
{
    m_settingsSearchRowBounds = bounds;
}

/// @brief 整页完成后滚动目标并按主题绘制三秒高亮。
/// @details 隐藏设置定位到组标题；缺少工程或谱面时沿用原页提示。
/// 标题回退不会伪造不存在的设置行，也不会替用户打开依赖开关。
/// 自动滚动完成后，用户可以立即手动滚走，不持续抢占滚动位置。
/// 高亮只提交到正文窗口 DrawList，受当前 Child 裁剪限制。
/// 不使用 Foreground 遮罩或半透明填充，避免盖住设置控件与弹窗。
/// 高亮超时保留目标身份，以便隐藏设置提示和返回结果入口继续工作。
/// @warning UI 热路径只做固定次数几何计算，无阻塞等待或配置写入。
void SettingsView::finishSettingsSearchNavigation()
{
    if ( !m_settingsSearchTarget ) return;
    // 必须先判定整页是否登记了目标，避免标题回调早于设置行时误报隐藏。
    const auto bounds                 = m_settingsSearchRowBounds
                                            ? m_settingsSearchRowBounds
                                            : m_settingsSearchHeaderBounds;
    m_settingsSearchTargetUnavailable = !m_settingsSearchRowBounds.has_value();
    if ( !bounds ) {
        // 没有工程或谱面时页面可能完全没有分组，取消无效的重复滚动请求。
        m_settingsSearchScrollPending = false;
        return;
    }
    const auto windowPos  = ImGui::GetWindowPos();
    const auto windowSize = ImGui::GetWindowSize();
    if ( m_settingsSearchScrollPending ) {
        // 使用真实布局坐标定位；展开后的内容高度由本帧页面完整提交。
        // 滚动只请求一次，后续用户拖动滚动条不会被自动拉回。
        ImGui::SetScrollFromPosY(
            bounds->y - windowPos.y + bounds->height * 0.5f, 0.35f);
        m_settingsSearchScrollPending = false;
        return;
    }
    const bool visible = bounds->y + bounds->height > windowPos.y &&
                         bounds->y < windowPos.y + windowSize.y;
    // 离屏目标不启动计时，也不在另一个 Child 或弹窗上绘制高亮。
    // 目标始终以本帧矩形为准，字体和 DPI 改变后不使用旧像素位置。
    if ( !visible ) return;
    const double now = ImGui::GetTime();
    // 等滚动生效且目标可见才计时，不能在搜索结果点击时提前消耗高亮时长。
    if ( m_settingsSearchHighlightUntil == 0.0 )
        m_settingsSearchHighlightUntil = now + 3.0;
    if ( now >= m_settingsSearchHighlightUntil ) return;
    // 强调色跟随主题，不使用固定荧光色；仅画边框，不覆盖输入控件和弹窗。
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRect({ bounds->x, bounds->y },
                      { bounds->x + bounds->width, bounds->y + bounds->height },
                      ImGui::GetColorU32(ImGuiCol_HeaderActive),
                      ImGui::GetStyle().FrameRounding,
                      0,
                      2.0f);
}
}  // namespace MMM::UI
