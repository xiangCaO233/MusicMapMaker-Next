#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace MMM
{
namespace Hash
{
/// @brief 使用 64 位 FNV-1a 计算稳定翻译键哈希。
/// @param str 待计算的 UTF-8 翻译键。
/// @return 与平台无关的 64 位哈希值。
constexpr uint64_t hashString(std::string_view str)
{
    uint64_t hash = 14695981039346656037ULL;
    for ( char c : str ) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}
}  // namespace Hash

namespace Translation
{
/// @brief 指向翻译器稳定字符串池的零拥有文本引用。
class TRResult
{
public:
    /// @brief 构造指向空字符串的翻译引用。
    TRResult() noexcept : m_view("") {}

    /// @brief 构造包含已知长度的翻译引用，不扫描字符串内容。
    /// @param view 生命周期由翻译器稳定字符串池保证的文本视图。
    explicit TRResult(std::string_view view) noexcept : m_view(view) {}

    /// @brief 获取以空字符结尾的 UTF-8 字符串地址。
    /// @return 翻译器生命周期内保持有效的字符串地址。
    [[nodiscard]] const char* data() const noexcept { return m_view.data(); }

    /// @brief 获取不拥有文本的字符串视图。
    /// @return 包含稳定地址和已知长度的字符串视图。
    [[nodiscard]] std::string_view view() const noexcept { return m_view; }

    /// @brief 显式创建拥有翻译文本的字符串副本。
    /// @return 独立拥有内容的字符串。
    [[nodiscard]] std::string toString() const { return std::string(m_view); }

    /// @brief 获取翻译文本字节数。
    /// @return UTF-8 文本的字节数。
    [[nodiscard]] std::size_t size() const noexcept { return m_view.size(); }

    /// @brief 判断翻译文本是否为空。
    /// @return 文本为空时返回 true。
    [[nodiscard]] bool empty() const noexcept { return m_view.empty(); }

private:
    /// @brief 指向稳定字符串池的文本视图。
    std::string_view m_view;
};

/// @brief 为 fmt 提供零复制的翻译文本格式化视图。
/// @param result 待格式化的翻译引用。
/// @return 不拥有内容的字符串视图。
inline std::string_view format_as(const TRResult& result) noexcept
{
    return result.view();
}

/// @brief 皮肤翻译覆写文件的合并结果。
struct LanguageOverrideResult {
    /// @brief 覆写文件是否成功解析。
    bool loaded{ false };

    /// @brief 默认语言字典中不存在、因而被忽略的字段名。
    std::vector<std::string> unknownFields;
};

/// @brief 翻译缓存和稳定字符串池的只读统计快照。
struct TranslationCacheStats {
    /// @brief 已加载语言字典数量。
    std::size_t dictionaryCount{ 0 };

    /// @brief 当前语言字典字段数量。
    std::size_t activeDictionaryEntryCount{ 0 };

    /// @brief 当前语言指针缓存字段数量。
    std::size_t pointerCacheEntryCount{ 0 };

    /// @brief 稳定字符串池中的唯一文本数量。
    std::size_t stableStringCount{ 0 };

    /// @brief 稳定字符串池保存的文本总字节数，不包含容器节点开销。
    std::size_t stableStringBytes{ 0 };

    /// @brief 统计快照对应的翻译版本。
    uint32_t version{ 0 };
};

class Translator
{
public:
    Translator();
    Translator(Translator&&)                 = delete;
    Translator(const Translator&)            = delete;
    Translator& operator=(Translator&&)      = delete;
    Translator& operator=(const Translator&) = delete;
    ~Translator();

    /// @brief 载入一个 Lua 语言字典。
    /// @param langID 默认语言字典的稳定标识。
    /// @param langLuaFile 语言文件路径。
    /// @return 文件成功解析并发布时返回 true。
    /// @warning 解析在锁外执行，发布字典时短暂持有独占锁。
    bool loadLanguage(const std::string& langID,
                      const std::string& langLuaFile);

    /// @brief 将皮肤翻译覆写合并到已加载的默认语言字典。
    /// @param langID 要覆写的默认语言标识。
    /// @param langLuaFile 皮肤覆写文件路径。
    /// @return 文件解析状态及默认字典中不存在的字段。
    /// @warning 解析在锁外执行，合并字典时短暂持有独占锁；未知字段不会
    /// 插入默认字典。
    LanguageOverrideResult applyLanguageOverride(
        const std::string& langID, const std::string& langLuaFile);

    /// @brief 切换当前语言。
    /// @param langID 语言文件名对应的稳定标识。
    /// @return 找到并切换成功时返回 true。
    /// @warning 低频配置路径：会独占翻译缓存并使版本递增。
    bool switchLang(const std::string& langID);

    /// @brief 清空已加载语言和指针缓存，用于皮肤热切换。
    /// @warning 低频皮肤重载路径：会独占翻译缓存；已返回字符串继续由稳定池
    /// 保活。
    void clear();

    /// @brief 获取翻译器版本。
    /// @return 最近一次语言切换或清空后的版本。
    /// @warning UI 与逻辑线程热路径会读取该原子版本；写入只发生在低频语言
    /// 切换路径，使用 acquire/release 保证缓存更新顺序。
    uint32_t getVersion() const;

    /// @brief 获取指定键的稳定翻译引用。
    /// @param keyHash 翻译键哈希。
    /// @param fallbackStr 缺少翻译时使用的回退文本。
    /// @return 包含稳定 UTF-8 字符串地址和已知长度的零拥有引用。
    /// @warning UI 与逻辑线程热路径会并发调用；缓存命中只持有共享读锁，
    /// 未命中才进入独占写锁并池化字符串。
    TRResult translate(uint64_t keyHash, const char* fallbackStr);

    /// @brief 获取翻译缓存和稳定字符串池的只读统计快照。
    /// @return 在共享锁保护下采集的缓存统计。
    /// @warning 该接口用于诊断和测试，不应在 UI 每帧热路径调用。
    [[nodiscard]] TranslationCacheStats getCacheStats() const;

private:
    /// @brief 隐藏语言字典、锁与缓存容器，避免实现细节传播到所有 UI 头。
    struct Impl;

    /// @brief 翻译器生命周期内唯一持有的私有实现。
    std::unique_ptr<Impl> m_impl;

    /// @brief 在已持有独占锁时把文本放入稳定字符串池。
    /// @param value 待池化的文本。
    /// @return 指向稳定字符串池节点的字符串视图。
    std::string_view internStringLocked(std::string_view value);

    /// @brief 在已持有独占锁时为当前语言完整重建指针缓存。
    /// @warning 仅在低频语言切换、加载或覆写路径执行，会遍历当前字典。
    void rebuildPointerCacheLocked();
};

/// @brief 获取当前皮肤持有的翻译器。
/// @return 当前皮肤翻译器的非拥有引用。
/// @warning UI 热路径会调用此入口；实现仅转发到全局 SkinManager，不分配内存。
Translator& getActiveTranslator();

}  // namespace Translation

// =========================================================
// 基础宏：返回显式零拥有翻译引用。
// =========================================================
#define TR(keyStr)                                     \
    MMM::Translation::getActiveTranslator().translate( \
        MMM::Hash::hashString(keyStr), keyStr)

// =========================================================
// 缓存宏 (性能最高，适用于 ImGui 每帧调用的场景)
// =========================================================
#define TR_CACHE(keyStr)                                                         \
    ([]() -> MMM::Translation::TRResult {                                        \
        static thread_local MMM::Translation::TRResult cachedResult;             \
        static thread_local uint32_t                   cachedVersion    = 0;     \
        static thread_local bool                       cacheInitialized = false; \
        static constexpr uint64_t keyHash = MMM::Hash::hashString(keyStr);       \
        auto&          translator = MMM::Translation::getActiveTranslator();     \
        const uint32_t version    = translator.getVersion();                     \
        if ( !cacheInitialized || cachedVersion != version ) {                   \
            cachedResult     = translator.translate(keyHash, keyStr);            \
            cachedVersion    = version;                                          \
            cacheInitialized = true;                                             \
        }                                                                        \
        return cachedResult;                                                     \
    }())

}  // namespace MMM
