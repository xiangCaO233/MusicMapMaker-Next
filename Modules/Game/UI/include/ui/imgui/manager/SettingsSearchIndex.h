#pragma once

#include "event/ui/UISettingsTabEvent.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::UI
{
/// @brief 设置搜索目录中的稳定行身份与当前语言下的展示缓存。
/// @details 身份由分类、分组键和标签键组成，不能仅靠本地化标签去重。
struct SettingsSearchEntry {
    /// @brief 此设置实际绘制所在的分类，不根据标签键前缀推断。
    Event::SettingsTab m_tab{ Event::SettingsTab::Software };
    /// @brief 可折叠分组的翻译键；快捷键页使用分类标题。
    std::string m_sectionKey;
    /// @brief 设置行的标签翻译键，也是导航时的匹配依据。
    std::string m_labelKey;
    /// @brief 可选项、说明及提示的翻译键，不单独形成搜索结果。
    std::vector<std::string> m_fieldKeys;
    /// @brief 原设置的配置字段名，允许通过英文标识查找。
    std::string m_keywords;
    /// @brief 内置中英文名称及人工维护的近义词，独立于当前界面语言。
    std::string m_aliases;
    /// @brief 当前语言的设置项名称。
    std::string m_label;
    /// @brief 当前语言的所属分组。
    std::string m_section;
    /// @brief 当前语言的所属分类。
    std::string m_category;
    /// @brief ASCII 小写化后的名称，供精确命中优先排序。
    std::string m_foldedLabel;
    /// @brief 所有可搜索字段的归一化文本；字段之间保留分隔空白。
    std::string m_searchText;
};

/// @brief 一条搜索命中；只保存稳定目录索引，不复制设置定义。
struct SettingsSearchResult {
    /// @brief 原始目录条目索引，语言变化时仍保持身份。
    std::size_t m_index{ 0 };
    /// @brief 名称匹配优先于说明和选项匹配，分数越大越靠前。
    int m_score{ 0 };
};

/// @brief 设置搜索的纯数据索引，不访问配置、会话、文件系统或 ImGui。
/// @details 全分类目录嵌入程序，运行时不执行隐藏设置页以收集控件。
class SettingsSearchIndex
{
public:
    /// @brief 翻译查询回调，只在语言版本变化时调用。
    using Translate = std::function<std::string_view(std::string_view)>;

    /// @brief 从编译期嵌入的目录创建稳定条目，保留原设置顺序。
    SettingsSearchIndex();

    /// @brief 更新语言与查询；仅输入或翻译版本变化才重新过滤排序。
    /// @param query 支持空白分隔的多个关键词，全部关键词必须命中。
    /// @param translationVersion 翻译器发布的版本，用于使文本缓存失效。
    /// @param translate 查询当前语言文本的回调。
    /// @warning UI 热路径仅比较缓存键；重建由用户输入或语言切换触发。
    void update(std::string_view query, uint32_t translationVersion,
                const Translate& translate);

    /// @brief 返回当前语言下的目录，用于显示结果及定位真实设置行。
    const std::vector<SettingsSearchEntry>& entries() const
    {
        return m_entries;
    }
    /// @brief 返回按匹配程度排序的命中缓存，空查询不会列出全部设置。
    const std::vector<SettingsSearchResult>& results() const
    {
        return m_results;
    }
    /// @brief 判断查询是否有非空白内容，空白输入等同于退出搜索。
    bool hasQuery() const { return !m_query.empty(); }

private:
    /// @brief 不含运行态设置值的稳定目录。
    std::vector<SettingsSearchEntry> m_entries;
    /// @brief 当前命中缓存，常态帧只读取。
    std::vector<SettingsSearchResult> m_results;
    /// @brief 裁切首尾空白且 ASCII 小写化的查询。
    std::string m_query;
    /// @brief 上次原始输入，常态帧直接比较以避免重复归一化和分配。
    std::string m_input;
    /// @brief 缓存对应的翻译版本。
    uint32_t m_translationVersion{ 0 };
    /// @brief 首次查询也需要生成本地化文本，不能把版本零误认为已缓存。
    bool m_translated{ false };
};
}  // namespace MMM::UI
