#pragma once

#include <cstdint>
#include <spdlog/fmt/fmt.h>  // 确保引入了 fmt
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

    // 载入语言文件
    void loadLanguage(const std::string& langLuaFile);

    // 切换语言
    bool switchLang(const std::string& langID);

    // 获取翻译器版本
    uint32_t getVersion() const;

    // 翻译
    const char* translate(uint32_t keyHash, const char* fallbackStr);

private:
    // 翻译器版本
    uint32_t m_version{ 0 };

    // 语言map: [translate_key:result]
    using Dictionary = std::unordered_map<uint32_t, std::string>;

    // 所有字典: [langID:langMap]
    std::unordered_map<std::string, Dictionary> m_Dictionarys;

    // 当前字典
    Dictionary* m_currentDictionary{ nullptr };

    // 翻译指针缓存：[keyHash:stablePtr]
    // 当语言切换或字典更新时必须清空，用于加速 translate 函数
    std::unordered_map<uint32_t, const char*> m_pointerCache;

    // 字符串池：确保所有返回给 UI 的指针在整个程序运行期间都是稳定的
    // unordered_set 中的元素地址在插入新元素时不会改变
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
        static const char*        cached_ptr     = nullptr;                      \
        static uint32_t           cached_version = 0xFFFFFFFF;                   \
        static constexpr uint32_t key_hash       = MMM::Hash::hash_str(key_str); \
        auto&                     translator =                                   \
            MMM::Config::SkinManager::instance().getTranslator();                \
        if ( cached_version != translator.getVersion() ) {                       \
            cached_ptr     = translator.translate(key_hash, key_str);            \
            cached_version = translator.getVersion();                            \
        }                                                                        \
        return MMM::Translation::TRResult(cached_ptr);                           \
    }())

// TR_FMT 保持不变，fmt 会自动识别 TRResult 的转换或其内部的 string_view
#define TR_FMT(key, ...) fmt::format(fmt::runtime(TR(key).view), __VA_ARGS__)

}  // namespace MMM
