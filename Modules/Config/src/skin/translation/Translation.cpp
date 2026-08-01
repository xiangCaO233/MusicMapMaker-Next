#include "config/skin/translation/Translation.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"

#include <sol/sol.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MMM
{
namespace Translation
{
/// @brief 翻译器私有并发字典、指针缓存与稳定字符串池。
struct Translator::Impl {
    /// @brief 单个语言字典。
    using Dictionary = std::unordered_map<uint32_t, std::string>;

    /// @brief 保护语言字典、当前字典、指针缓存和稳定字符串池。
    mutable std::shared_mutex mutex;

    /// @brief 翻译缓存版本。
    /// @warning UI 与逻辑线程并发读取，语言加载线程低频写入；原子用于避免
    /// `TR_CACHE` 版本检查的数据竞争。
    std::atomic<uint32_t> version{ 0 };

    /// @brief 按语言 ID 保存的全部字典。
    std::unordered_map<std::string, Dictionary> dictionaries;

    /// @brief 当前语言字典观察指针。
    Dictionary* currentDictionary{ nullptr };

    /// @brief 翻译键哈希到稳定字符串地址的热路径缓存。
    std::unordered_map<uint32_t, const char*> pointerCache;

    /// @brief 保证已返回字符串地址在翻译器生命周期内稳定的字符串池。
    std::unordered_set<std::string> stringPool;
};

Translator::Translator() : m_impl(std::make_unique<Impl>())
{
    XINFO("Initializing translations");
}

Translator::~Translator() = default;

Translator& getActiveTranslator()
{
    return Config::SkinManager::instance().getTranslator();
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

    sol::table       langTable = resultObject.as<sol::table>();
    Impl::Dictionary newDict;

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

    std::unique_lock lock(m_impl->mutex);
    auto&            dictionary = m_impl->dictionaries[langID];
    const bool       wasCurrent = m_impl->currentDictionary == &dictionary;
    dictionary                  = std::move(newDict);
    if ( m_impl->currentDictionary == nullptr || wasCurrent ) {
        m_impl->currentDictionary = &dictionary;
        m_impl->pointerCache.clear();
        m_impl->version.fetch_add(1, std::memory_order_release);
    }
}

// 切换语言
bool Translator::switchLang(const std::string& langID)
{
    std::unique_lock lock(m_impl->mutex);
    auto             it = m_impl->dictionaries.find(langID);
    if ( it != m_impl->dictionaries.end() ) {
        m_impl->currentDictionary = &(it->second);
        m_impl->pointerCache.clear();  // 重要：字典切换，旧的指针缓存必须清空
        m_impl->version.fetch_add(1, std::memory_order_release);
        return true;
    }
    return false;
}

/// @brief 清空已加载语言和指针缓存，用于皮肤热切换。
void Translator::clear()
{
    std::unique_lock lock(m_impl->mutex);
    m_impl->dictionaries.clear();
    m_impl->currentDictionary = nullptr;
    m_impl->pointerCache.clear();
    m_impl->version.fetch_add(1, std::memory_order_release);
}

// 获取翻译器版本
uint32_t Translator::getVersion() const
{
    return m_impl->version.load(std::memory_order_acquire);
}

// 翻译
const char* Translator::translate(uint32_t keyHash, const char* fallbackStr)
{
    // --- 极速路径：指针缓存 (uint32_t 查找) ---
    {
        std::shared_lock lock(m_impl->mutex);
        const auto       pointerIt = m_impl->pointerCache.find(keyHash);
        if ( pointerIt != m_impl->pointerCache.end() ) {
            return pointerIt->second;
        }
    }

    // 首次出现的键进入独占路径，并在获取锁后再次检查并发填充结果。
    std::unique_lock lock(m_impl->mutex);
    const auto       pointerIt = m_impl->pointerCache.find(keyHash);
    if ( pointerIt != m_impl->pointerCache.end() ) {
        return pointerIt->second;
    }

    // --- 正常路径：字典查找 ---
    const char* resultStr = fallbackStr ? fallbackStr : "";
    if ( m_impl->currentDictionary ) {
        auto it = m_impl->currentDictionary->find(keyHash);
        if ( it != m_impl->currentDictionary->end() ) {
            resultStr = it->second.c_str();
        }
    }

    // --- 稳定化处理：池化 ---
    // 只要是在池里的字符串，其地址在程序运行期间就是绝对稳定的。
    const auto  poolIt    = m_impl->stringPool.insert(resultStr).first;
    const char* stablePtr = poolIt->c_str();

    // 存入加速缓存
    m_impl->pointerCache[keyHash] = stablePtr;

    return stablePtr;
}

}  // namespace Translation

}  // namespace MMM
