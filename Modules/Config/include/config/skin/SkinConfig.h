#pragma once

#include "translation/Translation.h"
#include <cstdint>
#include <filesystem>
#include <sol/forward.hpp>
#include <string>
#include <unordered_map>
#include <vector>

struct ImFont;

namespace MMM
{
namespace Config
{
/// @brief 皮肤推荐 UI 主题对应的系统外观分支。
enum class SkinThemeAppearance : std::uint8_t {
    Light,  ///< 系统偏好亮色外观。
    Dark    ///< 系统偏好暗色外观。
};

/// @brief 打击特效纹理在轨道中的布局方式。
enum class HitEffectLayoutMode : std::uint8_t {
    Fixed,     ///< 在判定线打击点按物件缩放绘制固定尺寸纹理。
    TrackFill  ///< 将纹理拉伸至对应单轨的完整可见区域。
};

// 简单的颜色结构
struct Color {
    float r, g, b, a;
};

// 皮肤数据结构
struct SkinData {
    // 皮肤路径
    std::filesystem::path skinPath;

    // 皮肤名称
    std::string themeName;
    std::string themeAuthor;
    std::string themeVersion;

    // 颜色表
    std::unordered_map<std::string, Color> colors;

    /// @brief 皮肤按语言 ID 声明的可选翻译覆写文件。
    std::unordered_map<std::string, std::filesystem::path> langOverrideLuaPaths;

    /// @brief 皮肤覆写中默认语言字典不存在、启动时需要提示的字段。
    std::vector<std::string> missingTranslationOverrideFields;

    /// @brief 默认语言不可用时切换的语言 ID。
    std::string fallBackLang{ "zh_cn" };

    /// @brief 皮肤为系统亮暗外观分别推荐的 UI 主题。
    struct ThemeBinding {
        /// @brief 系统为亮色或无法识别系统偏好时使用的主题。
        std::string light{ "DeepDark" };

        /// @brief 系统为暗色时使用的主题。
        std::string dark{ "DeepDark" };
    } defaultThemes;

    // 字体表
    std::unordered_map<std::string, std::filesystem::path> fontPaths;

    // 字体套装 (用于在 AppConfig 中切换)
    std::vector<std::pair<std::string, std::filesystem::path>> asciiFonts;
    std::vector<std::pair<std::string, std::filesystem::path>> cjkFonts;

    // 运行时字体对象表 (Key: 字体配置ID, Value: ImFont*)
    std::unordered_map<std::string, ImFont*> runtimeFonts;

    // 资产路径表 (Key: 资产ID, Value: 文件路径)
    std::unordered_map<std::string, std::filesystem::path> assetPaths;

    // 音频路径表 (Key: 音频ID, Value: 文件路径)
    std::unordered_map<std::string, std::filesystem::path> audioPaths;

    /// @brief 音效文件开头到有效出声点的延迟，单位为秒。
    std::unordered_map<std::string, double> audioLeadInSeconds;

    // 特效序列帧表
    struct EffectSequence {
        std::vector<std::filesystem::path> frames;
        uint32_t startId{ 0 };  // 对应的起始 TextureID
    };
    std::unordered_map<std::string, EffectSequence> effectSequences;

    // 特效基础帧率
    float effectBaseFps{ 60.0f };


    struct EffectsConfig {
        /// @brief 打击特效纹理布局配置。
        struct HitEffect {
            /// @brief 旧皮肤默认保持固定位置、固定尺寸绘制。
            HitEffectLayoutMode layout{ HitEffectLayoutMode::Fixed };
        } hitEffect;

        struct Glow {
            int   passes    = 8;
            float intensity = 1.0f;
        } glow;
    } effects;

    // 画布配置结构
    struct CanvasConfig {
        // 画布名称
        std::string canvas_name{};
        // 画布着色器模块表 (Key: 资产ID, Value: 文件路径)
        std::unordered_map<std::string, std::filesystem::path>
            canvas_shader_modules{};
    };
    // 画布表 (Key: 画布名称, Value: 画布配置)
    std::unordered_map<std::string, CanvasConfig> canvas_configs;
    // 空画布配置
    CanvasConfig null_canvas_config{};

    // 布局配置表 (Key: 布局配置ID, Value: 值)
    std::unordered_map<std::string, std::string> layoutConfigs;

    // 数值配置表
    std::unordered_map<std::string, float> values;

    // 常用分拍列表
    std::vector<int> commonDivisors;
};

class SkinManager
{
public:
    static SkinManager& instance();

    ///@brief 获取常用分拍列表
    inline const std::vector<int>& getCommonDivisors() const
    {
        return m_data.commonDivisors;
    }

    ///@brief 获取翻译器
    inline Translation::Translator& getTranslator() { return m_translator; }

    /// @brief 使用应用默认翻译资源载入皮肤。
    /// @param luaFilePath 皮肤入口 Lua 文件路径。
    /// @return 皮肤及默认翻译均载入成功时返回 true。
    bool loadSkin(const std::string& luaFilePath);

    /// @brief 使用指定默认翻译资源目录载入皮肤。
    /// @param luaFilePath 皮肤入口 Lua 文件路径。
    /// @param translationsRoot 默认翻译文件所在目录。
    /// @return 皮肤及默认翻译均载入成功时返回 true。
    /// @warning 该重载用于测试和受控资源环境，常规调用应使用单参数入口。
    bool loadSkin(const std::string&           luaFilePath,
                  const std::filesystem::path& translationsRoot);

    ///@brief 直接获取皮肤数据
    const SkinData& getData() const { return m_data; }

    /// @brief 获取皮肤兼容旧调用方的默认主题。
    /// @return 亮色分支主题；无法识别系统外观时也使用该分支。
    const std::string& getDefaultTheme() const
    {
        return m_data.defaultThemes.light;
    }

    /// @brief 获取皮肤为指定系统外观推荐的主题。
    /// @param appearance 系统亮暗外观分支。
    /// @return 对应分支的主题名称。
    const std::string& getDefaultTheme(SkinThemeAppearance appearance) const
    {
        return appearance == SkinThemeAppearance::Dark
                   ? m_data.defaultThemes.dark
                   : m_data.defaultThemes.light;
    }

    ///@brief 获取字体路径
    std::filesystem::path getFontPath(const std::string& key);

    ///@brief 获取音频路径
    std::filesystem::path getAudioPath(const std::string& key);

    /// @brief 获取音效文件开头到有效出声点的延迟。
    /// @param key 音频 ID。
    /// @return 延迟，单位为秒；不存在时返回 0。
    double getAudioLeadInSeconds(const std::string& key) const;

    ///@brief 获取资产路径
    std::filesystem::path getAssetPath(const std::string& key);

    ///@brief 获取特效序列帧
    const SkinData::EffectSequence* getEffectSequence(
        const std::string& key) const;

    ///@brief 获取特效基础帧率
    float getEffectBaseFps() const { return m_data.effectBaseFps; }

    /// @brief 获取打击特效纹理布局方式。
    HitEffectLayoutMode getHitEffectLayoutMode() const
    {
        return m_data.effects.hitEffect.layout;
    }

    ///@brief 获取画布配置
    const SkinData::CanvasConfig& getCanvasConfig(
        const std::string& canvasName);

    ///@brief 获取发光特效的渲染轮次
    int   getGlowPasses() const { return m_data.effects.glow.passes; }
    float getGlowIntensity() const { return m_data.effects.glow.intensity; }

    ///@brief 获取布局配置
    std::string getLayoutConfig(const std::string& key);

    ///@brief 获取数值配置
    float getValue(const std::string& key, float defaultValue = 0.0f);

    ///@brief 获取颜色配置
    Color getColor(const std::string& key);

    ///@brief 获取运行时字体
    ImFont* getFont(const std::string& key);

    ///@brief 获取所有可用的 ASCII 字体
    const std::vector<std::pair<std::string, std::filesystem::path>>&
    getAsciiFonts() const
    {
        return m_data.asciiFonts;
    }

    ///@brief 获取所有可用的 CJK 字体
    const std::vector<std::pair<std::string, std::filesystem::path>>&
    getCjkFonts() const
    {
        return m_data.cjkFonts;
    }

    ///@brief 设置运行时字体 (供渲染层初始化时调用)
    void setFont(const std::string& key, ImFont* font);

    /// @brief 清空运行时字体指针缓存。
    void clearRuntimeFonts();

private:
    ///@brief 皮肤数据
    SkinData m_data;

    ///@brief 翻译器
    Translation::Translator m_translator;

    void parseAssetsRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    /// @brief 递归解析音频配置表。
    /// @param currentTable 当前处理的 Lua 表。
    /// @param prefix 键前缀。
    void parseAudiosRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    /**
     * @brief 递归解析颜色配置表
     * @param currentTable 当前处理的 Lua 表
     * @param prefix 键前缀（用于处理嵌套，如 "preview"）
     */
    void parseColorsRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    /**
     * @brief 递归解析布局配置表
     * @param currentTable 当前处理的 Lua 表
     * @param prefix 键前缀（用于处理嵌套，如 "side_bar"）
     */
    void parseLayoutRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    /**
     * @brief 递归解析数值配置表
     * @param currentTable 当前处理的 Lua 表
     * @param prefix 键前缀
     */
    void parseValuesRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    SkinManager() = default;
};

}  // namespace Config
}  // namespace MMM
