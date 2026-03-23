#pragma once

#include "translation/Translation.h"
#include <filesystem>
#include <sol/sol.hpp>
#include <string>
#include <unordered_map>
#include <vector>

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

    // 字体表
    std::unordered_map<std::string, std::filesystem::path> fontPaths;

    // 资产路径表 (Key: 资产ID, Value: 文件路径)
    std::unordered_map<std::string, std::filesystem::path> assetPaths;

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

    ///@brief 获取字体路径
    std::filesystem::path getFontPath(const std::string& key);

    ///@brief 获取资产路径
    std::filesystem::path getAssetPath(const std::string& key);

    ///@brief 获取画布配置
    const SkinData::CanvasConfig& getCanvasConfig(
        const std::string& canvasName);

    ///@brief 获取布局配置
    std::string getLayoutConfig(const std::string& key);

    ///@brief 获取颜色配置
    Color getColor(const std::string& key);

private:
    ///@brief 皮肤数据
    SkinData m_data;

    ///@brief 翻译器
    Translation::Translator m_translator;

    void parseAssetsRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    /**
     * @brief 递归解析布局配置表
     * @param currentTable 当前处理的 Lua 表
     * @param prefix 键前缀（用于处理嵌套，如 "side_bar"）
     */
    void parseLayoutRecursive(const sol::table&  currentTable,
                              const std::string& prefix);



    SkinManager() = default;
};

}  // namespace Config
}  // namespace MMM
