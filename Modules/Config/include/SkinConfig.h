#pragma once
#include <string>
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
    std::string themeName;

    // 颜色表
    std::unordered_map<std::string, Color> colors;

    // 资产路径表 (Key: 资产ID, Value: 文件路径)
    std::unordered_map<std::string, std::string> assetPaths;

    float windowPadding;
};

class SkinManager
{
public:
    static SkinManager& instance();

    bool            loadSkin(const std::string& luaFilePath);
    const SkinData& getData() const { return m_data; }

    // 辅助获取函数
    std::string getAssetPath(const std::string& key);
    Color       getColor(const std::string& key);

private:
    SkinData m_data;
    SkinManager() = default;
};

}  // namespace Config
}  // namespace MMM
