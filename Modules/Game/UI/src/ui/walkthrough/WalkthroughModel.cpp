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
/// @param id 来自资源文件的主题、章节、分支或步骤 ID。
/// @return 非空且仅含白名单字符时为 true。
bool validId(const std::string& id)
{
    // 进度键以斜杠连接主题和步骤；禁止斜杠可避免键边界歧义。
    // 标识独立于本地化标题，切换语言不应改变已完成记录。
    return !id.empty() && id.size() <= 128 &&
           id.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789."
               "_-") == std::string::npos;
}
/// @brief 读取字符串字段，缺失或类型不匹配时返回空串。
/// @param object 已由上层确认类型的 JSON 对象。
/// @param name 字段名，不写回输入对象。
/// @return 字符串副本；必填字段由调用方进一步校验。
std::string field(const nlohmann::json& object, const char* name)
{
    const auto it = object.find(name);
    return it != object.end() && it->is_string() ? it->get<std::string>()
                                                 : std::string{};
}
/// @brief 读取有限非负整数顺序，缺失时保留默认值。
/// @param object 带可选 order 字段的章节或主题对象。
/// @param result 仅在字段存在且校验成功后更新。
/// @return 字段缺失或有效时为 true，非法值不使用静默截断。
bool readOrder(const nlohmann::json& object, int& result)
{
    const auto it = object.find("order");
    if ( it == object.end() ) return true;
    // 在转换为 int 前限定范围，避免超大 JSON 整数窄化。
    if ( !it->is_number_integer() || *it < 0 || *it > 100000 ) return false;
    result = it->get<int>();
    return true;
}
/// @brief 兼容单字符串与语言映射两种文本写法。
/// @param object 已通过结构检查的所属对象。
/// @param name 需要解析的标题、描述或正文字段。
/// @return 仅包含字符串值的翻译表；空表是否有效由字段用途决定。
Text text(const nlohmann::json& object, const char* name)
{
    Text       result;
    const auto it = object.find(name);
    if ( it == object.end() ) return result;
    // 简写文本归入默认英语键，与 Text::get 的回退顺序一致。
    if ( it->is_string() )
        result.m_translations["en_us"] = it->get<std::string>();
    else if ( it->is_object() )
        for ( const auto& [language, value] : it->items() ) {
            // 单项类型错误不丢弃其余语言；必填标题最终仍须有可用文本。
            if ( value.is_string() )
                result.m_translations[language] = value.get<std::string>();
        }
    return result;
}
/// @brief 读取可选字符串数组，错误类型由调用方拒绝。
/// @param object 尚在解析中的步骤对象。
/// @param name signals 或 requires，两种列表采用相同的长度约束。
/// @param result 接收追加值，调用方使用尚未发布的步骤对象。
/// @return 字段缺失视为空列表；超过上限或含非字符串时失败。
bool strings(const nlohmann::json& object, const char* name,
             std::vector<std::string>& result)
{
    const auto it = object.find(name);
    if ( it == object.end() ) return true;
    if ( !it->is_array() || it->size() > 128 ) return false;
    // 失败前可能已追加部分项；上层丢弃整个主题，不消费半成品。
    for ( const auto& value : *it ) {
        if ( !value.is_string() ) return false;
        result.push_back(value.get<std::string>());
    }
    return true;
}
}  // namespace

/// @brief 按请求语言、英语、首个翻译的顺序返回借用文本。
/// @param language 精确语言键，调用前由上层统一语言标识。
/// @return 引用属于翻译表；空表返回静态空串。
/// @warning 界面文本查询路径，不在此处加载语言资源或修改翻译表。
const std::string& Text::get(std::string_view language) const
{
    // 不在这里补写缺失翻译，界面查询不会改变资源数据。
    if ( auto it = m_translations.find(language); it != m_translations.end() )
        return it->second;
    if ( auto it = m_translations.find("en_us"); it != m_translations.end() )
        return it->second;
    static const std::string empty;
    // map 的首项按语言键排序，回退结果不依赖 JSON 中的字段顺序。
    return m_translations.empty() ? empty : m_translations.begin()->second;
}

/// @brief 解析章节目录并按展示顺序稳定排序。
/// @param input 目录 JSON 数组，允许空目录。
/// @return 全部有效的章节或首个错误，不返回部分章节。
/// 目录不包含主题清单，因此允许没有主题的章节预留导航位置。
/// @warning 资源加载路径会分配和排序，调用方应缓存结果。
std::expected<std::vector<Chapter>, std::string> parseChapters(
    std::string_view input)
{
    // 先限制原始文本，再解析；目录项数另外限制对象数量。
    if ( input.size() > 1024 * 1024 )
        return std::unexpected("章节目录超过 1 MiB");
    const auto json = nlohmann::json::parse(input, nullptr, false);
    // 禁用语法解析异常，损坏文本通过 discarded 类型进入此错误分支。
    if ( !json.is_array() || json.size() > 64 )
        return std::unexpected("章节目录必须为有限数组");
    std::vector<Chapter>  chapters;
    std::set<std::string> ids;
    for ( const auto& item : json ) {
        if ( !item.is_object() ) return std::unexpected("章节格式错误");
        Chapter chapter;
        chapter.m_id    = field(item, "id");
        chapter.m_title = text(item, "title");
        // ID 唯一性与标题一起验证，避免标签页指向不明确的章节。
        if ( !validId(chapter.m_id) || !ids.insert(chapter.m_id).second ||
             chapter.m_title.m_translations.empty() ||
             !readOrder(item, chapter.m_order) )
            return std::unexpected("章节 ID、标题或顺序无效");
        chapters.push_back(std::move(chapter));
    }
    // 相同 order 保留资产声明次序，作者无需为了平级章节强行编号。
    std::stable_sort(chapters.begin(),
                     chapters.end(),
                     [](const Chapter& a, const Chapter& b) {
                         return a.m_order < b.m_order;
                     });
    return chapters;
}

/// @brief 构造主题并验证步骤依赖图。
/// @param input 单个主题的 JSON 对象，最大 1 MiB。
/// @return 校验成功的值对象或中文错误，不修改学习记录。
/// 章节是否已在目录声明由上层组织资产，本函数只验证章节 ID 的格式。
/// @warning 低频加载路径；递归深度由主题内最多 128 个步骤约束。
std::expected<Topic, std::string> parseTopic(std::string_view input)
{
    // 校验只处理结构和引用；动作、信号的业务含义由服务层解释。
    // 解析期间只修改局部 topic，任意字段失败都整体放弃候选。
    if ( input.size() > 1024 * 1024 ) return std::unexpected("主题超过 1 MiB");
    const auto json = nlohmann::json::parse(input, nullptr, false);
    if ( !json.is_object() ) return std::unexpected("主题不是有效 JSON 对象");
    // 描述和正文允许为空，导航标题则要求至少一种翻译。
    Topic topic;
    topic.m_id          = field(json, "id");
    topic.m_title       = text(json, "title");
    topic.m_description = text(json, "description");
    if ( !validId(topic.m_id) || topic.m_title.m_translations.empty() )
        return std::unexpected("主题缺少有效 ID 或标题");
    if ( json.contains("chapter") ) {
        // 未声明章节的旧主题沿用 other；显式声明则必须给出合法 ID。
        topic.m_chapter = field(json, "chapter");
        if ( !validId(topic.m_chapter) ) return std::unexpected("无效章节 ID");
    }
    if ( !readOrder(json, topic.m_order) )
        return std::unexpected("无效主题顺序");
    // 占位标记必须是布尔值，避免字符串形式的 false 被误当作开启。
    if ( auto placeholder = json.find("placeholder");
         placeholder != json.end() ) {
        if ( !placeholder->is_boolean() )
            return std::unexpected("placeholder 必须为布尔值");
        topic.m_placeholder = placeholder->get<bool>();
    }
    // 内容版本用于描述修订，不参与步骤进度键，也不会清空旧进度。
    if ( auto version = json.find("version"); version != json.end() ) {
        if ( !version->is_number_integer() || *version < 1 ||
             *version > 100000 )
            return std::unexpected("无效内容版本");
        topic.m_version = version->get<int>();
    }
    // 学习阶段 order 不生成依赖；前置关系只来自步骤声明的 requires。
    const auto mode = field(json, "completion");
    // 主题完成模式归约分支；它与步骤信号的 match 模式相互独立。
    if ( !mode.empty() && mode != "any" && mode != "all" )
        return std::unexpected("completion 必须为 any 或 all");
    topic.m_anyBranch   = mode != "all";
    const auto branches = json.find("branches");
    if ( topic.m_placeholder ) {
        // 占位主题只提供导航入口，不允许携带尚不应执行的半成品分支。
        // 分支字段可以省略或给空数组，统一得到无步骤的主题。
        if ( branches != json.end() &&
             (!branches->is_array() || !branches->empty()) )
            return std::unexpected("占位主题不能包含操作分支");
        return topic;
    }
    if ( branches == json.end() || !branches->is_array() || branches->empty() ||
         branches->size() > 32 )
        return std::unexpected("主题需要 1 至 32 个分支");
    // 空分支不构成学习流程；需要仅保留入口时应使用 placeholder。
    std::set<std::string> branchIds, stepIds;
    // 分支分别编号，步骤却在主题内共用命名空间，支持跨分支依赖。
    // 分支与步骤仍保留数组声明次序；依赖验证不会重排用户看到的流程。
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
            // 上限统计全部分支的步骤，既限制解析成本也限制后续图遍历。
            if ( !entry.is_object() || stepIds.size() >= 128 )
                return std::unexpected("步骤格式错误或数量过多");
            Step step;
            step.m_id     = field(entry, "id");
            step.m_title  = text(entry, "title");
            step.m_body   = text(entry, "body");
            step.m_action = field(entry, "action");
            // ID 决定持久化身份；同一主题内不能用重复 ID 表示不同步骤。
            if ( !validId(step.m_id) || !stepIds.insert(step.m_id).second ||
                 step.m_title.m_translations.empty() )
                return std::unexpected("步骤 ID 重复或标题缺失");
            if ( !strings(entry, "signals", step.m_signals) ||
                 !strings(entry, "requires", step.m_prerequisites) )
                return std::unexpected("步骤信号或前置列表错误");
            // 列表类型已确认，前置 ID 的存在性留到完整索引建立后验证。
            const auto match = field(entry, "match");
            // 没有 match 时采用任一信号完成；空信号列表留给手动确认。
            if ( !match.empty() && match != "all" && match != "any" )
                return std::unexpected("match 必须为 any 或 all");
            step.m_allSignals = match == "all";
            branch.m_steps.push_back(std::move(step));
        }
        topic.m_branches.push_back(std::move(branch));
    }
    // 所有 vector 填充完后才取元素地址，之后验证期间不再扩容或移动。
    // 建完整索引再解析 requires，使前置步骤无需在文件中先出现。
    std::map<std::string, const Step*> index;
    for ( const auto& branch : topic.m_branches )
        for ( const auto& step : branch.m_steps )
            index.emplace(step.m_id, &step);
    // 三色标记：0 未访问、1 在当前递归栈、2 已确认无环。
    // 已完成节点可复用结论，共享前置步骤不会被误判为环。
    std::map<std::string, int>       marks;
    std::function<bool(const Step&)> visit = [&](const Step& step) {
        auto& mark = marks[step.m_id];
        // 回到递归栈中的节点才表示循环，包括直接依赖自身的情况。
        if ( mark == 1 ) return false;
        if ( mark == 2 ) return true;
        mark = 1;
        for ( const auto& id : step.m_prerequisites ) {
            // 缺失引用与循环都使整个主题无效，不偷偷忽略前置限制。
            auto it = index.find(id);
            if ( it == index.end() || !visit(*it->second) ) return false;
        }
        // 此时全部前置已验证，缓存的是整条依赖链的无环结论。
        mark = 2;
        return true;
    };
    // 从每个节点启动，覆盖没有被其他步骤引用的独立分支。
    for ( const auto& [id, step] : index )
        if ( !visit(*step) ) return std::unexpected("步骤引用不存在或构成循环");
    return topic;
}

/// @brief 查询持久化完成来源，不创建缺失记录。
/// @param topic 提供记录命名空间，不要求主题的所有步骤均已完成。
/// @param step 提供稳定步骤 ID，标题和顺序不参与查找。
/// @return manual、automatic 或空视图；reset/restore 后应重新查询。
std::string_view Progress::source(const Topic& topic, const Step& step) const
{
    // 不包含版本或分支 ID：主题重排与文案修订不会丢失稳定步骤的进度。
    const auto it = m_records.find(topic.m_id + "/" + step.m_id);
    return it == m_records.end() ? std::string_view{} : it->second;
}
/// @brief 将两种完成来源统一为步骤完成状态。
/// @param topic 所属主题，隔离不同主题的同名步骤。
/// @param step 被查询的步骤，不根据当前可用性反向撤销已完成结果。
/// @return 手动了解与实操完成都算完成，不要求再次发送业务信号。
bool Progress::completed(const Topic& topic, const Step& step) const
{
    return !source(topic, step).empty();
}
/// @brief 记录用户主动确认了解。
/// @param topic 当前学习主题，记录写入此主题的命名空间。
/// @param step 用户确认的步骤，调用方负责传入所属主题中的定义。
/// @return 首次写入时为 true，重复确认不触发持久化更新。
bool Progress::acknowledge(const Topic& topic, const Step& step)
{
    // 已有实操来源保持不变；手动确认也不检查前置步骤，允许跳读。
    if ( completed(topic, step) ) return false;
    m_records[topic.m_id + "/" + step.m_id] = "manual";
    return true;
}
/// @brief 检查主题内前置记录，供操作入口和自动完成共用。
/// @param topic 前置步骤查找的主题范围，不跨主题解析依赖。
/// @param step 已通过主题解析校验的步骤定义。
/// @return 空前置列表天然可用；只影响操作，不限制阅读或手动确认。
bool Progress::available(const Topic& topic, const Step& step) const
{
    return std::all_of(step.m_prerequisites.begin(),
                       step.m_prerequisites.end(),
                       [&](const auto& id) {
                           return m_records.contains(topic.m_id + "/" + id);
                       });
}
/// @brief 累积相关信号并把可完成步骤归约到稳定状态。
/// @param topic 提供步骤与依赖定义，不从持久化记录推测业务规则。
/// @param value 空值表示只重算，不添加新实操信号。
/// @return 学习记录是否改变；只新增易失信号时仍返回 false。
/// @warning 业务事件路径会更新容器并多轮遍历，不适合每帧调用。
bool Progress::signal(const Topic& topic, std::string_view value)
{
    // 信号按主题隔离，同名业务事件不能直接借用另一主题的实操历史。
    auto& signals  = m_signals[topic.m_id];
    bool  relevant = false;
    for ( const auto& branch : topic.m_branches )
        for ( const auto& step : branch.m_steps )
            if ( std::find(step.m_signals.begin(),
                           step.m_signals.end(),
                           value) != step.m_signals.end() )
                relevant = true;
    // 空值是主动重算请求，不当作资产中的可观察业务事件。
    if ( !relevant && !value.empty() ) return false;
    // 无关事件不保存；相关事件去重，重复通知不会无限增长记录。
    if ( !value.empty() &&
         std::find(signals.begin(), signals.end(), value) == signals.end() )
        signals.emplace_back(value);
    bool changed = false, passChanged;
    // 文件顺序未必是拓扑序；前置步骤本轮完成后需要再推进它的依赖者。
    // 分支目标达成不终止归约，其他分支仍可继续学习并记录完成来源。
    // 每次有效轮次至少新增一条完成记录，有限步骤保证循环最终停止。
    do {
        passChanged = false;
        for ( const auto& branch : topic.m_branches )
            for ( const auto& step : branch.m_steps ) {
                if ( completed(topic, step) || !available(topic, step) ||
                     step.m_signals.empty() )
                    continue;
                // 信号可先于前置步骤到达；前置完成后消费已经积累的信号。
                // 空信号步骤已跳过，避免 all_of 对空集合直接判为完成。
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
    // 完成记录单调增加；撤销进度只由显式 reset 或 restore 处理。
    return changed;
}
/// @brief 重置单个主题，为重新演练建立干净状态。
/// @param topic 按 ID 清理，包括当前资产中已不再声明的旧步骤。
/// 不删除其他主题或重新解析资源；调用方负责保存重置后的记录。
void Progress::reset(const Topic& topic)
{
    // 包含斜杠的完整前缀，避免重置 foo 时误删 foobar 的步骤。
    const auto prefix = topic.m_id + "/";
    std::erase_if(m_records, [&](const auto& pair) {
        return pair.first.starts_with(prefix);
    });
    // 同时清理易失信号，避免下一次重算立刻恢复旧的自动完成状态。
    m_signals.erase(topic.m_id);
}
/// @brief 生成版本化的学习记录 JSON。
/// @return 带 schema 和 steps 的 UTF-8 JSON 文本，由调用方负责写文件。
/// 仅保存完成结果；进程重启后必须重新实操才能取得尚未完成步骤的信号。
std::string Progress::serialize() const
{
    return nlohmann::json{ { "schema", 1 }, { "steps", m_records } }.dump(2);
}
/// @brief 校验并替换学习记录，损坏输入保持当前状态。
/// @param input schema 为 1 的记录对象，最大 1 MiB。
/// @return 完整恢复成功时为 true，不支持的格式由调用方选择后续处理。
/// 恢复采用替换而非合并语义，成功读取空 steps 会清空现有学习记录。
bool Progress::restore(std::string_view input)
{
    // 与主题输入同样限制文本体积；这里不依赖主题已经加载。
    // 恢复只处理存储格式，不发业务事件，也不重新执行已完成步骤的操作。
    if ( input.size() > 1024 * 1024 ) return false;
    const auto json = nlohmann::json::parse(input, nullptr, false);
    if ( !json.is_object() || !json.contains("schema") || json["schema"] != 1 ||
         !json.contains("steps") || !json["steps"].is_object() )
        return false;
    // 先构造候选，任一来源值非法都不能覆盖当前有效记录。
    std::map<std::string, std::string, std::less<>> records;
    // 步骤键按原样保留，不要求当前主题资源已加载。
    for ( const auto& [key, value] : json["steps"].items() ) {
        // 来源只允许两种语义，未知字符串不能被 completed 误认作成功。
        if ( !value.is_string() || (value != "manual" && value != "automatic") )
            return false;
        records[key] = value.get<std::string>();
    }
    // 保留未知步骤键，使暂时缺失的主题或新版资产重新出现时可恢复进度。
    m_records = std::move(records);
    // 已完成来源可以恢复，未归约的实操事件不跨加载边界沿用。
    m_signals.clear();
    return true;
}
}  // namespace MMM::UI::Walkthrough
