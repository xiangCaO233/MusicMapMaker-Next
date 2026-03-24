#pragma once

#include <cstdint>
#include <spdlog/fmt/fmt.h>  // 确保引入了 fmt
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
    const std::string& translate(uint32_t keyHash, const char* fallbackStr);

private:
    // 翻译器版本
    uint32_t m_version{ 0 };

    // 语言map: [translate_key:result]
    using Dictionary = std::unordered_map<uint32_t, std::string>;

    // 所有字典: [langID:langMap]
    std::unordered_map<std::string, Dictionary> m_Dictionarys;

    // 当前字典
    Dictionary* m_currentDictionary{ nullptr };
};

struct TRResult {
    std::string_view view;

    // 自动转换为 const char* (ImGui 最需要这个)
    // 注意：这要求翻译器返回的字符串在内存中是 null-terminated 的
    // 绝大多数 std::string 或字符串字面量都满足这一点
    operator const char*() const { return view.data(); }

    // 自动转换为 std::string_view
    operator std::string_view() const { return view; }

    // 为了兼容 fmt 和其他需要 string 的地方
    operator std::string() const { return std::string(view); }

    // 提供基础方法
    const char* data() const { return view.data(); }
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
#define TR(key_str)                                                             \
    ([&]() -> MMM::Translation::TRResult {                                      \
        static std::string cachedResult;                                        \
        static uint32_t    cachedVer = 0;                                       \
                                                                                \
        uint32_t keyHash = MMM::Hash::hash_str(key_str);                        \
                                                                                \
        auto&    trans  = MMM::Config::SkinManager::instance().getTranslator(); \
        uint32_t curVer = trans.getVersion();                                   \
                                                                                \
        if ( cachedVer != curVer || cachedResult.empty() ) {                    \
            cachedResult = trans.translate(keyHash, key_str);                   \
            cachedVer    = curVer;                                              \
        }                                                                       \
        /* 返回包装后的结果 */                                                  \
        return { cachedResult };                                                \
    })()

// TR_FMT 保持不变，fmt 会自动识别 TRResult 的转换或其内部的 string_view
#define TR_FMT(key, ...) fmt::format(fmt::runtime(TR(key).view), __VA_ARGS__)

}  // namespace MMM
