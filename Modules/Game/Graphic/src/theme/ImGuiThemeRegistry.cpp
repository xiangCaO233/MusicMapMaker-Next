#include "graphic/theme/ImGuiThemeRegistry.h"

#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace MMM::Graphic
{
namespace
{

/// @brief 浮点样式字段名称与成员映射。
struct FloatStyleField {
    /// @brief Lua 字段名称。
    std::string_view name;
    /// @brief ImGuiStyle 浮点成员。
    float ImGuiStyle::* member;
};

/// @brief 二维向量样式字段名称与成员映射。
struct Vec2StyleField {
    /// @brief Lua 字段名称。
    std::string_view name;
    /// @brief ImGuiStyle 二维向量成员。
    ImVec2 ImGuiStyle::* member;
};

/// @brief 布尔样式字段名称与成员映射。
struct BoolStyleField {
    /// @brief Lua 字段名称。
    std::string_view name;
    /// @brief ImGuiStyle 布尔成员。
    bool ImGuiStyle::* member;
};

/// @brief 方向样式字段名称与成员映射。
struct DirectionStyleField {
    /// @brief Lua 字段名称。
    std::string_view name;
    /// @brief ImGuiStyle 方向成员。
    ImGuiDir ImGuiStyle::* member;
};

constexpr std::array FLOAT_STYLE_FIELDS{
    FloatStyleField{ "Alpha", &ImGuiStyle::Alpha },
    FloatStyleField{ "DisabledAlpha", &ImGuiStyle::DisabledAlpha },
    FloatStyleField{ "WindowRounding", &ImGuiStyle::WindowRounding },
    FloatStyleField{ "WindowBorderSize", &ImGuiStyle::WindowBorderSize },
    FloatStyleField{ "WindowBorderHoverPadding",
                     &ImGuiStyle::WindowBorderHoverPadding },
    FloatStyleField{ "ChildRounding", &ImGuiStyle::ChildRounding },
    FloatStyleField{ "ChildBorderSize", &ImGuiStyle::ChildBorderSize },
    FloatStyleField{ "PopupRounding", &ImGuiStyle::PopupRounding },
    FloatStyleField{ "PopupBorderSize", &ImGuiStyle::PopupBorderSize },
    FloatStyleField{ "FrameRounding", &ImGuiStyle::FrameRounding },
    FloatStyleField{ "FrameBorderSize", &ImGuiStyle::FrameBorderSize },
    FloatStyleField{ "IndentSpacing", &ImGuiStyle::IndentSpacing },
    FloatStyleField{ "ColumnsMinSpacing", &ImGuiStyle::ColumnsMinSpacing },
    FloatStyleField{ "ScrollbarSize", &ImGuiStyle::ScrollbarSize },
    FloatStyleField{ "ScrollbarRounding", &ImGuiStyle::ScrollbarRounding },
    FloatStyleField{ "ScrollbarPadding", &ImGuiStyle::ScrollbarPadding },
    FloatStyleField{ "GrabMinSize", &ImGuiStyle::GrabMinSize },
    FloatStyleField{ "GrabRounding", &ImGuiStyle::GrabRounding },
    FloatStyleField{ "LogSliderDeadzone", &ImGuiStyle::LogSliderDeadzone },
    FloatStyleField{ "ImageRounding", &ImGuiStyle::ImageRounding },
    FloatStyleField{ "ImageBorderSize", &ImGuiStyle::ImageBorderSize },
    FloatStyleField{ "TabRounding", &ImGuiStyle::TabRounding },
    FloatStyleField{ "TabBorderSize", &ImGuiStyle::TabBorderSize },
    FloatStyleField{ "TabMinWidthBase", &ImGuiStyle::TabMinWidthBase },
    FloatStyleField{ "TabMinWidthShrink", &ImGuiStyle::TabMinWidthShrink },
    FloatStyleField{ "TabCloseButtonMinWidthSelected",
                     &ImGuiStyle::TabCloseButtonMinWidthSelected },
    FloatStyleField{ "TabCloseButtonMinWidthUnselected",
                     &ImGuiStyle::TabCloseButtonMinWidthUnselected },
    FloatStyleField{ "TabBarBorderSize", &ImGuiStyle::TabBarBorderSize },
    FloatStyleField{ "TabBarOverlineSize", &ImGuiStyle::TabBarOverlineSize },
    FloatStyleField{ "TableAngledHeadersAngle",
                     &ImGuiStyle::TableAngledHeadersAngle },
    FloatStyleField{ "TreeLinesSize", &ImGuiStyle::TreeLinesSize },
    FloatStyleField{ "TreeLinesRounding", &ImGuiStyle::TreeLinesRounding },
    FloatStyleField{ "MenuItemRounding", &ImGuiStyle::MenuItemRounding },
    FloatStyleField{ "SelectableRounding", &ImGuiStyle::SelectableRounding },
    FloatStyleField{ "DragDropTargetRounding",
                     &ImGuiStyle::DragDropTargetRounding },
    FloatStyleField{ "DragDropTargetBorderSize",
                     &ImGuiStyle::DragDropTargetBorderSize },
    FloatStyleField{ "DragDropTargetPadding",
                     &ImGuiStyle::DragDropTargetPadding },
    FloatStyleField{ "ColorMarkerSize", &ImGuiStyle::ColorMarkerSize },
    FloatStyleField{ "InputTextCursorSize", &ImGuiStyle::InputTextCursorSize },
    FloatStyleField{ "SeparatorSize", &ImGuiStyle::SeparatorSize },
    FloatStyleField{ "SeparatorTextBorderSize",
                     &ImGuiStyle::SeparatorTextBorderSize },
    FloatStyleField{ "DockingSeparatorSize",
                     &ImGuiStyle::DockingSeparatorSize },
    FloatStyleField{ "MouseCursorScale", &ImGuiStyle::MouseCursorScale },
    FloatStyleField{ "CurveTessellationTol",
                     &ImGuiStyle::CurveTessellationTol },
    FloatStyleField{ "CircleTessellationMaxError",
                     &ImGuiStyle::CircleTessellationMaxError },
};

constexpr std::array VEC2_STYLE_FIELDS{
    Vec2StyleField{ "WindowPadding", &ImGuiStyle::WindowPadding },
    Vec2StyleField{ "WindowMinSize", &ImGuiStyle::WindowMinSize },
    Vec2StyleField{ "WindowTitleAlign", &ImGuiStyle::WindowTitleAlign },
    Vec2StyleField{ "FramePadding", &ImGuiStyle::FramePadding },
    Vec2StyleField{ "ItemSpacing", &ImGuiStyle::ItemSpacing },
    Vec2StyleField{ "ItemInnerSpacing", &ImGuiStyle::ItemInnerSpacing },
    Vec2StyleField{ "CellPadding", &ImGuiStyle::CellPadding },
    Vec2StyleField{ "TouchExtraPadding", &ImGuiStyle::TouchExtraPadding },
    Vec2StyleField{ "TableAngledHeadersTextAlign",
                    &ImGuiStyle::TableAngledHeadersTextAlign },
    Vec2StyleField{ "ButtonTextAlign", &ImGuiStyle::ButtonTextAlign },
    Vec2StyleField{ "SelectableTextAlign", &ImGuiStyle::SelectableTextAlign },
    Vec2StyleField{ "SeparatorTextAlign", &ImGuiStyle::SeparatorTextAlign },
    Vec2StyleField{ "SeparatorTextPadding", &ImGuiStyle::SeparatorTextPadding },
    Vec2StyleField{ "DisplayWindowPadding", &ImGuiStyle::DisplayWindowPadding },
    Vec2StyleField{ "DisplaySafeAreaPadding",
                    &ImGuiStyle::DisplaySafeAreaPadding },
};

constexpr std::array BOOL_STYLE_FIELDS{
    BoolStyleField{ "DockingNodeHasCloseButton",
                    &ImGuiStyle::DockingNodeHasCloseButton },
    BoolStyleField{ "AntiAliasedLines", &ImGuiStyle::AntiAliasedLines },
    BoolStyleField{ "AntiAliasedLinesUseTex",
                    &ImGuiStyle::AntiAliasedLinesUseTex },
    BoolStyleField{ "AntiAliasedFill", &ImGuiStyle::AntiAliasedFill },
};

constexpr std::array DIRECTION_STYLE_FIELDS{
    DirectionStyleField{ "WindowMenuButtonPosition",
                         &ImGuiStyle::WindowMenuButtonPosition },
    DirectionStyleField{ "ColorButtonPosition",
                         &ImGuiStyle::ColorButtonPosition },
};

/// @brief 单个浮点字段覆盖值。
struct FloatStyleOverride {
    /// @brief 目标成员。
    float ImGuiStyle::* member;
    /// @brief 覆盖值。
    float value;
};

/// @brief 单个二维向量字段覆盖值。
struct Vec2StyleOverride {
    /// @brief 目标成员。
    ImVec2 ImGuiStyle::* member;
    /// @brief 覆盖值。
    ImVec2 value;
};

/// @brief 单个布尔字段覆盖值。
struct BoolStyleOverride {
    /// @brief 目标成员。
    bool ImGuiStyle::* member;
    /// @brief 覆盖值。
    bool value;
};

/// @brief 单个方向字段覆盖值。
struct DirectionStyleOverride {
    /// @brief 目标成员。
    ImGuiDir ImGuiStyle::* member;
    /// @brief 覆盖值。
    ImGuiDir value;
};

/// @brief 单个颜色字段覆盖值。
struct ColorStyleOverride {
    /// @brief ImGuiCol 索引。
    ImGuiCol index;
    /// @brief RGBA 覆盖值。
    ImVec4 value;
};

/// @brief Lua 主题定义解析后的 ImGuiStyle 增量。
struct ImGuiThemeStylePatch {
    /// @brief 浮点字段覆盖。
    std::vector<FloatStyleOverride> floats;
    /// @brief 二维向量字段覆盖。
    std::vector<Vec2StyleOverride> vectors;
    /// @brief 布尔字段覆盖。
    std::vector<BoolStyleOverride> booleans;
    /// @brief 方向字段覆盖。
    std::vector<DirectionStyleOverride> directions;
    /// @brief 颜色字段覆盖。
    std::vector<ColorStyleOverride> colors;

    /// @brief 将全部覆盖值写入样式。
    /// @param style 目标样式。
    void apply(ImGuiStyle& style) const
    {
        for ( const auto& overrideValue : floats ) {
            style.*(overrideValue.member) = overrideValue.value;
        }
        for ( const auto& overrideValue : vectors ) {
            style.*(overrideValue.member) = overrideValue.value;
        }
        for ( const auto& overrideValue : booleans ) {
            style.*(overrideValue.member) = overrideValue.value;
        }
        for ( const auto& overrideValue : directions ) {
            style.*(overrideValue.member) = overrideValue.value;
        }
        for ( const auto& overrideValue : colors ) {
            style.Colors[overrideValue.index] = overrideValue.value;
        }
    }
};

/// @brief 判断 Lua 对象是否为有限数值。
/// @param object 待检查对象。
/// @param value 成功时写入的浮点值。
/// @return 对象为有限数值时返回 true。
bool readFiniteFloat(const sol::object& object, float& value)
{
    if ( object.get_type() != sol::type::number ) return false;
    const double parsed = object.as<double>();
    if ( !std::isfinite(parsed) ) return false;
    value = static_cast<float>(parsed);
    return std::isfinite(value);
}

/// @brief 从 Lua 表读取二维向量。
/// @param object 数组或带 x、y 字段的表。
/// @param value 成功时写入的向量。
/// @return 格式有效时返回 true。
bool readVec2(const sol::object& object, ImVec2& value)
{
    if ( !object.is<sol::table>() ) return false;
    const sol::table table = object.as<sol::table>();
    sol::object      x     = table[1];
    sol::object      y     = table[2];
    if ( !x.valid() || x.get_type() == sol::type::nil ) x = table["x"];
    if ( !y.valid() || y.get_type() == sol::type::nil ) y = table["y"];
    return readFiniteFloat(x, value.x) && readFiniteFloat(y, value.y);
}

/// @brief 从 Lua 表读取 RGBA 颜色。
/// @param object 数组或带 r、g、b、a 字段的表。
/// @param value 成功时写入的颜色。
/// @return 四个分量均位于 0 到 1 时返回 true。
bool readColor(const sol::object& object, ImVec4& value)
{
    if ( !object.is<sol::table>() ) return false;
    const sol::table table = object.as<sol::table>();
    sol::object      r     = table[1];
    sol::object      g     = table[2];
    sol::object      b     = table[3];
    sol::object      a     = table[4];
    if ( !r.valid() || r.get_type() == sol::type::nil ) r = table["r"];
    if ( !g.valid() || g.get_type() == sol::type::nil ) g = table["g"];
    if ( !b.valid() || b.get_type() == sol::type::nil ) b = table["b"];
    if ( !a.valid() || a.get_type() == sol::type::nil ) a = table["a"];
    if ( !readFiniteFloat(r, value.x) || !readFiniteFloat(g, value.y) ||
         !readFiniteFloat(b, value.z) || !readFiniteFloat(a, value.w) ) {
        return false;
    }
    return value.x >= 0.0f && value.x <= 1.0f && value.y >= 0.0f &&
           value.y <= 1.0f && value.z >= 0.0f && value.z <= 1.0f &&
           value.w >= 0.0f && value.w <= 1.0f;
}

/// @brief 将 Lua 方向文本转换为 ImGuiDir。
/// @param object 方向字符串。
/// @param direction 成功时写入的方向。
/// @return 为 None、Left、Right、Up 或 Down 时返回 true。
bool readDirection(const sol::object& object, ImGuiDir& direction)
{
    if ( !object.is<std::string>() ) return false;
    const std::string value = object.as<std::string>();
    if ( value == "None" ) {
        direction = ImGuiDir_None;
    } else if ( value == "Left" ) {
        direction = ImGuiDir_Left;
    } else if ( value == "Right" ) {
        direction = ImGuiDir_Right;
    } else if ( value == "Up" ) {
        direction = ImGuiDir_Up;
    } else if ( value == "Down" ) {
        direction = ImGuiDir_Down;
    } else {
        return false;
    }
    return true;
}

/// @brief 按 Dear ImGui 当前版本的稳定颜色名称查找索引。
/// @param name Lua 颜色字段名称。
/// @return 找到时返回颜色索引，否则返回 ImGuiCol_COUNT。
ImGuiCol findColorIndex(std::string_view name)
{
    for ( int index = 0; index < ImGuiCol_COUNT; ++index ) {
        if ( name == ImGui::GetStyleColorName(index) ) {
            return index;
        }
    }
    return ImGuiCol_COUNT;
}

/// @brief 解析 Colors 表。
/// @param colorsTable Lua 颜色覆盖表。
/// @param patch 目标样式增量。
/// @param error 失败时写入错误。
/// @return 全部颜色字段有效时返回 true。
bool parseColors(const sol::table& colorsTable, ImGuiThemeStylePatch& patch,
                 std::string& error)
{
    for ( const auto& entry : colorsTable ) {
        if ( !entry.first.is<std::string>() ) {
            error = "Colors 仅允许使用字符串键";
            return false;
        }
        const std::string colorName  = entry.first.as<std::string>();
        const ImGuiCol    colorIndex = findColorIndex(colorName);
        if ( colorIndex == ImGuiCol_COUNT ) {
            error = "未知 ImGui 颜色字段: " + colorName;
            return false;
        }
        ImVec4 color;
        if ( !readColor(entry.second, color) ) {
            error = "颜色 " + colorName + " 必须是四个 0 到 1 的有限 RGBA 数值";
            return false;
        }
        patch.colors.push_back({ colorIndex, color });
    }
    return true;
}

/// @brief 解析单个 ImGuiStyle 覆盖表。
/// @param styleTable Lua 样式表。
/// @param patch 目标样式增量。
/// @param error 失败时写入错误。
/// @return 所有字段均受支持且类型有效时返回 true。
bool parseStylePatch(const sol::table& styleTable, ImGuiThemeStylePatch& patch,
                     std::string& error)
{
    for ( const auto& entry : styleTable ) {
        if ( !entry.first.is<std::string>() ) {
            error = "style 仅允许使用字符串键";
            return false;
        }
        const std::string fieldName = entry.first.as<std::string>();
        if ( fieldName == "Colors" ) {
            if ( !entry.second.is<sol::table>() ) {
                error = "style.Colors 必须是表";
                return false;
            }
            if ( !parseColors(entry.second.as<sol::table>(), patch, error) ) {
                return false;
            }
            continue;
        }

        if ( auto field = std::find_if(FLOAT_STYLE_FIELDS.begin(),
                                       FLOAT_STYLE_FIELDS.end(),
                                       [&](const FloatStyleField& candidate) {
                                           return candidate.name == fieldName;
                                       });
             field != FLOAT_STYLE_FIELDS.end() ) {
            float value = 0.0f;
            if ( !readFiniteFloat(entry.second, value) ) {
                error = fieldName + " 必须是有限数值";
                return false;
            }
            patch.floats.push_back({ field->member, value });
            continue;
        }

        if ( auto field = std::find_if(VEC2_STYLE_FIELDS.begin(),
                                       VEC2_STYLE_FIELDS.end(),
                                       [&](const Vec2StyleField& candidate) {
                                           return candidate.name == fieldName;
                                       });
             field != VEC2_STYLE_FIELDS.end() ) {
            ImVec2 value;
            if ( !readVec2(entry.second, value) ) {
                error = fieldName + " 必须是两个有限数值组成的表";
                return false;
            }
            patch.vectors.push_back({ field->member, value });
            continue;
        }

        if ( auto field = std::find_if(BOOL_STYLE_FIELDS.begin(),
                                       BOOL_STYLE_FIELDS.end(),
                                       [&](const BoolStyleField& candidate) {
                                           return candidate.name == fieldName;
                                       });
             field != BOOL_STYLE_FIELDS.end() ) {
            if ( entry.second.get_type() != sol::type::boolean ) {
                error = fieldName + " 必须是布尔值";
                return false;
            }
            patch.booleans.push_back(
                { field->member, entry.second.as<bool>() });
            continue;
        }

        if ( auto field =
                 std::find_if(DIRECTION_STYLE_FIELDS.begin(),
                              DIRECTION_STYLE_FIELDS.end(),
                              [&](const DirectionStyleField& candidate) {
                                  return candidate.name == fieldName;
                              });
             field != DIRECTION_STYLE_FIELDS.end() ) {
            ImGuiDir value = ImGuiDir_None;
            if ( !readDirection(entry.second, value) ) {
                error = fieldName + " 必须是 None、Left、Right、Up 或 Down";
                return false;
            }
            patch.directions.push_back({ field->member, value });
            continue;
        }

        error = "未知 ImGuiStyle 字段: " + fieldName;
        return false;
    }
    return true;
}

/// @brief 从 Lua 主题定义创建主题实例。
/// @param definition 单个主题定义表。
/// @param sourcePath 插件文件路径。
/// @param error 失败时写入错误。
/// @return 成功时返回主题实例。
std::unique_ptr<ImGuiTheme> parseThemeDefinition(
    const sol::table& definition, const std::filesystem::path& sourcePath,
    std::string& error)
{
    const sol::optional<std::string> id = definition["id"];
    if ( !id || id->empty() ) {
        error = "主题定义缺少非空字符串 id";
        return nullptr;
    }

    std::string displayName = definition["name"].get_or(id.value());
    if ( displayName.empty() ) {
        displayName = id.value();
    }
    std::string baseThemeId =
        definition["base"].get_or(std::string("DeepDark"));
    if ( baseThemeId.empty() ) {
        error = "主题 " + id.value() + " 的 base 不能为空";
        return nullptr;
    }

    ImGuiThemeStylePatch patch;
    sol::object          styleObject = definition["style"];
    if ( styleObject.valid() && styleObject.get_type() != sol::type::nil ) {
        if ( !styleObject.is<sol::table>() ) {
            error = "主题 " + id.value() + " 的 style 必须是表";
            return nullptr;
        }
        if ( !parseStylePatch(styleObject.as<sol::table>(), patch, error) ) {
            error = "主题 " + id.value() + ": " + error;
            return nullptr;
        }
    }

    return std::make_unique<ImGuiTheme>(
        id.value(),
        std::move(displayName),
        ImGuiThemeOrigin::Plugin,
        std::move(baseThemeId),
        sourcePath,
        [patch = std::move(patch)](ImGuiStyle& style) { patch.apply(style); });
}

/// @brief 追加插件错误并写入日志。
/// @param result 当前重载结果。
/// @param sourcePath 错误来源文件。
/// @param message 错误说明。
void appendPluginError(ThemePluginReloadResult&     result,
                       const std::filesystem::path& sourcePath,
                       std::string                  message,
                       ThemePluginInfo*             pluginInfo = nullptr)
{
    XERROR("Theme plugin load failed [{}]: {}",
           Config::pathToUtf8(sourcePath),
           message);
    if ( pluginInfo ) {
        ++pluginInfo->errorCount;
        if ( pluginInfo->firstError.empty() ) {
            pluginInfo->firstError = message;
        }
    }
    result.errors.push_back({ sourcePath, std::move(message) });
}

/// @brief 生成主题插件相对于配置根目录的稳定 ID。
/// @param pluginDirectory 主题插件根目录。
/// @param pluginPath 插件 Lua 文件路径。
/// @return 使用通用分隔符的 themes/<相对路径>。
std::string makeThemePluginId(const std::filesystem::path& pluginDirectory,
                              const std::filesystem::path& pluginPath)
{
    const auto relativePath = pluginPath.lexically_relative(pluginDirectory);
    return "themes/" + Config::pathToUtf8Generic(relativePath);
}

}  // namespace

bool ImGuiThemeRegistry::registerBuiltInTheme(std::unique_ptr<ImGuiTheme> theme)
{
    if ( !theme || theme->origin() != ImGuiThemeOrigin::BuiltIn ||
         !theme->baseThemeId().empty() || !isValidThemeId(theme->id()) ||
         contains(theme->id()) ) {
        return false;
    }
    m_themes.push_back(std::move(theme));
    return true;
}

ThemePluginReloadResult ImGuiThemeRegistry::reloadThemePlugins(
    const std::filesystem::path& pluginDirectory,
    std::span<const std::string> disabledPluginIds)
{
    clearPluginThemes();
    m_plugins.clear();
    ThemePluginReloadResult result;

    std::error_code filesystemError;
    std::filesystem::create_directories(pluginDirectory, filesystemError);
    if ( filesystemError ) {
        appendPluginError(result,
                          pluginDirectory,
                          "无法创建主题插件目录: " + filesystemError.message());
        return result;
    }

    std::vector<std::filesystem::path>            pluginFiles;
    std::filesystem::recursive_directory_iterator iterator(
        pluginDirectory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystemError);
    const std::filesystem::recursive_directory_iterator end;
    if ( filesystemError ) {
        appendPluginError(result,
                          pluginDirectory,
                          "无法扫描主题插件目录: " + filesystemError.message());
        return result;
    }

    while ( iterator != end ) {
        const auto      path = iterator->path();
        std::error_code fileError;
        const bool      regularFile = iterator->is_regular_file(fileError);
        if ( fileError ) {
            appendPluginError(
                result, path, "无法读取插件文件状态: " + fileError.message());
        } else if ( regularFile && path.extension() == ".lua" ) {
            pluginFiles.push_back(path);
        }
        iterator.increment(filesystemError);
        if ( filesystemError ) {
            appendPluginError(
                result,
                pluginDirectory,
                "扫描插件目录时发生错误: " + filesystemError.message());
            filesystemError.clear();
        }
    }

    std::sort(pluginFiles.begin(),
              pluginFiles.end(),
              [](const auto& lhs, const auto& rhs) {
                  return Config::pathToUtf8(lhs) < Config::pathToUtf8(rhs);
              });
    result.discoveredPluginFiles = pluginFiles.size();

    for ( const auto& pluginPath : pluginFiles ) {
        const std::string pluginId =
            makeThemePluginId(pluginDirectory, pluginPath);
        const bool enabled = std::find(disabledPluginIds.begin(),
                                       disabledPluginIds.end(),
                                       pluginId) == disabledPluginIds.end();
        m_plugins.push_back({
            .id         = pluginId,
            .sourcePath = pluginPath,
            .enabled    = enabled,
        });
        ThemePluginInfo& pluginInfo = m_plugins.back();
        if ( !enabled ) {
            ++result.disabledPluginFiles;
            continue;
        }

        std::ifstream pluginFile(pluginPath, std::ios::in | std::ios::binary);
        if ( !pluginFile ) {
            ++result.failedThemeCount;
            appendPluginError(
                result, pluginPath, "无法打开 Lua 插件文件", &pluginInfo);
            continue;
        }
        const std::string script((std::istreambuf_iterator<char>(pluginFile)),
                                 std::istreambuf_iterator<char>());

        sol::state lua;
        lua.open_libraries(
            sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
        const std::string chunkName = Config::pathToUtf8(pluginPath);
        sol::protected_function_result scriptResult =
            lua.safe_script(script, sol::script_pass_on_error, chunkName);
        if ( !scriptResult.valid() ) {
            ++result.failedThemeCount;
            const sol::error error = scriptResult;
            appendPluginError(result, pluginPath, error.what(), &pluginInfo);
            continue;
        }

        sol::object pluginObject = scriptResult;
        if ( !pluginObject.is<sol::table>() ) {
            ++result.failedThemeCount;
            appendPluginError(
                result, pluginPath, "插件入口必须返回一个 Lua 表", &pluginInfo);
            continue;
        }
        const sol::table  pluginTable = pluginObject.as<sol::table>();
        const std::string pluginType =
            pluginTable["type"].get_or(std::string());
        if ( pluginType != "theme" ) {
            ++result.failedThemeCount;
            appendPluginError(result,
                              pluginPath,
                              "主题插件的 type 必须为 theme",
                              &pluginInfo);
            continue;
        }

        std::vector<sol::table> definitions;
        sol::object             themesObject = pluginTable["themes"];
        if ( themesObject.valid() &&
             themesObject.get_type() != sol::type::nil ) {
            if ( !themesObject.is<sol::table>() ) {
                ++result.failedThemeCount;
                appendPluginError(result,
                                  pluginPath,
                                  "themes 必须是主题定义数组",
                                  &pluginInfo);
                continue;
            }
            const sol::table themes = themesObject.as<sol::table>();
            for ( std::size_t index = 1;; ++index ) {
                sol::object definition = themes[index];
                if ( !definition.valid() ||
                     definition.get_type() == sol::type::nil ) {
                    break;
                }
                if ( !definition.is<sol::table>() ) {
                    ++result.failedThemeCount;
                    appendPluginError(result,
                                      pluginPath,
                                      "themes[" + std::to_string(index) +
                                          "] 必须是主题定义表",
                                      &pluginInfo);
                    continue;
                }
                definitions.push_back(definition.as<sol::table>());
            }
        } else {
            definitions.push_back(pluginTable);
        }

        if ( definitions.empty() ) {
            ++result.failedThemeCount;
            appendPluginError(
                result, pluginPath, "插件没有定义任何主题", &pluginInfo);
            continue;
        }

        for ( const auto& definition : definitions ) {
            std::string error;
            auto theme = parseThemeDefinition(definition, pluginPath, error);
            if ( !theme ) {
                ++result.failedThemeCount;
                appendPluginError(
                    result, pluginPath, std::move(error), &pluginInfo);
                continue;
            }
            const std::string themeId(theme->id());
            if ( !registerPluginTheme(std::move(theme)) ) {
                ++result.failedThemeCount;
                appendPluginError(
                    result,
                    pluginPath,
                    "主题 ID 重复、非法或 base 不是已注册内置主题: " + themeId,
                    &pluginInfo);
                continue;
            }
            ++result.loadedThemeCount;
            ++pluginInfo.loadedThemeCount;
        }
    }

    XINFO(
        "Theme plugins reloaded: {} file(s), {} disabled, {} theme(s), {} "
        "error(s)",
        result.discoveredPluginFiles,
        result.disabledPluginFiles,
        result.loadedThemeCount,
        result.errors.size());
    return result;
}

const ThemePluginInfo* ImGuiThemeRegistry::findPlugin(std::string_view id) const
{
    const auto plugin =
        std::find_if(m_plugins.begin(), m_plugins.end(), [&](const auto& item) {
            return item.id == id;
        });
    return plugin == m_plugins.end() ? nullptr : &*plugin;
}

const ImGuiTheme* ImGuiThemeRegistry::findTheme(std::string_view id) const
{
    const auto theme =
        std::find_if(m_themes.begin(), m_themes.end(), [&](const auto& item) {
            return item && item->id() == id;
        });
    return theme == m_themes.end() ? nullptr : theme->get();
}

bool ImGuiThemeRegistry::applyTheme(std::string_view id,
                                    ImGuiStyle&      style) const
{
    const ImGuiTheme* theme = findTheme(id);
    if ( !theme ) return false;

    style = ImGuiStyle();
    if ( theme->origin() == ImGuiThemeOrigin::Plugin ) {
        const ImGuiTheme* baseTheme = findTheme(theme->baseThemeId());
        if ( !baseTheme || baseTheme->origin() != ImGuiThemeOrigin::BuiltIn ) {
            return false;
        }
        baseTheme->apply(style);
    }
    theme->apply(style);
    return true;
}

bool ImGuiThemeRegistry::registerPluginTheme(std::unique_ptr<ImGuiTheme> theme)
{
    if ( !theme || theme->origin() != ImGuiThemeOrigin::Plugin ||
         !isValidThemeId(theme->id()) || contains(theme->id()) ) {
        return false;
    }
    const ImGuiTheme* baseTheme = findTheme(theme->baseThemeId());
    if ( !baseTheme || baseTheme->origin() != ImGuiThemeOrigin::BuiltIn ) {
        return false;
    }
    m_themes.push_back(std::move(theme));
    return true;
}

void ImGuiThemeRegistry::clearPluginThemes()
{
    std::erase_if(m_themes, [](const auto& theme) {
        return theme && theme->origin() == ImGuiThemeOrigin::Plugin;
    });
}

bool ImGuiThemeRegistry::isValidThemeId(std::string_view id)
{
    if ( id.empty() || id.size() > 128 || id == Config::UI_THEME_AUTO_ID ) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' ||
               character == '_' || character == '-';
    });
}

}  // namespace MMM::Graphic
