#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <filesystem>
#include <regex>
#include <vector>

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
    m_data.effectBaseFps = skinTable["meta"]["effectbasefps"].get_or(60.0f);

    // 解析 Colors
    sol::optional<sol::table> colorsTableOpt = skinTable["colors"];
    if ( colorsTableOpt ) {
        parseColorsRecursive(colorsTableOpt.value(), "");
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

    // 从 AppConfig 获取保存的语言设置，如果未设置则回退
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    if ( !m_translator.switchLang(settings.language) ) {
        m_translator.switchLang(m_data.fallBackLang);
    }

    // 解析 Fonts
    sol::table fontsTable = skinTable["fonts"];
    for ( const auto& kv : fontsTable ) {
        std::string key       = kv.first.as<std::string>();
        std::string rpath     = kv.second.as<std::string>();
        m_data.fontPaths[key] = m_data.skinPath / "resources" / rpath;
    }

    // 解析 AsciiFonts
    sol::optional<sol::table> asciiFontsTableOpt = skinTable["ascii_fonts"];
    m_data.asciiFonts.clear();
    if ( asciiFontsTableOpt ) {
        sol::table asciiFontsTable = asciiFontsTableOpt.value();
        // 检查是否为数组格式 (ordered)
        if ( asciiFontsTable[1].valid() ) {
            for ( size_t i = 1; i <= asciiFontsTable.size(); ++i ) {
                sol::object entry = asciiFontsTable[i];
                if ( entry.is<sol::table>() ) {
                    sol::table t = entry.as<sol::table>();
                    if ( t[1].valid() && t[2].valid() ) {
                        m_data.asciiFonts.emplace_back(
                            t[1].get<std::string>(),
                            m_data.skinPath / "resources" /
                                t[2].get<std::string>());
                    }
                }
            }
        } else {
            // 回退到字典格式 (unordered)
            for ( const auto& kv : asciiFontsTable ) {
                std::string name   = kv.first.as<std::string>();
                std::string rpath  = kv.second.as<std::string>();
                m_data.asciiFonts.emplace_back(
                    name, m_data.skinPath / "resources" / rpath);
            }
        }
    }

    // 解析 CjkFonts
    sol::optional<sol::table> cjkFontsTableOpt = skinTable["cjk_fonts"];
    m_data.cjkFonts.clear();
    if ( cjkFontsTableOpt ) {
        sol::table cjkFontsTable = cjkFontsTableOpt.value();
        if ( cjkFontsTable[1].valid() ) {
            for ( size_t i = 1; i <= cjkFontsTable.size(); ++i ) {
                sol::object entry = cjkFontsTable[i];
                if ( entry.is<sol::table>() ) {
                    sol::table t = entry.as<sol::table>();
                    if ( t[1].valid() && t[2].valid() ) {
                        m_data.cjkFonts.emplace_back(
                            t[1].get<std::string>(),
                            m_data.skinPath / "resources" /
                                t[2].get<std::string>());
                    }
                }
            }
        } else {
            for ( const auto& kv : cjkFontsTable ) {
                std::string name  = kv.first.as<std::string>();
                std::string rpath = kv.second.as<std::string>();
                m_data.cjkFonts.emplace_back(
                    name, m_data.skinPath / "resources" / rpath);
            }
        }
    }

    // 解析 Theme 并保存为皮肤的默认推荐主题
    sol::optional<std::string> themeOpt = skinTable["theme"];
    if ( themeOpt ) {
        m_data.defaultTheme = themeOpt.value();
    }

    // 在你的解析代码中
    sol::table assetsTable = skinTable["assets"];
    if ( assetsTable.valid() ) {
        // 初始前缀为空，开始递归
        parseAssetsRecursive(assetsTable, "");

        // 统一分配序列帧的起始 ID (避免在渲染层硬编码偏移)
        // 从 1000 (TextureID::EffectStart) 开始分配
        uint32_t                 currentId = 1000;
        std::vector<std::string> seqKeys;
        for ( const auto& [key, seq] : m_data.effectSequences ) {
            seqKeys.push_back(key);
        }
        // 排序以保证多平台/多次运行时的 ID 分配确定性
        std::sort(seqKeys.begin(), seqKeys.end());

        for ( const auto& key : seqKeys ) {
            m_data.effectSequences[key].startId = currentId;
            uint32_t frameCount                 = static_cast<uint32_t>(
                m_data.effectSequences[key].frames.size());
            XINFO("Assigning sequence {} start ID: {}, frames: {}",
                  key,
                  currentId,
                  frameCount);
            currentId += frameCount;
        }
    }

    sol::table audiosTable = skinTable["audios"];
    if ( audiosTable.valid() ) {
        parseAudiosRecursive(audiosTable, "");
    }

    // 解析特效配置
    sol::optional<sol::table> effectsTableOpt = skinTable["effects"];
    if ( effectsTableOpt ) {
        sol::table                effectsTable = effectsTableOpt.value();
        sol::optional<sol::table> glowOpt      = effectsTable["glow"];
        if ( glowOpt ) {
            m_data.effects.glow.passes = glowOpt.value()["passes"].get_or(8);
            m_data.effects.glow.intensity =
                glowOpt.value()["intensity"].get_or(1.0f);
        }
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

    // 解析 fontsize
    sol::optional<sol::table> fontsizeTableOpt = skinTable["fontsize"];
    if ( fontsizeTableOpt ) {
        parseLayoutRecursive(fontsizeTableOpt.value(), "fontsize");
    }

    // 解析 values
    sol::optional<sol::table> valuesTableOpt = skinTable["values"];
    if ( valuesTableOpt ) {
        parseValuesRecursive(valuesTableOpt.value(), "");
    }

    XINFO("Skin loaded: " + m_data.themeName);
    return true;
}

void SkinManager::parseAudiosRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    for ( const auto& kv : currentTable ) {
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        if ( value.is<std::string>() ) {
            std::string rpath          = value.as<std::string>();
            m_data.audioPaths[fullKey] = m_data.skinPath / "resources" / rpath;
        } else if ( value.is<sol::table>() ) {
            parseAudiosRecursive(value.as<sol::table>(), fullKey);
        }
    }
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
            std::string rpath = value.as<std::string>();

            // 检查是否是序列帧格式，例如 "image/note/effect/flick/[1 ..
            // 16].png"
            std::regex  seqRegex(R"(^(.*)\[(\d+)\s*\.\.\s*(\d+)\](.*)$)");
            std::smatch match;
            if ( std::regex_match(rpath, match, seqRegex) ) {
                std::string prefix = match[1].str();
                int         start  = std::stoi(match[2].str());
                int         end    = std::stoi(match[3].str());
                std::string suffix = match[4].str();

                SkinData::EffectSequence seq;
                int                      step    = (start <= end) ? 1 : -1;
                int                      current = start;
                while ( true ) {
                    std::string framePath =
                        prefix + std::to_string(current) + suffix;
                    seq.frames.push_back(m_data.skinPath / "resources" /
                                         framePath);
                    if ( current == end ) break;
                    current += step;
                }
                m_data.effectSequences[fullKey] = seq;
            } else {
                m_data.assetPaths[fullKey] =
                    m_data.skinPath / "resources" / rpath;
            }
        } else if ( value.is<sol::table>() ) {
            // 如果是表，则递归进入下一层
            parseAssetsRecursive(value.as<sol::table>(), fullKey);
        }
    }
}

/**
 * @brief 递归解析颜色配置表
 * @param currentTable 当前处理的 Lua 表
 * @param prefix 键前缀（用于处理嵌套，如 "preview"）
 */
void SkinManager::parseColorsRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    for ( const auto& kv : currentTable ) {
        // 获取键名
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        // 构建完整的键名（如 "preview.boundingbox"）
        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        if ( value.is<sol::table>() ) {
            // 尝试直接作为颜色数组解析
            sol::table valTable = value.as<sol::table>();
            // 检查表的第一项是否是数字，从而判断这是颜色数组还是嵌套表
            sol::object firstElem = valTable[1];
            if ( firstElem.is<float>() || firstElem.is<double>() ) {
                // 是颜色数组，解析
                std::vector<float> val = valTable.as<std::vector<float>>();
                if ( val.size() >= 3 ) {
                    m_data.colors[fullKey] = {
                        val[0], val[1], val[2], val.size() > 3 ? val[3] : 1.0f
                    };
                }
            } else {
                // 是嵌套表，继续递归
                parseColorsRecursive(valTable, fullKey);
            }
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

void SkinManager::parseValuesRecursive(const sol::table&  currentTable,
                                       const std::string& prefix)
{
    for ( const auto& kv : currentTable ) {
        std::string key   = kv.first.as<std::string>();
        sol::object value = kv.second;

        std::string fullKey = prefix.empty() ? key : prefix + "." + key;

        if ( value.is<sol::table>() ) {
            parseValuesRecursive(value.as<sol::table>(), fullKey);
        } else if ( value.is<double>() ) {
            m_data.values[fullKey] = static_cast<float>(value.as<double>());
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

std::filesystem::path SkinManager::getAudioPath(const std::string& key)
{
    if ( auto audioPathit = m_data.audioPaths.find(key);
         audioPathit != m_data.audioPaths.end() ) {
        return audioPathit->second;
    }
    XERROR("Audio key not found: " + key);
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

const SkinData::EffectSequence* SkinManager::getEffectSequence(
    const std::string& key) const
{
    if ( auto it = m_data.effectSequences.find(key);
         it != m_data.effectSequences.end() ) {
        return &it->second;
    }
    return nullptr;
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

float SkinManager::getValue(const std::string& key, float defaultValue)
{
    if ( auto it = m_data.values.find(key); it != m_data.values.end() ) {
        return it->second;
    }
    return defaultValue;
}

Color SkinManager::getColor(const std::string& key)
{
    if ( m_data.colors.find(key) != m_data.colors.end() ) {
        return m_data.colors[key];
    }
    // 默认返回紫色以示错误
    return { 1.0f, 0.0f, 1.0f, 1.0f };
}

ImFont* SkinManager::getFont(const std::string& key)
{
    if ( auto it = m_data.runtimeFonts.find(key);
         it != m_data.runtimeFonts.end() ) {
        return it->second;
    }
    return nullptr;
}

void SkinManager::setFont(const std::string& key, ImFont* font)
{
    m_data.runtimeFonts[key] = font;
}

}  // namespace Config
}  // namespace MMM
