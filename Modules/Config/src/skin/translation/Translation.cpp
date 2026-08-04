#include "config/skin/translation/Translation.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"

#include <sol/sol.hpp>

#include <algorithm>
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
namespace
{
/// @brief 为稳定字符串池提供支持 string_view 异构查找的哈希器。
struct TransparentStringHash {
    using is_transparent = void;

    /// @brief 计算字符串视图哈希。
    /// @param value 待计算文本。
    /// @return 与字符串重载一致的哈希值。
    std::size_t operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }

    /// @brief 计算拥有型字符串哈希。
    /// @param value 待计算文本。
    /// @return 与字符串视图重载一致的哈希值。
    std::size_t operator()(const std::string& value) const noexcept
    {
        return (*this)(std::string_view(value));
    }

    /// @brief 计算空字符结尾字符串哈希。
    /// @param value 待计算文本。
    /// @return 与字符串视图重载一致的哈希值。
    std::size_t operator()(const char* value) const noexcept
    {
        return (*this)(std::string_view(value ? value : ""));
    }
};

/// @brief 为稳定字符串池提供支持 string_view 异构查找的比较器。
struct TransparentStringEqual {
    using is_transparent = void;

    /// @brief 比较两个字符串视图。
    /// @param left 左侧文本。
    /// @param right 右侧文本。
    /// @return 文本完全一致时返回 true。
    bool operator()(std::string_view left,
                    std::string_view right) const noexcept
    {
        return left == right;
    }
};

/// @brief 从 Lua 文件解析字符串键值翻译项。
/// @param langLuaFile Lua 翻译文件路径。
/// @param entries 成功时接收翻译字段及文本。
/// @return 文件成功读取且返回 Lua table 时返回 true。
bool parseLanguageFile(
    const std::string&                                langLuaFile,
    std::vector<std::pair<std::string, std::string>>& entries)
{
    const std::filesystem::path path = Config::utf8ToPath(langLuaFile);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table);

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if ( !file ) {
        XERROR("Failed to open lang file: {}", langLuaFile);
        return false;
    }
    std::string script((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    auto        result =
        lua.safe_script(script, sol::script_pass_on_error, langLuaFile);

    if ( !result.valid() ) {
        sol::error err = result;
        XERROR("Error loading lang: {}", err.what());
        return false;
    }

    sol::object resultObject = result;
    if ( !resultObject.is<sol::table>() ) {
        XERROR("Language file did not return a table: {}", langLuaFile);
        return false;
    }

    sol::table langTable = resultObject.as<sol::table>();
    entries.clear();
    entries.reserve(langTable.size());
    for ( const auto& kv : langTable ) {
        if ( !kv.first.is<std::string>() || !kv.second.is<std::string>() ) {
            continue;
        }
        entries.emplace_back(kv.first.as<std::string>(),
                             kv.second.as<std::string>());
    }
    return true;
}
}  // namespace

/// @brief 翻译器私有并发字典、指针缓存与稳定字符串池。
struct Translator::Impl {
    /// @brief 单个语言字典，译文直接引用稳定字符串池以避免重复保存内容。
    using Dictionary = std::unordered_map<uint64_t, std::string_view>;

    /// @brief 保护语言字典、当前字典、指针缓存和稳定字符串池。
    mutable std::shared_mutex mutex;

    /// @brief 翻译缓存版本。
    /// @warning UI 与逻辑线程并发读取，语言加载线程低频写入；原子用于避免
    /// `TR_CACHE` 版本检查的数据竞争。
    std::atomic<uint32_t> version{ 0 };

    /// @brief 按语言 ID 保存的全部字典。
    std::unordered_map<std::string, Dictionary> dictionaries;

    /// @brief 按语言 ID 保存默认字典的原始字段名，用于精确校验皮肤覆写。
    std::unordered_map<std::string, std::unordered_set<std::string>>
        defaultFieldNames;

    /// @brief 当前语言字典观察指针。
    Dictionary* currentDictionary{ nullptr };

    /// @brief 翻译键哈希到稳定字符串地址的热路径缓存。
    std::unordered_map<uint64_t, std::string_view> pointerCache;

    /// @brief 保证已返回字符串地址在翻译器生命周期内稳定的字符串池。
    std::unordered_set<std::string, TransparentStringHash,
                       TransparentStringEqual>
        stringPool;

    /// @brief 稳定字符串池保存的文本总字节数，不包含容器节点开销。
    std::size_t stableStringBytes{ 0 };
};

Translator::Translator() : m_impl(std::make_unique<Impl>())
{
    XINFO("Initializing translations");
}

Translator::~Translator() = default;

std::string_view Translator::internStringLocked(std::string_view value)
{
    const auto existing = m_impl->stringPool.find(value);
    if ( existing != m_impl->stringPool.end() ) {
        return { existing->data(), existing->size() };
    }

    const auto [inserted, wasInserted] = m_impl->stringPool.emplace(value);
    if ( wasInserted ) {
        m_impl->stableStringBytes += inserted->size();
    }
    return { inserted->data(), inserted->size() };
}

void Translator::rebuildPointerCacheLocked()
{
    m_impl->pointerCache.clear();
    if ( !m_impl->currentDictionary ) return;

    m_impl->pointerCache.reserve(m_impl->currentDictionary->size());
    for ( const auto& [keyHash, value] : *m_impl->currentDictionary ) {
        m_impl->pointerCache.emplace(keyHash, value);
    }
}

Translator& getActiveTranslator()
{
    return Config::SkinManager::instance().getTranslator();
}

bool Translator::loadLanguage(const std::string& langID,
                              const std::string& langLuaFile)
{
    XINFO("Loading language: {}", langID);

    std::vector<std::pair<std::string, std::string>> entries;
    if ( langID.empty() || !parseLanguageFile(langLuaFile, entries) ) {
        return false;
    }

    std::unordered_set<std::string> newFieldNames;
    newFieldNames.reserve(entries.size());
    for ( const auto& [key, value] : entries ) {
        static_cast<void>(value);
        newFieldNames.insert(key);
    }

    std::unique_lock lock(m_impl->mutex);
    m_impl->stringPool.reserve(m_impl->stringPool.size() + entries.size());
    Impl::Dictionary newDictionary;
    newDictionary.reserve(entries.size());
    for ( const auto& [key, value] : entries ) {
        newDictionary[MMM::Hash::hashString(key)] = internStringLocked(value);
    }

    auto&      dictionary = m_impl->dictionaries[langID];
    const bool wasCurrent = m_impl->currentDictionary == &dictionary;
    dictionary            = std::move(newDictionary);
    m_impl->defaultFieldNames[langID] = std::move(newFieldNames);
    if ( m_impl->currentDictionary == nullptr || wasCurrent ) {
        m_impl->currentDictionary = &dictionary;
        rebuildPointerCacheLocked();
        m_impl->version.fetch_add(1, std::memory_order_release);
    }
    return true;
}

LanguageOverrideResult Translator::applyLanguageOverride(
    const std::string& langID, const std::string& langLuaFile)
{
    LanguageOverrideResult                           result;
    std::vector<std::pair<std::string, std::string>> entries;
    if ( !parseLanguageFile(langLuaFile, entries) ) return result;
    result.loaded = true;

    std::unique_lock lock(m_impl->mutex);
    const auto       dictionaryIt = m_impl->dictionaries.find(langID);
    const auto       fieldNamesIt = m_impl->defaultFieldNames.find(langID);
    if ( dictionaryIt == m_impl->dictionaries.end() ||
         fieldNamesIt == m_impl->defaultFieldNames.end() ) {
        result.unknownFields.reserve(entries.size());
        for ( const auto& [key, value] : entries ) {
            static_cast<void>(value);
            result.unknownFields.push_back(key);
        }
        std::sort(result.unknownFields.begin(), result.unknownFields.end());
        result.unknownFields.erase(std::unique(result.unknownFields.begin(),
                                               result.unknownFields.end()),
                                   result.unknownFields.end());
        return result;
    }

    bool changed = false;
    for ( auto& [key, value] : entries ) {
        if ( !fieldNamesIt->second.contains(key) ) {
            result.unknownFields.push_back(key);
            continue;
        }
        dictionaryIt->second[MMM::Hash::hashString(key)] =
            internStringLocked(value);
        changed = true;
    }

    std::sort(result.unknownFields.begin(), result.unknownFields.end());
    result.unknownFields.erase(
        std::unique(result.unknownFields.begin(), result.unknownFields.end()),
        result.unknownFields.end());
    if ( changed && m_impl->currentDictionary == &dictionaryIt->second ) {
        rebuildPointerCacheLocked();
        m_impl->version.fetch_add(1, std::memory_order_release);
    }
    return result;
}

// 切换语言
bool Translator::switchLang(const std::string& langID)
{
    std::unique_lock lock(m_impl->mutex);
    auto             it = m_impl->dictionaries.find(langID);
    if ( it != m_impl->dictionaries.end() ) {
        m_impl->currentDictionary = &(it->second);
        rebuildPointerCacheLocked();
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
    m_impl->defaultFieldNames.clear();
    m_impl->currentDictionary = nullptr;
    m_impl->pointerCache.clear();
    m_impl->version.fetch_add(1, std::memory_order_release);
}

// 获取翻译器版本
uint32_t Translator::getVersion() const
{
    return m_impl->version.load(std::memory_order_acquire);
}

TranslationCacheStats Translator::getCacheStats() const
{
    std::shared_lock      lock(m_impl->mutex);
    TranslationCacheStats stats;
    stats.dictionaryCount = m_impl->dictionaries.size();
    stats.activeDictionaryEntryCount =
        m_impl->currentDictionary ? m_impl->currentDictionary->size() : 0;
    stats.pointerCacheEntryCount = m_impl->pointerCache.size();
    stats.stableStringCount      = m_impl->stringPool.size();
    stats.stableStringBytes      = m_impl->stableStringBytes;
    stats.version = m_impl->version.load(std::memory_order_acquire);
    return stats;
}

TRResult Translator::translate(uint64_t keyHash, const char* fallbackStr)
{
    // --- 极速路径：指针缓存 (uint64_t 查找) ---
    {
        std::shared_lock lock(m_impl->mutex);
        const auto       pointerIt = m_impl->pointerCache.find(keyHash);
        if ( pointerIt != m_impl->pointerCache.end() ) {
            return TRResult(pointerIt->second);
        }
    }

    // 首次出现的键进入独占路径，并在获取锁后再次检查并发填充结果。
    std::unique_lock lock(m_impl->mutex);
    const auto       pointerIt = m_impl->pointerCache.find(keyHash);
    if ( pointerIt != m_impl->pointerCache.end() ) {
        return TRResult(pointerIt->second);
    }

    // --- 正常路径：字典查找 ---
    std::string_view result =
        fallbackStr ? std::string_view(fallbackStr) : std::string_view();
    if ( m_impl->currentDictionary ) {
        auto it = m_impl->currentDictionary->find(keyHash);
        if ( it != m_impl->currentDictionary->end() ) {
            result = it->second;
        }
    }

    const std::string_view stableResult = internStringLocked(result);
    m_impl->pointerCache.emplace(keyHash, stableResult);
    return TRResult(stableResult);
}

}  // namespace Translation

}  // namespace MMM
