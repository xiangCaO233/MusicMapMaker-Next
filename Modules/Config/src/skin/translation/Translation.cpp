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
    // 对外路径始终采用 UTF-8，集中转换后再交给本机文件系统接口。
    const std::filesystem::path path = Config::utf8ToPath(langLuaFile);

    // 每次解析使用独立 Lua 状态，避免翻译文件污染皮肤脚本的全局环境。
    sol::state lua;
    // 翻译仅需要基础语法和 table，限制可用库可缩小脚本副作用范围。
    lua.open_libraries(sol::lib::base, sol::lib::table);

    // 二进制模式禁止平台换行转换改变 Lua 源码的字节表示。
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if ( !file ) {
        XERROR("Failed to open lang file: {}", langLuaFile);
        return false;
    }
    // 文件规模较小且只在加载阶段读取，完整脚本用于 sol2 安全执行入口。
    std::string script((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    // 错误作为返回对象传播，符合项目禁止 C++ 异常的失败处理约束。
    auto result =
        lua.safe_script(script, sol::script_pass_on_error, langLuaFile);

    if ( !result.valid() ) {
        sol::error err = result;
        XERROR("Error loading lang: {}", err.what());
        return false;
    }

    // 语言文件契约要求顶层表达式返回 table，其他结果均不可合并。
    sol::object resultObject = result;
    if ( !resultObject.is<sol::table>() ) {
        XERROR("Language file did not return a table: {}", langLuaFile);
        return false;
    }

    sol::table langTable = resultObject.as<sol::table>();
    // 只有完整解析成功后才清空输出，调用方不会收到半成品字典。
    entries.clear();
    entries.reserve(langTable.size());
    for ( const auto& kv : langTable ) {
        // 非字符串扩展字段不属于翻译契约，忽略而不阻断兼容文件加载。
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

/// @brief 创建拥有独立字典、缓存与稳定字符串池的翻译器。
/// @note 初始状态不包含语言，首次成功加载的字典会自动成为当前语言。
Translator::Translator() : m_impl(std::make_unique<Impl>())
{
    // 具体容器隐藏在实现对象中，减少高扇出公共头的重编译范围。
    XINFO("Initializing translations");
}

/// @brief 释放翻译器及其全部稳定字符串。
/// @warning 销毁前必须确保外部不再持有由本翻译器返回的 TRResult。
Translator::~Translator() = default;

/// @brief 在独占锁保护下取得内容唯一且地址稳定的字符串视图。
/// @param value 待池化的 UTF-8 文本。
/// @return 指向字符串池节点的非拥有视图。
std::string_view Translator::internStringLocked(std::string_view value)
{
    // 调用方持有独占锁，因此可直接执行异构查找而无需再次同步。
    const auto existing = m_impl->stringPool.find(value);
    if ( existing != m_impl->stringPool.end() ) {
        // 返回池内节点视图，重复译文共享同一份稳定存储。
        return { existing->data(), existing->size() };
    }

    // unordered_set 以节点保存字符串，rehash 不会使元素引用失效。
    const auto [inserted, wasInserted] = m_impl->stringPool.emplace(value);
    if ( wasInserted ) {
        // 统计仅累计唯一文本，便于观察皮肤热切换后的常驻容量。
        m_impl->stableStringBytes += inserted->size();
    }
    return { inserted->data(), inserted->size() };
}

/// @brief 在独占锁保护下按当前字典重建哈希指针缓存。
/// @warning 会遍历当前语言全部字段，仅允许低频发布路径调用。
void Translator::rebuildPointerCacheLocked()
{
    // 语言切换后旧哈希映射不可继续命中，必须先完整清空。
    m_impl->pointerCache.clear();
    if ( !m_impl->currentDictionary ) return;

    // 一次预留避免重建期间反复扩容；该路径只在低频发布阶段执行。
    m_impl->pointerCache.reserve(m_impl->currentDictionary->size());
    for ( const auto& [keyHash, value] : *m_impl->currentDictionary ) {
        m_impl->pointerCache.emplace(keyHash, value);
    }
}

/// @brief 获取当前皮肤管理器所持有的活动翻译器。
/// @return 生命周期由 SkinManager 管理的非拥有引用。
/// @warning 该入口位于 UI 热路径，不得加入分配、文件访问或阻塞操作。
Translator& getActiveTranslator()
{
    // 翻译器由当前 SkinManager 持有，入口本身不复制所有权。
    return Config::SkinManager::instance().getTranslator();
}

/// @brief 解析并原子发布一个默认语言字典。
/// @param langID 字典的稳定语言标识。
/// @param langLuaFile 返回字符串 table 的 Lua 文件路径。
/// @return 文件有效且字典完整发布时返回 true。
/// @warning Lua 解析在锁外进行，发布阶段会短暂独占所有翻译容器。
bool Translator::loadLanguage(const std::string& langID,
                              const std::string& langLuaFile)
{
    XINFO("Loading language: {}", langID);

    // 文件读取和 Lua 执行在锁外完成，避免阻塞并发界面翻译查询。
    std::vector<std::pair<std::string, std::string>> entries;
    if ( langID.empty() || !parseLanguageFile(langLuaFile, entries) ) {
        return false;
    }

    // 保留原始字段名，皮肤覆写不能只凭哈希判断未知字段。
    std::unordered_set<std::string> newFieldNames;
    newFieldNames.reserve(entries.size());
    for ( const auto& [key, value] : entries ) {
        // 此轮只收集键；译文在取得锁后统一进入稳定字符串池。
        static_cast<void>(value);
        newFieldNames.insert(key);
    }

    // 发布阶段独占全部共享容器，读线程只会看到旧字典或完整新字典。
    std::unique_lock lock(m_impl->mutex);
    // 预留节点数量降低池化大量翻译时的 rehash 次数。
    m_impl->stringPool.reserve(m_impl->stringPool.size() + entries.size());
    Impl::Dictionary newDictionary;
    newDictionary.reserve(entries.size());
    for ( const auto& [key, value] : entries ) {
        // 运行期仅以稳定哈希查找，字符串键不进入热路径字典。
        newDictionary[MMM::Hash::hashString(key)] = internStringLocked(value);
    }

    // 先取得目标节点地址，用于判断被替换字典是否正是当前语言。
    auto&      dictionary = m_impl->dictionaries[langID];
    const bool wasCurrent = m_impl->currentDictionary == &dictionary;
    // 移动完整临时字典保证不会逐项暴露中间发布状态。
    dictionary                        = std::move(newDictionary);
    m_impl->defaultFieldNames[langID] = std::move(newFieldNames);
    if ( m_impl->currentDictionary == nullptr || wasCurrent ) {
        // 首次加载自动激活；重载当前语言时同步刷新全部指针缓存。
        m_impl->currentDictionary = &dictionary;
        rebuildPointerCacheLocked();
        // release 将字典和缓存更新发布给 TR_CACHE 的 acquire 读取。
        m_impl->version.fetch_add(1, std::memory_order_release);
    }
    return true;
}

/// @brief 把皮肤译文合并到既有默认字段集合。
/// @param langID 已加载的默认语言标识。
/// @param langLuaFile 皮肤提供的覆写 Lua 文件。
/// @return 解析状态及被拒绝的未知字段列表。
/// @warning 合并阶段会独占翻译容器，当前语言变化时还会重建热缓存。
LanguageOverrideResult Translator::applyLanguageOverride(
    const std::string& langID, const std::string& langLuaFile)
{
    LanguageOverrideResult                           result;
    std::vector<std::pair<std::string, std::string>> entries;
    // 仍在锁外解析覆写脚本，解析失败与“成功但无有效字段”明确区分。
    if ( !parseLanguageFile(langLuaFile, entries) ) return result;
    result.loaded = true;

    std::unique_lock lock(m_impl->mutex);
    // 覆写必须依附已加载的默认字典，不能自行创建不完整语言。
    const auto dictionaryIt = m_impl->dictionaries.find(langID);
    const auto fieldNamesIt = m_impl->defaultFieldNames.find(langID);
    if ( dictionaryIt == m_impl->dictionaries.end() ||
         fieldNamesIt == m_impl->defaultFieldNames.end() ) {
        // 缺少默认字段集合时把所有键报告为未知，但解析本身仍算成功。
        result.unknownFields.reserve(entries.size());
        for ( const auto& [key, value] : entries ) {
            static_cast<void>(value);
            result.unknownFields.push_back(key);
        }
        // 排序并去重使启动提示确定，避免 Lua table 遍历顺序泄漏到 UI。
        std::sort(result.unknownFields.begin(), result.unknownFields.end());
        result.unknownFields.erase(std::unique(result.unknownFields.begin(),
                                               result.unknownFields.end()),
                                   result.unknownFields.end());
        return result;
    }

    // 只有至少一个合法字段生效时才需要重建当前语言缓存和递增版本。
    bool changed = false;
    for ( auto& [key, value] : entries ) {
        if ( !fieldNamesIt->second.contains(key) ) {
            // 拒绝默认字典之外的字段，防止拼写错误静默成为新接口。
            result.unknownFields.push_back(key);
            continue;
        }
        // 新译文先进入稳定池，替换字典视图不会悬空既有 TRResult。
        dictionaryIt->second[MMM::Hash::hashString(key)] =
            internStringLocked(value);
        changed = true;
    }

    // 对混合合法与未知字段执行同样的确定性报告整理。
    std::sort(result.unknownFields.begin(), result.unknownFields.end());
    result.unknownFields.erase(
        std::unique(result.unknownFields.begin(), result.unknownFields.end()),
        result.unknownFields.end());
    if ( changed && m_impl->currentDictionary == &dictionaryIt->second ) {
        // 非当前语言可延迟到切换时重建；当前语言必须立即刷新热缓存。
        rebuildPointerCacheLocked();
        m_impl->version.fetch_add(1, std::memory_order_release);
    }
    return result;
}

/// @brief 切换当前活动语言并重建热路径指针缓存。
/// @param langID 已加载语言的稳定标识。
/// @return 目标语言存在并成功发布时返回 true。
/// @warning 低频设置路径会独占字典并完整遍历目标语言字段。
bool Translator::switchLang(const std::string& langID)
{
    // 查找与指针发布处于同一独占区间，避免容器变更使观察指针失效。
    std::unique_lock lock(m_impl->mutex);
    auto             it = m_impl->dictionaries.find(langID);
    if ( it != m_impl->dictionaries.end() ) {
        // currentDictionary 观察 unordered_map 节点；节点生命周期由容器保证。
        m_impl->currentDictionary = &(it->second);
        rebuildPointerCacheLocked();
        m_impl->version.fetch_add(1, std::memory_order_release);
        return true;
    }
    // 切换失败保持原语言和版本不变，调用方可以继续显示已有译文。
    return false;
}

/// @brief 清空已加载语言和指针缓存，用于皮肤热切换。
void Translator::clear()
{
    // 清除字典时独占所有查询，防止读线程访问即将释放的字典节点。
    std::unique_lock lock(m_impl->mutex);
    m_impl->dictionaries.clear();
    m_impl->defaultFieldNames.clear();
    m_impl->currentDictionary = nullptr;
    // 稳定字符串池刻意保留，使此前返回给 UI 的 TRResult 继续有效。
    m_impl->pointerCache.clear();
    m_impl->version.fetch_add(1, std::memory_order_release);
}

/// @brief 读取当前翻译缓存版本。
/// @return 与最近一次缓存发布对应的单调递增版本值。
/// @warning UI 热路径调用，仅执行 acquire 原子读取，不获取容器锁。
uint32_t Translator::getVersion() const
{
    // 与发布端 release 配对，版本变化后可见其之前完成的缓存更新。
    return m_impl->version.load(std::memory_order_acquire);
}

/// @brief 获取语言字典、热缓存和稳定池的诊断快照。
/// @return 同一共享锁时间点下采集的统计数据。
/// @warning 诊断路径会读取多个容器大小，不应加入每帧 UI 更新。
TranslationCacheStats Translator::getCacheStats() const
{
    // 共享锁保证字典指针及各容器统计来自一致的发布状态。
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

/// @brief 查询翻译并为命中或回退文本提供稳定地址。
/// @param keyHash 调用方预先计算的稳定字段哈希。
/// @param fallbackStr 未找到字段时显示的空字符结尾文本。
/// @return 指向翻译器稳定字符串池的轻量结果。
/// @warning UI 与逻辑线程热路径：命中仅共享锁查找，未命中才独占池化。
TRResult Translator::translate(uint64_t keyHash, const char* fallbackStr)
{
    // 热路径优先只持共享锁执行一次 uint64_t 查找，不分配或复制字符串。
    {
        std::shared_lock lock(m_impl->mutex);
        const auto       pointerIt = m_impl->pointerCache.find(keyHash);
        if ( pointerIt != m_impl->pointerCache.end() ) {
            return TRResult(pointerIt->second);
        }
    }

    // 首次出现的键进入独占路径，并在获取锁后再次检查并发填充结果。
    // 双重检查避免多个线程为同一哈希重复池化并覆盖缓存。
    std::unique_lock lock(m_impl->mutex);
    const auto       pointerIt = m_impl->pointerCache.find(keyHash);
    if ( pointerIt != m_impl->pointerCache.end() ) {
        return TRResult(pointerIt->second);
    }

    // 字典缺失时保留调用方键名作为回退，确保界面仍有可识别文本。
    std::string_view result =
        fallbackStr ? std::string_view(fallbackStr) : std::string_view();
    if ( m_impl->currentDictionary ) {
        // 当前字典只保存稳定池视图，命中结果无需再次复制。
        auto it = m_impl->currentDictionary->find(keyHash);
        if ( it != m_impl->currentDictionary->end() ) {
            result = it->second;
        }
    }

    // 即使回退参数来自临时调用点，也先池化后再返回长期有效视图。
    const std::string_view stableResult = internStringLocked(result);
    // 缺失键同样缓存，避免每帧重复进入独占锁路径。
    m_impl->pointerCache.emplace(keyHash, stableResult);
    return TRResult(stableResult);
}

}  // namespace Translation

}  // namespace MMM
