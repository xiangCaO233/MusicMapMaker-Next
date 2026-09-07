#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::UI::Walkthrough
{
/// @brief 演练文本按语言标识提供翻译，缺失时回退英语或首个文本。
struct Text {
    /// @brief 语言与 Markdown 文本。
    std::map<std::string, std::string, std::less<>> m_translations;
    /// @brief 查询文本，不分配、不访问文件。
    const std::string& get(std::string_view language) const;
};
/// @brief 一个可单独确认了解的演练步骤。
struct Step {
    std::string              m_id;       ///< 稳定步骤标识。
    Text                     m_title;    ///< 步骤标题。
    Text                     m_body;     ///< Markdown 正文。
    std::vector<std::string> m_signals;  ///< 自动完成所需信号。
    bool m_allSignals{ false };  ///< true 要求全部信号，否则任一信号即可。
    std::vector<std::string>
                m_prerequisites;  ///< 自动完成和操作入口的前置步骤。
    std::string m_action;         ///< 已注册的操作 ID，空值表示没有操作。
};
/// @brief 同一目标的一条独立操作分支。
struct Branch {
    std::string       m_id;     ///< 稳定分支标识。
    Text              m_title;  ///< 分支标题。
    std::vector<Step> m_steps;  ///< 有序步骤，可独立手动确认。
};
/// @brief 可由数据文件扩展的演练主题。
struct Topic {
    std::string m_id;            ///< 稳定主题标识。
    int         m_version{ 1 };  ///< 内容修订版本，稳定步骤 ID 保留学习记录。
    Text        m_title;         ///< 主题标题。
    Text        m_description;   ///< 主题说明。
    bool m_anyBranch{ true };  ///< true 表示任一分支完成目标，false 要求全部。
    std::vector<Branch> m_branches;  ///< 操作分支。
};
/// @brief 解析并验证主题，拒绝重复标识、无效引用、循环前置依赖和超大输入。
std::expected<Topic, std::string> parseTopic(std::string_view json);
/// @brief 与窗口生命周期无关的学习记录及纯状态归约器。
class Progress
{
public:
    /// @brief 查询步骤是否已了解或已实操完成。
    bool completed(const Topic& topic, const Step& step) const;
    /// @brief 查询步骤完成来源，空字符串表示尚未完成。
    std::string_view source(const Topic& topic, const Step& step) const;
    /// @brief 任意步骤均允许独立手动确认，不受前置步骤限制。
    bool acknowledge(const Topic& topic, const Step& step);
    /// @brief 根据已注册业务信号推进自动完成，返回是否改变持久化进度。
    bool signal(const Topic& topic, std::string_view signal);
    /// @brief 清除此主题的学习和本次实操信号，不影响其他主题。
    void reset(const Topic& topic);
    /// @brief 检查前置步骤，仅用于操作和自动完成，不限制阅读。
    bool available(const Topic& topic, const Step& step) const;
    /// @brief 序列化学习记录，不保存易失实操信号。
    std::string serialize() const;
    /// @brief 恢复合法学习记录；损坏输入不会清空已有状态。
    bool restore(std::string_view json);

private:
    /// @brief 主题/步骤对应 manual 或 automatic，保留未知步骤以支持升级。
    std::map<std::string, std::string, std::less<>> m_records;
    /// @brief 当前进程见过的信号，主题间隔离。
    std::map<std::string, std::vector<std::string>, std::less<>> m_signals;
};
}  // namespace MMM::UI::Walkthrough
