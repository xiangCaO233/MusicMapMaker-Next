#pragma once

#include <spdlog/fmt/fmt.h>  // 确保引入了 fmt

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace MMM
{
namespace Hash
{
// FNV-1a Hash 算法 (编译期计算)
constexpr uint32_t hash_str(std::string_view str)
{
    uint32_t hash = 2166136261u;
    for ( char c : str ) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}
}  // namespace Hash

namespace Translation
{
class Translator
{
public:
    Translator();
    Translator(Translator&&)                 = delete;
    Translator(const Translator&)            = delete;
    Translator& operator=(Translator&&)      = delete;
    Translator& operator=(const Translator&) = delete;
    ~Translator()                            = default;

    /// @brief 载入一个 Lua 语言字典。
    /// @param langLuaFile 语言文件路径。
    /// @warning 解析在锁外执行，发布字典时短暂持有独占锁。
    void loadLanguage(const std::string& langLuaFile);

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

    /// @brief 获取指定键的稳定翻译指针。
    /// @param keyHash 翻译键哈希。
    /// @param fallbackStr 缺少翻译时使用的回退文本。
    /// @return 在 Translator 生命周期内保持有效的 UTF-8 字符串指针。
    /// @warning UI 与逻辑线程热路径会并发调用；缓存命中只持有共享读锁，
    /// 未命中才进入独占写锁并池化字符串。
    const char* translate(uint32_t keyHash, const char* fallbackStr);

private:
    /// @brief 保护语言字典、当前字典、指针缓存和稳定字符串池。
    mutable std::shared_mutex m_mutex;

    /// @brief 翻译缓存版本。
    /// @warning UI 与逻辑线程并发读取，语言加载线程低频写入；原子用于避免
    /// `TR_CACHE` 版本检查的数据竞争。
    std::atomic<uint32_t> m_version{ 0 };

    // 语言map: [translate_key:result]
    using Dictionary = std::unordered_map<uint32_t, std::string>;

    // 所有字典: [langID:langMap]
    std::unordered_map<std::string, Dictionary> m_Dictionarys;

    // 当前字典
    Dictionary* m_currentDictionary{ nullptr };

    // 翻译指针缓存：[keyHash:stablePtr]
    // 当语言切换或字典更新时必须清空，用于加速 translate 函数
    std::unordered_map<uint32_t, const char*> m_pointerCache;

    // 字符串池：确保所有返回给 UI 的指针在 Translator 生命周期内稳定。
    // 语言热切换时不得清空；unordered_set 插入和 rehash 不使元素引用失效。
    std::unordered_set<std::string> m_stringPool;
};

struct TRResult {
    // 原始字符串指针，直接来自字典 Map 或 fallback 字符串字面量
    const char* pStr;
    // 兼容 view 访问
    std::string_view view;

    TRResult(const char* s) : pStr(s), view(s ? s : "") {}

    // 自动转换为 const char* (ImGui 最需要这个)
    operator const char*() const { return pStr; }

    // 自动转换为 std::string_view
    operator std::string_view() const { return view; }

    // 为了兼容 fmt 和其他需要 string 的地方
    operator std::string() const { return std::string(view); }

    // 提供基础方法
    const char* data() const { return pStr; }
    bool        empty() const { return view.empty(); }
};

// =========================================================
// 定义 format_as 钩子
// =========================================================
inline std::string_view format_as(const TRResult& tr)
{
    return tr.view;
}

}  // namespace Translation

// =========================================================
// 基础宏 (支持自动转换为 const char*)
// =========================================================
#define TR(key_str)                                                     \
    MMM::Translation::TRResult(                                         \
        MMM::Config::SkinManager::instance().getTranslator().translate( \
            MMM::Hash::hash_str(key_str), key_str))

// =========================================================
// 缓存宏 (性能最高，适用于 ImGui 每帧调用的场景)
// =========================================================
#define TR_CACHE(key_str)                                                        \
    ([]() -> MMM::Translation::TRResult {                                        \
        static thread_local const char* cached_ptr     = nullptr;                \
        static thread_local uint32_t    cached_version = 0xFFFFFFFF;             \
        static constexpr uint32_t       key_hash = MMM::Hash::hash_str(key_str); \
        auto&                           translator =                             \
            MMM::Config::SkinManager::instance().getTranslator();                \
        const uint32_t version = translator.getVersion();                        \
        if ( cached_version != version ) {                                       \
            cached_ptr     = translator.translate(key_hash, key_str);            \
            cached_version = version;                                            \
        }                                                                        \
        return MMM::Translation::TRResult(cached_ptr);                           \
    }())

// TR_FMT 保持不变，fmt 会自动识别 TRResult 的转换或其内部的 string_view
#define TR_FMT(key, ...) fmt::format(fmt::runtime(TR(key).view), __VA_ARGS__)

}  // namespace MMM
