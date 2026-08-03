#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
/// @brief 皮肤翻译覆写文件的合并结果。
struct LanguageOverrideResult {
    /// @brief 覆写文件是否成功解析。
    bool loaded{ false };

    /// @brief 默认语言字典中不存在、因而被忽略的字段名。
    std::vector<std::string> unknownFields;
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

    /// @brief 获取指定键的稳定翻译指针。
    /// @param keyHash 翻译键哈希。
    /// @param fallbackStr 缺少翻译时使用的回退文本。
    /// @return 在 Translator 生命周期内保持有效的 UTF-8 字符串指针。
    /// @warning UI 与逻辑线程热路径会并发调用；缓存命中只持有共享读锁，
    /// 未命中才进入独占写锁并池化字符串。
    const char* translate(uint32_t keyHash, const char* fallbackStr);

private:
    /// @brief 隐藏语言字典、锁与缓存容器，避免实现细节传播到所有 UI 头。
    struct Impl;

    /// @brief 翻译器生命周期内唯一持有的私有实现。
    std::unique_ptr<Impl> m_impl;
};

/// @brief 获取当前皮肤持有的翻译器。
/// @return 当前皮肤翻译器的非拥有引用。
/// @warning UI 热路径会调用此入口；实现仅转发到全局 SkinManager，不分配内存。
Translator& getActiveTranslator();

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
#define TR(key_str)                                        \
    MMM::Translation::TRResult(                            \
        MMM::Translation::getActiveTranslator().translate( \
            MMM::Hash::hash_str(key_str), key_str))

// =========================================================
// 缓存宏 (性能最高，适用于 ImGui 每帧调用的场景)
// =========================================================
#define TR_CACHE(key_str)                                                        \
    ([]() -> MMM::Translation::TRResult {                                        \
        static thread_local const char* cached_ptr     = nullptr;                \
        static thread_local uint32_t    cached_version = 0xFFFFFFFF;             \
        static constexpr uint32_t       key_hash = MMM::Hash::hash_str(key_str); \
        auto&          translator = MMM::Translation::getActiveTranslator();     \
        const uint32_t version    = translator.getVersion();                     \
        if ( cached_version != version ) {                                       \
            cached_ptr     = translator.translate(key_hash, key_str);            \
            cached_version = version;                                            \
        }                                                                        \
        return MMM::Translation::TRResult(cached_ptr);                           \
    }())

}  // namespace MMM
