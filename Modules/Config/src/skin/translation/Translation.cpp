#include "config/skin/translation/Translation.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <fstream>
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
    std::filesystem::path path   = Config::utf8ToPath(langLuaFile);
    std::string           langID = Config::pathToUtf8(path.stem());

    XINFO("Loading language: {}", langID);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table);

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if ( !file ) {
        XERROR("Failed to open lang file: {}", langLuaFile);
        return;
    }
    std::string script((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    auto        result =
        lua.safe_script(script, sol::script_pass_on_error, langLuaFile);

    if ( !result.valid() ) {
        sol::error err = result;
        XERROR("Error loading lang: {}", err.what());
        return;
    }

    sol::object resultObject = result;
    if ( !resultObject.is<sol::table>() ) {
        XERROR("Language file did not return a table: {}", langLuaFile);
        return;
    }

    sol::table langTable = resultObject.as<sol::table>();
    Dictionary newDict;

    // 遍历 Lua 表，将字符串键转换为 uint32 Hash 存入 Map。
    for ( const auto& kv : langTable ) {
        if ( !kv.first.is<std::string>() || !kv.second.is<std::string>() ) {
            continue;
        }

        std::string keyStr = kv.first.as<std::string>();
        std::string valStr = kv.second.as<std::string>();

        // 运行时计算 Hash，只在加载时发生一次。
        uint32_t keyHash = MMM::Hash::hash_str(keyStr);

        newDict[keyHash] = valStr;
    }

    m_Dictionarys[langID] = std::move(newDict);

    if ( m_currentDictionary == nullptr ) {
        switchLang(langID);
    }
}

// 切换语言
bool Translator::switchLang(const std::string& langID)
{
    auto it = m_Dictionarys.find(langID);
    if ( it != m_Dictionarys.end() ) {
        m_currentDictionary = &(it->second);
        m_version++;             // 版本号 +1，通知所有宏更新缓存
        m_pointerCache.clear();  // 重要：字典切换，旧的指针缓存必须清空
        return true;
    }
    return false;
}

/// @brief 清空已加载语言和指针缓存，用于皮肤热切换。
void Translator::clear()
{
    m_Dictionarys.clear();
    m_currentDictionary = nullptr;
    m_pointerCache.clear();
    m_stringPool.clear();
    ++m_version;
}

// 获取翻译器版本
uint32_t Translator::getVersion() const
{
    return m_version;
}

// 翻译
const char* Translator::translate(uint32_t keyHash, const char* fallbackStr)
{
    // --- 极速路径：指针缓存 (uint32_t 查找) ---
    auto it_ptr = m_pointerCache.find(keyHash);
    if ( it_ptr != m_pointerCache.end() ) {
        return it_ptr->second;
    }

    // --- 正常路径：字典查找 ---
    const char* resultStr = fallbackStr;
    if ( m_currentDictionary ) {
        auto it = m_currentDictionary->find(keyHash);
        if ( it != m_currentDictionary->end() ) {
            resultStr = it->second.c_str();
        }
    }

    // --- 稳定化处理：池化 ---
    // 只要是在池里的字符串，其地址在程序运行期间就是绝对稳定的。
    auto [poolIt, success] = m_stringPool.insert(resultStr);
    const char* stablePtr  = poolIt->c_str();

    // 存入加速缓存
    m_pointerCache[keyHash] = stablePtr;

    return stablePtr;
}

}  // namespace Translation

}  // namespace MMM
