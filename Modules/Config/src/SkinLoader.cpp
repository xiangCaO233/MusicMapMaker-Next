#include "SkinConfig.h"
#include "colorful-log.h"
#include <filesystem>
#include <sol/sol.hpp>

namespace MMM
{
namespace Config
{

SkinManager& SkinManager::instance()
{
    static SkinManager inst;
    return inst;
}

bool SkinManager::loadSkin(const std::string& luaFilePath)
{
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table);

    namespace fs = std::filesystem;
    // --- 路径注入 ---
    fs::path    path(luaFilePath);
    fs::path    absPath = fs::absolute(path);
    std::string skinDir = absPath.parent_path().string();
    std::replace(skinDir.begin(), skinDir.end(), '\\', '/');
    if (!skinDir.empty() && skinDir.back() != '/') skinDir += '/';
    // 注入 __SKINLUA_DIR__
    lua["__SKINLUA_DIR__"] = skinDir;
    XINFO("LuaJIT Loading skin from: " + skinDir);

    // 加载 Lua 文件
    sol::protected_function_result result = lua.script_file(luaFilePath);
    if (!result.valid())
    {
        sol::error err = result;
        XERROR("Failed to load skin lua: " + std::string(err.what()));
        return false;
    }

    // 获取返回的 Table (对应 Lua 中的 return Skin)
    sol::table skinTable = result;

    // 解析 Meta
    m_data.themeName = skinTable["meta"]["name"].get_or<std::string>("Unknown");

    // 解析 Colors
    sol::table colorsTable = skinTable["colors"];
    for (const auto& kv : colorsTable)
    {
        std::string        key = kv.first.as<std::string>();
        std::vector<float> val = kv.second.as<std::vector<float>>();

        if (val.size() >= 3)
        {
            m_data.colors[key] = {
                val[0], val[1], val[2], val.size() > 3 ? val[3] : 1.0f
            };
        }
    }

    // 解析 Assets
    sol::table assetsTable = skinTable["assets"];
    for (const auto& kv : assetsTable)
    {
        std::string key        = kv.first.as<std::string>();
        std::string path       = kv.second.as<std::string>();
        m_data.assetPaths[key] = path;
    }

    XINFO("Skin loaded: " + m_data.themeName);
    return true;
}

std::string SkinManager::getAssetPath(const std::string& key)
{
    if (m_data.assetPaths.find(key) != m_data.assetPaths.end())
    {
        return m_data.assetPaths[key];
    }
    XERROR("Asset key not found: " + key);
    return "";
}

Color SkinManager::getColor(const std::string& key)
{
    if (m_data.colors.find(key) != m_data.colors.end())
    {
        return m_data.colors[key];
    }
    // 默认返回紫色以示错误
    return { 1.0f, 0.0f, 1.0f, 1.0f };
}

}  // namespace Config
}  // namespace MMM
