#pragma once

#include <filesystem>
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

    ///@brief 获取颜色配置
    Color getColor(const std::string& key);

private:
    ///@brief 皮肤数据
    SkinData m_data;

    SkinManager() = default;
};

}  // namespace Config
}  // namespace MMM
