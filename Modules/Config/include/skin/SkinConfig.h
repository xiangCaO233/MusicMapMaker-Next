#pragma once

#include <string>
#include <filesystem>
#include <unordered_map>

namespace MMM
{
namespace Config
{
// 简单的颜色结构
struct Color
{
    float r, g, b, a;
};

// 皮肤数据持有者
struct SkinData
{
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

    // 布局配置表 (Key: 布局配置ID, Value: 值)
    std::unordered_map<std::string, std::string> layoutConfigs;
};

class SkinManager
{
public:
    static SkinManager& instance();

    bool            loadSkin(const std::string& luaFilePath);
    const SkinData& getData() const { return m_data; }

    // 辅助获取函数
    std::filesystem::path getFontPath(const std::string& key);
    std::filesystem::path getAssetPath(const std::string& key);
    Color       getColor(const std::string& key);

private:
    SkinData m_data;
    SkinManager() = default;
};

}  // namespace Config
}  // namespace MMM
