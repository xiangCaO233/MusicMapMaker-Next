#include "translation/Translation.h"
#include "colorful-log.h"
#include <filesystem>
#include <sol/sol.hpp>

namespace MMM
{
namespace Translation
{
Translator& Translator::instance()
{
    static Translator translator;
    return translator;
}

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

    try
    {
        sol::state lua;
        lua.open_libraries(sol::lib::base, sol::lib::table);
        auto result = lua.script_file(langLuaFile);

        if ( !result.valid() ) return;

        sol::table langTable = result;
        Dictionary newDict;

        // 遍历 Lua 表，将 String Key 转换为 uint32 Hash 存入 Map
        for ( const auto& kv : langTable )
        {
            std::string keyStr = kv.first.as<std::string>();
            std::string valStr = kv.second.as<std::string>();

            // 运行时计算 Hash (只在加载时发生一次)
            uint32_t keyHash = MMM::Hash::hash_str(keyStr);

            newDict[keyHash] = valStr;
        }

        m_Dictionarys[langID] = std::move(newDict);

        if ( m_currentDictionary == nullptr )
        {
            switchLang(langID);
        }
    }
    catch ( const std::exception& e )
    {
        XERROR("Error loading lang: {}", e.what());
    }
}

// 切换语言
bool Translator::switchLang(const std::string& langID)
{
    auto it = m_Dictionarys.find(langID);
    if ( it != m_Dictionarys.end() )
    {
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
const std::string& Translator::translate(uint32_t    keyHash,
                                         const char* fallbackStr)
{
    // 如果字典还没加载，或者找不到，返回原文的包装
    // 需要一个静态 string 来存储 fallback，以返回引用
    static std::string fallback;

    if ( m_currentDictionary )
    {
        // 整数查找，非常快
        auto it = m_currentDictionary->find(keyHash);
        if ( it != m_currentDictionary->end() )
        {
            return it->second;
        }
    }

    // 找不到时，返回 fallbackStr
    // 为了安全返回引用，需要赋值给静态变量
    // (非线程安全，但 UI 线程通常是单线程)
    fallback = fallbackStr;
    return fallback;
}

}  // namespace Translation

}  // namespace MMM
