#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <sol/sol.hpp>

namespace MMM
{
namespace Translation
{
Translator::Translator()
{
    XINFO("Initializing translations");
}

// 载入语言文件
void Translator::loadLanguage(const std::string& langLuaFile)
{
    std::filesystem::path path(langLuaFile);
    std::string           langID = path.stem().string();

    XINFO("Loading language: {}", langID);

    try {
        sol::state lua;
        lua.open_libraries(sol::lib::base, sol::lib::table);
        auto result = lua.script_file(langLuaFile);

        if ( !result.valid() ) return;

        sol::table langTable = result;
        Dictionary newDict;

        // 遍历 Lua 表，将 String Key 转换为 uint32 Hash 存入 Map
        for ( const auto& kv : langTable ) {
            std::string keyStr = kv.first.as<std::string>();
            std::string valStr = kv.second.as<std::string>();

            // 运行时计算 Hash (只在加载时发生一次)
            uint32_t keyHash = MMM::Hash::hash_str(keyStr);

            newDict[keyHash] = valStr;
        }

        m_Dictionarys[langID] = std::move(newDict);

        if ( m_currentDictionary == nullptr ) {
            switchLang(langID);
        }
    } catch ( const std::exception& e ) {
        XERROR("Error loading lang: {}", e.what());
    }
}

// 切换语言
bool Translator::switchLang(const std::string& langID)
{
    auto it = m_Dictionarys.find(langID);
    if ( it != m_Dictionarys.end() ) {
        m_currentDictionary = &(it->second);
        m_version++;  // 版本号 +1，通知所有宏更新缓存
        return true;
    }
    return false;
}

// 获取翻译器版本
uint32_t Translator::getVersion() const
{
    return m_version;
}

// 翻译
const char* Translator::translate(uint32_t keyHash, const char* fallbackStr)
{
    const char* resultStr = fallbackStr;

    if ( m_currentDictionary ) {
        // 整数查找，非常快
        auto it = m_currentDictionary->find(keyHash);
        if ( it != m_currentDictionary->end() ) {
            resultStr = it->second.c_str();
        }
    }

    // --- 核心修复：池化 ---
    // 为了防止字典切换或皮肤重载导致 c_str() 指针失效，
    // 我们将所有被 UI 访问过的字符串放入一个稳定的池中。
    // 只要是在池里的字符串，其地址在程序运行期间就是绝对稳定的。
    auto poolIt = m_stringPool.find(resultStr);
    if ( poolIt == m_stringPool.end() ) {
        // 插入池并获取稳定地址
        auto [insertedIt, success] = m_stringPool.insert(resultStr);
        return insertedIt->c_str();
    }

    return poolIt->c_str();
}

}  // namespace Translation

}  // namespace MMM
