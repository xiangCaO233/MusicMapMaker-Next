#pragma once

#include "translation/Translation.h"
#include <filesystem>
#include <sol/sol.hpp>
#include <string>
#include <unordered_map>
#include <vector>

struct ImFont;

namespace MMM
{
namespace Config
{
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

    // 语言表
    std::unordered_map<std::string, std::filesystem::path> langLuaPaths;
    std::string fallBackLang{ "zh_cn" };

    /// @brief 皮肤推荐的默认主题
    std::string defaultTheme{ "DeepDark" };

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

    // 特效序列帧表
    struct EffectSequence {
        std::vector<std::filesystem::path> frames;
        uint32_t startId{ 0 };  // 对应的起始 TextureID
    };
    std::unordered_map<std::string, EffectSequence> effectSequences;

    // 特效基础帧率
    float effectBaseFps{ 60.0f };


    struct EffectsConfig {
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
};

class SkinManager
{
public:
    static SkinManager& instance();

    ///@brief 获取翻译器
    inline Translation::Translator& getTranslator() { return m_translator; }

    ///@brief 载入皮肤
    bool loadSkin(const std::string& luaFilePath);

    ///@brief 直接获取皮肤数据
    const SkinData& getData() const { return m_data; }

    ///@brief 获取皮肤默认主题
    std::string getDefaultTheme() const { return m_data.defaultTheme; }

    ///@brief 获取字体路径
    std::filesystem::path getFontPath(const std::string& key);

    ///@brief 获取音频路径
    std::filesystem::path getAudioPath(const std::string& key);

    ///@brief 获取资产路径
    std::filesystem::path getAssetPath(const std::string& key);

    ///@brief 获取特效序列帧
    const SkinData::EffectSequence* getEffectSequence(
        const std::string& key) const;

    ///@brief 获取特效基础帧率
    float getEffectBaseFps() const { return m_data.effectBaseFps; }

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

private:
    ///@brief 皮肤数据
    SkinData m_data;

    ///@brief 翻译器
    Translation::Translator m_translator;

    void parseAssetsRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

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
