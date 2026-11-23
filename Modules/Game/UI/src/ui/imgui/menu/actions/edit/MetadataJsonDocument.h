#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace MMM::UI
{

/// @brief 解析 JSON 元数据，语法错误通过 discarded 值返回。
[[nodiscard]] inline nlohmann::json parseJsonNoThrow(const char* text)
{
    return nlohmann::json::parse(text, nullptr, false);
}

/// @brief 按元数据字段的现有格式输出缩进文本。
[[nodiscard]] inline std::string dumpPrettyJson(const nlohmann::json& value)
{
    return value.dump(4);
}

/// @brief JSON 弹窗的编辑文本和已解析值，生命周期由 UI 线程独占。
/// @details 原始输入变化后才解析；结构编辑直接修改值，再同步生成文本。
/// 语法错误时仍保留编辑内容，调用方不得把 discarded 值提交给谱面。
class MetadataJsonDocument
{
public:
    /// @brief 打开字段并同步解析；格式化由调用方决定是否执行。
    /// @param text 输入内容按值接管，解析失败时仍可继续编辑。
    void replaceText(std::string text)
    {
        m_text = std::move(text);
        acceptTextEdit();
    }

    /// @brief 借给输入控件的缓冲；发生编辑后须先调用 acceptTextEdit 再读取值。
    /// @return 只允许 UI 控件在本次编辑期间写入的字符串引用。
    [[nodiscard]] std::string& text() { return m_text; }

    /// @brief 读取缓存值。
    /// @warning UI 热路径：只借用 DOM，不解析、不复制。
    [[nodiscard]] const nlohmann::json& value() const { return m_value; }

    /// @brief 接收本帧实际发生的文本编辑，同步语法状态及之后的提交值。
    /// @warning 只由输入控件返回修改时调用；空闲帧禁止重复解析。
    void acceptTextEdit() { m_value = parseJsonNoThrow(m_text.c_str()); }

    /// @brief 规范化有效值的文本，不二次解析也不改变其 JSON 类型。
    /// @warning 仅在用户请求格式化或结构修改时生成字符串，禁止逐帧调用。
    void format()
    {
        if ( !m_value.is_discarded() ) m_text = dumpPrettyJson(m_value);
    }

    /// @brief 将尚未提交的文档重建为可添加字段的空对象。
    void resetObject()
    {
        m_value = nlohmann::json::object();
        format();
    }

    /// @brief 删除对象字段并同步文本。
    /// @pre 当前值已是对象，由子字段表保证。
    void eraseField(const std::string& key)
    {
        m_value.erase(key);
        format();
    }

    /// @brief 添加或覆盖字段，非对象值按既有弹窗行为重建为对象。
    void setField(const std::string& key, nlohmann::json value)
    {
        // 用户明确执行结构编辑后，旧标量与非法文本均不再作为提交内容。
        if ( !m_value.is_object() ) m_value = nlohmann::json::object();
        m_value[key] = std::move(value);
        format();
    }

private:
    /// @brief 原始文本与格式化视图共用的缓冲。
    std::string m_text;
    /// @brief 最近一次文本编辑或结构操作产生的值。
    nlohmann::json m_value;
};

}  // namespace MMM::UI
