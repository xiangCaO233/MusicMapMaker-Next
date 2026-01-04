#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace MMM
{
namespace Hash
{
// FNV-1a Hash 算法 (编译期计算)
constexpr uint32_t hash_str(std::string_view str)
{
    uint32_t hash = 2166136261u;
    for ( char c : str )
    {
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
    static Translator& instance();

    // 载入语言文件
    void loadLanguage(const std::string& langLuaFile);

    // 切换语言
    bool switchLang(const std::string& langID);

    // 获取翻译器版本
    uint32_t getVersion() const;

    // 翻译
    const std::string& translate(uint32_t keyHash, const char* fallbackStr);

private:
    Translator();
    Translator(Translator&&)                 = delete;
    Translator(const Translator&)            = delete;
    Translator& operator=(Translator&&)      = delete;
    Translator& operator=(const Translator&) = delete;
    ~Translator()                            = default;

    // 翻译器版本
    uint32_t m_version{ 0 };

    // 语言map: [translate_key:result]
    using Dictionary = std::unordered_map<uint32_t, std::string>;

    // 所有语言map: [langID:langMap]
    std::unordered_map<std::string, Dictionary> m_Dictionarys;

    // 当前语言map
    Dictionary* m_currentDictionary{ nullptr };
};

}  // namespace Translation

// =========================================================
// 基础宏 (用于无参数的静态文本)
// =========================================================
#define TR(key_str)                                                         \
    (                                                                       \
        []() -> std::string_view                                            \
        {                                                                   \
            /* 静态缓存 */                                                  \
            static std::string_view cachedResult;                           \
            static uint32_t         cachedVer = 0;                          \
                                                                            \
            /* 编译期计算 Hash */                                           \
            /* 只要 key_str 是字符串字面量，这行代码在编译时就变成了常数 */ \
            constexpr uint32_t keyHash = MMM::Hash::hash_str(key_str);      \
                                                                            \
            auto&    trans  = MMM::Translation::Translator::instance();     \
            uint32_t curVer = trans.getVersion();                           \
                                                                            \
            /* 运行时检查版本 */                                            \
            if ( cachedVer != curVer || cachedResult.empty() )              \
            {                                                               \
                /* 传入 Hash 和 原文(作为fallback) */                       \
                cachedResult = trans.translate(keyHash, key_str);           \
                cachedVer    = curVer;                                      \
            }                                                               \
            return cachedResult;                                            \
        })()

// =========================================================
// 格式化缓存宏 (用于带参数的动态文本)
// 用法: TR_FMT("Hello {}", "World")
// =========================================================
#define TR_FMT(key, ...) fmt::format(fmt::runtime(TR(key)), __VA_ARGS__)

}  // namespace MMM
