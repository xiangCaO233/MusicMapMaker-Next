#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include <filesystem>

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
    fs::path path(luaFilePath);
    fs::path absPath    = fs::absolute(path);
    m_data.skinPath     = absPath.parent_path();
    std::string skinDir = m_data.skinPath.generic_string();
    // 规范路径字符串
    std::replace(skinDir.begin(), skinDir.end(), '\\', '/');
    if ( !skinDir.empty() && skinDir.back() != '/' ) skinDir += '/';
    // 注入 __SKINLUA_DIR__
    lua["__SKINLUA_DIR__"] = skinDir;
    XINFO("LuaJIT Loading skin from: " + skinDir);

    // 加载 Lua 文件
    sol::protected_function_result result = lua.script_file(luaFilePath);
    if ( !result.valid() ) {
        sol::error err = result;
        XERROR("Failed to load skin lua: " + std::string(err.what()));
        return false;
    }

    // 获取返回的 Table (对应 Lua 中的 return Skin)
    sol::table skinTable = result;

    // 解析 Meta
    m_data.themeName =
        skinTable["meta"]["name"].get_or<std::string>("Custom Skin");
    m_data.themeAuthor =
        skinTable["meta"]["author"].get_or<std::string>("Various Artists");
    m_data.themeVersion =
        skinTable["meta"]["version"].get_or<std::string>("1.0");

    // 解析 Colors
    sol::table colorsTable = skinTable["colors"];
    for ( const auto& kv : colorsTable ) {
        std::string        key = kv.first.as<std::string>();
        std::vector<float> val = kv.second.as<std::vector<float>>();

        if ( val.size() >= 3 ) {
            m_data.colors[key] = {
                val[0], val[1], val[2], val.size() > 3 ? val[3] : 1.0f
            };
        }
    }

    // 解析 langs
    sol::table langsTable = skinTable["langs"];
    for ( const auto& kv : langsTable ) {
        std::string key          = kv.first.as<std::string>();
        std::string rpath        = kv.second.as<std::string>();
        m_data.langLuaPaths[key] = m_data.skinPath / "resources" / rpath;
    }

    // 载入所有语言
    for ( auto& [langName, langLuaPath] : m_data.langLuaPaths ) {
        m_translator.loadLanguage(langLuaPath.generic_string());
    }
    // 选择回退语言
    m_translator.switchLang(m_data.fallBackLang);

    // 解析 Fonts
    sol::table fontsTable = skinTable["fonts"];
    for ( const auto& kv : fontsTable ) {
        std::string key       = kv.first.as<std::string>();
        std::string rpath     = kv.second.as<std::string>();
        m_data.fontPaths[key] = m_data.skinPath / "resources" / rpath;
    }

    // 在你的解析代码中
    sol::table assetsTable = skinTable["assets"];
    if ( assetsTable.valid() ) {
        // 初始前缀为空，开始递归
        parseAssetsRecursive(assetsTable, "");
    }

    // 解析 canvases_2d
    // 使用 sol::optional 防止皮肤文件中没有配这个表导致崩溃
    sol::optional<sol::table> canvasesTableOpt = skinTable["canvases_2d"];
    if ( canvasesTableOpt ) {
        sol::table canvasesTable = canvasesTableOpt.value();
        for ( const auto& kv : canvasesTable ) {
            // kv.first 是 "basic_2d_canvas"
            sol::table canvasData = kv.second.as<sol::table>();

            SkinData::CanvasConfig config;
            // 获取 name，比如 "Basic2DCanvas"
            config.canvas_name = canvasData["name"].get_or<std::string>("");

            // 解析 shader_modules
            sol::optional<sol::table> modulesOpt = canvasData["shader_modules"];
            if ( modulesOpt ) {
                sol::table modulesTable = modulesOpt.value();
                for ( const auto& modKv : modulesTable ) {
                    std::string modKey =
                        modKv.first.as<std::string>();  // "main", "effect"
                    std::string rpath = modKv.second.as<std::string>();
                    // 拼接绝对路径
                    config.canvas_shader_modules[modKey] =
                        m_data.skinPath / "resources" / rpath;
                }
            }

            // 使用 canvas_name 作为 Key 存入 map
            if ( !config.canvas_name.empty() ) {
                m_data.canvas_configs[config.canvas_name] = config;
            }
        }
    }

    // 解析 layout
    sol::table layoutTable = skinTable["layout"];
    if ( layoutTable.valid() ) {
        // 启动递归解析，初始前缀为空
        parseLayoutRecursive(layoutTable, "");
    }

    XINFO("Skin loaded: " + m_data.themeName);
    return true;
}

/**
 * @brief 递归解析 Lua 资产表
 * @param currentTable 当前正在处理的 Lua 表
 * @param prefix 当前键的前缀（用于处理嵌套，如 "side_bar"）
 */
void SkinManager::parseAssetsRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    for ( const auto& kv : currentTable ) {
        // 获取键名
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        // 构建完整的键名（如 "side_bar.file_explorer_icon"）
        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        if ( value.is<std::string>() ) {
            // 如果是字符串，说明到了叶子节点，保存路径
            std::string rpath          = value.as<std::string>();
            m_data.assetPaths[fullKey] = m_data.skinPath / "resources" / rpath;

            // 打印日志方便调试
            // XINFO("Loaded Asset: {} -> {}", fullKey, rpath);
        } else if ( value.is<sol::table>() ) {
            // 如果是表，则递归进入下一层
            parseAssetsRecursive(value.as<sol::table>(), fullKey);
        }
    }
}

/**
 * @brief 递归解析布局配置表
 * @param currentTable 当前处理的 Lua 表
 * @param prefix 键前缀（用于处理嵌套，如 "side_bar"）
 */
void SkinManager::parseLayoutRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    for ( const auto& kv : currentTable ) {
        // 获取键名
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        // 构建完整路径键名，如 "layout.side_bar.width"
        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        if ( value.is<sol::table>() ) {
            // 如果是子表，递归进入
            parseLayoutRecursive(value.as<sol::table>(), fullKey);
        } else {
            // 处理基本类型并转换为 string 存储
            if ( value.is<std::string>() ) {
                m_data.layoutConfigs[fullKey] = value.as<std::string>();
            } else if ( value.is<double>() ) {
                double val = value.as<double>();
                // 如果是整数，去掉小数点（例如把 32.0 变成 "32"）
                if ( val == static_cast<long long>(val) ) {
                    m_data.layoutConfigs[fullKey] =
                        std::to_string(static_cast<long long>(val));
                } else {
                    // 如果是浮点数（如 0.275），保留原样
                    m_data.layoutConfigs[fullKey] = std::to_string(val);
                }
            } else if ( value.is<bool>() ) {
                m_data.layoutConfigs[fullKey] =
                    value.as<bool>() ? "true" : "false";
            }
        }
    }
}

std::filesystem::path SkinManager::getFontPath(const std::string& key)
{
    if ( auto fontPathit = m_data.fontPaths.find(key);
         fontPathit != m_data.fontPaths.end() ) {
        return fontPathit->second;
    }
    XERROR("Asset key not found: " + key);
    return "";
}

std::filesystem::path SkinManager::getAssetPath(const std::string& key)
{
    if ( auto assetPathit = m_data.assetPaths.find(key);
         assetPathit != m_data.assetPaths.end() ) {
        return assetPathit->second;
    }
    XERROR("Asset key not found: " + key);
    return "";
}

///@brief 获取画布配置
const SkinData::CanvasConfig& SkinManager::getCanvasConfig(
    const std::string& canvasName)
{
    if ( auto canvas_config_it = m_data.canvas_configs.find(canvasName);
         canvas_config_it != m_data.canvas_configs.end() ) {
        return canvas_config_it->second;
    }
    XERROR("CanvasConfig key not found: " + canvasName);
    return m_data.null_canvas_config;
}

///@brief 获取布局配置
std::string SkinManager::getLayoutConfig(const std::string& key)
{
    if ( auto layout_config_it = m_data.layoutConfigs.find(key);
         layout_config_it != m_data.layoutConfigs.end() ) {
        return layout_config_it->second;
    }
    return {};
}

Color SkinManager::getColor(const std::string& key)
{
    if ( m_data.colors.find(key) != m_data.colors.end() ) {
        return m_data.colors[key];
    }
    // 默认返回紫色以示错误
    return { 1.0f, 0.0f, 1.0f, 1.0f };
}

}  // namespace Config
}  // namespace MMM
