#include "ui/imgui/manager/SettingsSearchIndex.h"
#include "BuiltinSettingsSearch.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 分类枚举与翻译键的显式映射，不要求枚举值连续。
/// @details 软件光标行使用 editor 前缀，项目备份行使用 software 前缀。
/// 因此行的所属分类必须来自目录身份，不能从标签翻译键推断。
/// 这里的顺序仅用于读取目录分类名，不参与用户配置序列化。
constexpr std::array CATEGORY_KEYS{
    std::pair{ Event::SettingsTab::Software, "ui.settings.software" },
    std::pair{ Event::SettingsTab::Collaboration, "ui.settings.collaboration" },
    std::pair{ Event::SettingsTab::Visual, "ui.settings.visual" },
    std::pair{ Event::SettingsTab::Project, "ui.settings.project" },
    std::pair{ Event::SettingsTab::Beatmap, "ui.settings.beatmap" },
    std::pair{ Event::SettingsTab::Editor, "ui.settings.editor" },
    std::pair{ Event::SettingsTab::Shortcut, "ui.settings.shortcut" },
    std::pair{ Event::SettingsTab::Debug, "ui.settings.debug" }
};
/// @brief 目录中分类名与枚举的显式映射，与可见文本无关。
constexpr std::array CATEGORY_NAMES{ "Software", "Collaboration", "Visual",
                                     "Project",  "Beatmap",       "Editor",
                                     "Shortcut", "Debug" };

/// @brief 只判断 ASCII 空白，避免逐字节 locale 转换破坏中文 UTF-8。
/// @details 输入来自 UTF-8 文本框，非 ASCII 字节必须作为原文保留。
/// 搜索不做拼音转换，也不把单个汉字的多个字节当成多个关键词。
/// @param value 单个输入字节。
/// @return 空格、换行等 ASCII 空白时返回 true。
bool isSpace(char value)
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

/// @brief ASCII 大小写归一化并裁切首尾空白，多字节文本保持原样。
/// @details 英文标识与界面标题可以大小写混用；中文采用完整子串匹配。
/// 本函数不依赖 locale，避免配置或操作系统语言改变匹配语义。
/// Unicode 的语言别名由目录明确提供，不进行可能误伤字段的自动替换。
/// @param value UTF-8 字段或查询。
/// @return 适合连续子串匹配的文本。
std::string fold(std::string_view value)
{
    // 裁切只用于查询和单个字段，不依赖进程 locale。
    while ( !value.empty() && isSpace(value.front()) ) value.remove_prefix(1);
    while ( !value.empty() && isSpace(value.back()) ) value.remove_suffix(1);
    std::string result(value);
    for ( char& byte : result ) {
        // 非 ASCII 字节不参与转换，完整中文字词按字节子串仍能正确匹配。
        if ( byte >= 'A' && byte <= 'Z' ) byte += 'a' - 'A';
    }
    return result;
}

/// @brief 按空白拆分查询，多个词可以分别命中不同搜索字段。
/// @details 每个词是连续子串，所有词同时命中才接受候选。
/// 字段之间留空白，防止前一个字段尾部与后一个字段头部拼成假词。
/// 这里只拆分用户输入；别名不会扩大成单独的虚构设置项。
/// @param query 已归一化查询，切片的生命周期由调用者保证。
/// @return 非空关键词视图，不分配各词文本副本。
std::vector<std::string_view> tokens(std::string_view query)
{
    std::vector<std::string_view> result;
    // 空格只划分词，不允许重复空白产生空词并扩大命中集合。
    while ( !query.empty() ) {
        const auto end   = query.find_first_of(" \t\r\n");
        const auto token = query.substr(0, end);
        if ( !token.empty() ) result.push_back(token);
        if ( end == std::string_view::npos ) break;
        query.remove_prefix(end + 1);
    }
    return result;
}
}  // namespace

/// @brief 低频构造嵌入目录，非法条目不参与搜索。
/// @details JSON 只在视图创建时解析一次，查询和常态帧不执行解析或磁盘读取。
/// 目录条目只承载界面已有设置的语义身份，不能持有配置引用或控件闭包。
/// 分类名在内置表中查找，未知分类直接拒绝，避免落到错误的设置页。
/// 可选 fields 和 aliases 容错读取，缺失时仍允许按名称和字段键查找。
/// 个人配置、项目路径和当前设置数值均不加入目录，也不会写回目录。
/// 稳定索引在本实例生命周期内不变，搜索排序只排列索引引用。
SettingsSearchIndex::SettingsSearchIndex()
{
    // 嵌入内容仍使用无异常解析，损坏构建资源不能通过异常中止程序。
    const auto catalog =
        nlohmann::json::parse(BUILTIN_SETTINGS_SEARCH, nullptr, false);
    // 目录与可见 UI 一起编译，语言覆写只会改变文本，不会改变此结构。
    // 解析失败时安全返回空索引，原设置页仍可以通过分类正常访问。
    if ( !catalog.is_object() || !catalog.contains("entries") ||
         !catalog["entries"].is_array() )
        return;
    for ( const auto& value : catalog["entries"] ) {
        // 只有四个稳定身份字段有效时才接受行，运行态设置值不在此目录中。
        if ( !value.is_object() || !value.contains("tab") ||
             !value["tab"].is_string() || !value.contains("section") ||
             !value["section"].is_string() || !value.contains("label") ||
             !value["label"].is_string() )
            continue;
        const auto& name = value["tab"].get_ref<const std::string&>();
        const auto  found =
            std::find(CATEGORY_NAMES.begin(), CATEGORY_NAMES.end(), name);
        // 未知分类不能误导航到软件页，也不能形成没有实际入口的结果。
        if ( found == CATEGORY_NAMES.end() ) continue;
        SettingsSearchEntry entry;
        entry.m_tab = CATEGORY_KEYS[found - CATEGORY_NAMES.begin()].first;
        entry.m_sectionKey = value["section"].get_ref<const std::string&>();
        entry.m_labelKey   = value["label"].get_ref<const std::string&>();
        if ( value.contains("fields") && value["fields"].is_array() ) {
            // 显式说明及可选值来自原行的翻译键，不能根据匹配词猜测控件类型。
            // 未知字段被忽略，保持目录升级前后读取的向前兼容边界。
            // 说明、选项和提示归属当前行，不创建额外的重复结果。
            for ( const auto& field : value["fields"] )
                if ( field.is_string() )
                    entry.m_fieldKeys.push_back(
                        field.get_ref<const std::string&>());
        }
        if ( value.contains("keywords") && value["keywords"].is_string() )
            entry.m_keywords = value["keywords"].get_ref<const std::string&>();
        // 内置双语名称与近义词不会随 UI 语言切换消失。
        if ( value.contains("aliases") && value["aliases"].is_string() )
            entry.m_aliases = value["aliases"].get_ref<const std::string&>();
        m_entries.push_back(std::move(entry));
    }
}

/// @brief 输入或翻译版本变化时重建搜索缓存。
/// @details 目录原始顺序作为同分结果的次序，不读取设置值或执行任何设置回调。
/// 原始输入、归一化查询和语言版本分别缓存：
/// - 相同原始输入直接返回，不再分配大小写转换后的文本；
/// - 只改首尾空白或大小写时，保留原来的过滤结果；
/// - 语言变化重建可见名称和搜索字段，但保留条目身份；
/// - 空查询清空命中集合，普通设置浏览不变成全目录结果列表；
/// - 名称精确、前缀和子串命中排在仅别名或说明命中之前；
/// - 同分时保留目录顺序，避免输入期间候选无故交换位置。
/// Translate 返回翻译池视图，仅在本次低频重建中复制为索引拥有的文本。
/// 双语别名与当前皮肤覆写文案并存，不因切换界面语言丢失另一语言入口。
/// @warning UI 热路径常态帧只比较查询与版本，过滤和排序仅发生在缓存失效时。
void SettingsSearchIndex::update(std::string_view query,
                                 uint32_t         translationVersion,
                                 const Translate& translate)
{
    // 先比较原始输入的归一化形式，英文大小写和首尾空白不会重复重建结果。
    const bool languageChanged =
        !m_translated || m_translationVersion != translationVersion;
    if ( !languageChanged && query == m_input ) return;
    m_input        = query;
    auto nextQuery = fold(query);
    if ( !languageChanged && nextQuery == m_query ) return;
    m_query = std::move(nextQuery);
    if ( languageChanged ) {
        for ( auto& entry : m_entries ) {
            entry.m_label   = translate(entry.m_labelKey);
            entry.m_section = translate(entry.m_sectionKey);
            const auto category =
                std::find_if(CATEGORY_KEYS.begin(),
                             CATEGORY_KEYS.end(),
                             [&entry](const auto& item) {
                                 return item.first == entry.m_tab;
                             });
            entry.m_category    = translate(category->second);
            entry.m_foldedLabel = fold(entry.m_label);
            // 分类、分组、字段名及翻译键同样可搜，但只匹配名称时优先展示。
            entry.m_searchText =
                entry.m_foldedLabel + " " + fold(entry.m_section) + " " +
                fold(entry.m_category) + " " + fold(entry.m_keywords) + " " +
                fold(entry.m_labelKey) + " " + fold(entry.m_aliases);
            for ( const auto& key : entry.m_fieldKeys ) {
                entry.m_searchText += " ";
                entry.m_searchText += fold(translate(key));
            }
        }
        // 切换语言后一次更新所有行，目标索引和分类身份仍然保持不变。
        m_translationVersion = translationVersion;
        m_translated         = true;
    }
    m_results.clear();
    // 名称相同的项目级和软件级设置仍是两个独立候选。
    // 排序不能去重或改变所属分类，否则会定位到错误的配置作用域。
    if ( m_query.empty() ) return;
    // tokens 借用 m_query，本轮过滤完成前不得移动或修改查询字符串。
    // 命中缓存不捕获这些视图，后续修改查询不会产生悬空引用。
    const auto words = tokens(m_query);
    for ( std::size_t i = 0; i < m_entries.size(); ++i ) {
        const auto& entry = m_entries[i];
        // 每个词都必须命中；例如“视觉 偏移”能够跨分类和标签两个字段匹配。
        if ( !std::all_of(words.begin(), words.end(), [&entry](auto word) {
                 return entry.m_searchText.find(word) != std::string::npos;
             }) )
            continue;
        int score = 0;
        // 精确名称、名称前缀、名称子串依次优先于仅说明或选项命中。
        if ( entry.m_foldedLabel == m_query )
            score = 1000;
        else if ( entry.m_foldedLabel.starts_with(m_query) )
            score = 800;
        else if ( entry.m_foldedLabel.find(m_query) != std::string::npos )
            score = 600;
        for ( const auto word : words )
            if ( entry.m_foldedLabel.find(word) != std::string::npos )
                score += 100;
        m_results.push_back({ i, score });
    }
    // 稳定排序让同分结果按原分类和页面顺序展示，避免输入时列表无故跳动。
    std::stable_sort(m_results.begin(),
                     m_results.end(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.m_score > rhs.m_score;
                     });
}
}  // namespace MMM::UI
