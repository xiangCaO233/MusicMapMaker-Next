#include "ui/walkthrough/WalkthroughModel.h"
#include <algorithm>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>

namespace MMM::UI::Walkthrough
{
namespace
{
/// @brief 只接受有限长度且不含路径分隔符的稳定标识。
bool validId(const std::string& id)
{
    return !id.empty() && id.size() <= 128 &&
           id.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789."
               "_-") == std::string::npos;
}
/// @brief 无异常读取字符串字段。
std::string field(const nlohmann::json& object, const char* name)
{
    const auto it = object.find(name);
    return it != object.end() && it->is_string() ? it->get<std::string>()
                                                 : std::string{};
}
/// @brief 无异常读取有限非负整数顺序，缺失时保留默认值。
bool readOrder(const nlohmann::json& object, int& result)
{
    const auto it = object.find("order");
    if ( it == object.end() ) return true;
    if ( !it->is_number_integer() || *it < 0 || *it > 100000 ) return false;
    result = it->get<int>();
    return true;
}
/// @brief 无异常读取本地化文本。
Text text(const nlohmann::json& object, const char* name)
{
    Text       result;
    const auto it = object.find(name);
    if ( it == object.end() ) return result;
    if ( it->is_string() )
        result.m_translations["en_us"] = it->get<std::string>();
    else if ( it->is_object() )
        for ( const auto& [language, value] : it->items() ) {
            if ( value.is_string() )
                result.m_translations[language] = value.get<std::string>();
        }
    return result;
}
/// @brief 无异常读取字符串数组，错误类型由调用方拒绝。
bool strings(const nlohmann::json& object, const char* name,
             std::vector<std::string>& result)
{
    const auto it = object.find(name);
    if ( it == object.end() ) return true;
    if ( !it->is_array() || it->size() > 128 ) return false;
    for ( const auto& value : *it ) {
        if ( !value.is_string() ) return false;
        result.push_back(value.get<std::string>());
    }
    return true;
}
}  // namespace

const std::string& Text::get(std::string_view language) const
{
    if ( auto it = m_translations.find(language); it != m_translations.end() )
        return it->second;
    if ( auto it = m_translations.find("en_us"); it != m_translations.end() )
        return it->second;
    static const std::string empty;
    return m_translations.empty() ? empty : m_translations.begin()->second;
}

std::expected<std::vector<Chapter>, std::string> parseChapters(
    std::string_view input)
{
    if ( input.size() > 1024 * 1024 )
        return std::unexpected("章节目录超过 1 MiB");
    const auto json = nlohmann::json::parse(input, nullptr, false);
    if ( !json.is_array() || json.size() > 64 )
        return std::unexpected("章节目录必须为有限数组");
    std::vector<Chapter>  chapters;
    std::set<std::string> ids;
    for ( const auto& item : json ) {
        if ( !item.is_object() ) return std::unexpected("章节格式错误");
        Chapter chapter;
        chapter.m_id    = field(item, "id");
        chapter.m_title = text(item, "title");
        if ( !validId(chapter.m_id) || !ids.insert(chapter.m_id).second ||
             chapter.m_title.m_translations.empty() ||
             !readOrder(item, chapter.m_order) )
            return std::unexpected("章节 ID、标题或顺序无效");
        chapters.push_back(std::move(chapter));
    }
    std::stable_sort(chapters.begin(),
                     chapters.end(),
                     [](const Chapter& a, const Chapter& b) {
                         return a.m_order < b.m_order;
                     });
    return chapters;
}

std::expected<Topic, std::string> parseTopic(std::string_view input)
{
    if ( input.size() > 1024 * 1024 ) return std::unexpected("主题超过 1 MiB");
    const auto json = nlohmann::json::parse(input, nullptr, false);
    if ( !json.is_object() ) return std::unexpected("主题不是有效 JSON 对象");
    Topic topic;
    topic.m_id          = field(json, "id");
    topic.m_title       = text(json, "title");
    topic.m_description = text(json, "description");
    if ( !validId(topic.m_id) || topic.m_title.m_translations.empty() )
        return std::unexpected("主题缺少有效 ID 或标题");
    if ( json.contains("chapter") ) {
        topic.m_chapter = field(json, "chapter");
        if ( !validId(topic.m_chapter) ) return std::unexpected("无效章节 ID");
    }
    if ( !readOrder(json, topic.m_order) )
        return std::unexpected("无效主题顺序");
    if ( auto placeholder = json.find("placeholder");
         placeholder != json.end() ) {
        if ( !placeholder->is_boolean() )
            return std::unexpected("placeholder 必须为布尔值");
        topic.m_placeholder = placeholder->get<bool>();
    }
    if ( auto version = json.find("version"); version != json.end() ) {
        if ( !version->is_number_integer() || *version < 1 ||
             *version > 100000 )
            return std::unexpected("无效内容版本");
        topic.m_version = version->get<int>();
    }
    const auto mode = field(json, "completion");
    if ( !mode.empty() && mode != "any" && mode != "all" )
        return std::unexpected("completion 必须为 any 或 all");
    topic.m_anyBranch   = mode != "all";
    const auto branches = json.find("branches");
    if ( topic.m_placeholder ) {
        if ( branches != json.end() &&
             (!branches->is_array() || !branches->empty()) )
            return std::unexpected("占位主题不能包含操作分支");
        return topic;
    }
    if ( branches == json.end() || !branches->is_array() || branches->empty() ||
         branches->size() > 32 )
        return std::unexpected("主题需要 1 至 32 个分支");
    std::set<std::string> branchIds, stepIds;
    for ( const auto& item : *branches ) {
        if ( !item.is_object() ) return std::unexpected("分支格式错误");
        Branch branch;
        branch.m_id    = field(item, "id");
        branch.m_title = text(item, "title");
        if ( !validId(branch.m_id) || !branchIds.insert(branch.m_id).second ||
             branch.m_title.m_translations.empty() )
            return std::unexpected("分支 ID 重复或标题缺失");
        auto steps = item.find("steps");
        if ( steps == item.end() || !steps->is_array() || steps->empty() )
            return std::unexpected("分支需要步骤");
        for ( const auto& entry : *steps ) {
            if ( !entry.is_object() || stepIds.size() >= 128 )
                return std::unexpected("步骤格式错误或数量过多");
            Step step;
            step.m_id     = field(entry, "id");
            step.m_title  = text(entry, "title");
            step.m_body   = text(entry, "body");
            step.m_action = field(entry, "action");
            if ( !validId(step.m_id) || !stepIds.insert(step.m_id).second ||
                 step.m_title.m_translations.empty() )
                return std::unexpected("步骤 ID 重复或标题缺失");
            if ( !strings(entry, "signals", step.m_signals) ||
                 !strings(entry, "requires", step.m_prerequisites) )
                return std::unexpected("步骤信号或前置列表错误");
            const auto match = field(entry, "match");
            if ( !match.empty() && match != "all" && match != "any" )
                return std::unexpected("match 必须为 any 或 all");
            step.m_allSignals = match == "all";
            branch.m_steps.push_back(std::move(step));
        }
        topic.m_branches.push_back(std::move(branch));
    }
    std::map<std::string, const Step*> index;
    for ( const auto& branch : topic.m_branches )
        for ( const auto& step : branch.m_steps )
            index.emplace(step.m_id, &step);
    std::map<std::string, int>       marks;
    std::function<bool(const Step&)> visit = [&](const Step& step) {
        auto& mark = marks[step.m_id];
        if ( mark == 1 ) return false;
        if ( mark == 2 ) return true;
        mark = 1;
        for ( const auto& id : step.m_prerequisites ) {
            auto it = index.find(id);
            if ( it == index.end() || !visit(*it->second) ) return false;
        }
        mark = 2;
        return true;
    };
    for ( const auto& [id, step] : index )
        if ( !visit(*step) ) return std::unexpected("步骤引用不存在或构成循环");
    return topic;
}

std::string_view Progress::source(const Topic& topic, const Step& step) const
{
    const auto it = m_records.find(topic.m_id + "/" + step.m_id);
    return it == m_records.end() ? std::string_view{} : it->second;
}
bool Progress::completed(const Topic& topic, const Step& step) const
{
    return !source(topic, step).empty();
}
bool Progress::acknowledge(const Topic& topic, const Step& step)
{
    if ( completed(topic, step) ) return false;
    m_records[topic.m_id + "/" + step.m_id] = "manual";
    return true;
}
bool Progress::available(const Topic& topic, const Step& step) const
{
    return std::all_of(step.m_prerequisites.begin(),
                       step.m_prerequisites.end(),
                       [&](const auto& id) {
                           return m_records.contains(topic.m_id + "/" + id);
                       });
}
bool Progress::signal(const Topic& topic, std::string_view value)
{
    auto& signals  = m_signals[topic.m_id];
    bool  relevant = false;
    for ( const auto& branch : topic.m_branches )
        for ( const auto& step : branch.m_steps )
            if ( std::find(step.m_signals.begin(),
                           step.m_signals.end(),
                           value) != step.m_signals.end() )
                relevant = true;
    if ( !relevant && !value.empty() ) return false;
    if ( !value.empty() &&
         std::find(signals.begin(), signals.end(), value) == signals.end() )
        signals.emplace_back(value);
    bool changed = false, passChanged;
    do {
        passChanged = false;
        for ( const auto& branch : topic.m_branches )
            for ( const auto& step : branch.m_steps ) {
                if ( completed(topic, step) || !available(topic, step) ||
                     step.m_signals.empty() )
                    continue;
                const auto seen = [&](const auto& name) {
                    return std::find(signals.begin(), signals.end(), name) !=
                           signals.end();
                };
                if ( step.m_allSignals ? std::all_of(step.m_signals.begin(),
                                                     step.m_signals.end(),
                                                     seen)
                                       : std::any_of(step.m_signals.begin(),
                                                     step.m_signals.end(),
                                                     seen) ) {
                    m_records[topic.m_id + "/" + step.m_id] = "automatic";
                    changed = passChanged = true;
                }
            }
    } while ( passChanged );
    return changed;
}
void Progress::reset(const Topic& topic)
{
    const auto prefix = topic.m_id + "/";
    std::erase_if(m_records, [&](const auto& pair) {
        return pair.first.starts_with(prefix);
    });
    m_signals.erase(topic.m_id);
}
std::string Progress::serialize() const
{
    return nlohmann::json{ { "schema", 1 }, { "steps", m_records } }.dump(2);
}
bool Progress::restore(std::string_view input)
{
    if ( input.size() > 1024 * 1024 ) return false;
    const auto json = nlohmann::json::parse(input, nullptr, false);
    if ( !json.is_object() || !json.contains("schema") || json["schema"] != 1 ||
         !json.contains("steps") || !json["steps"].is_object() )
        return false;
    std::map<std::string, std::string, std::less<>> records;
    for ( const auto& [key, value] : json["steps"].items() ) {
        if ( !value.is_string() || (value != "manual" && value != "automatic") )
            return false;
        records[key] = value.get<std::string>();
    }
    m_records = std::move(records);
    m_signals.clear();
    return true;
}
}  // namespace MMM::UI::Walkthrough
