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
    // 该白名单与当前 Dear ImGui ImGuiStyle 版本同步，Lua
    // 不能按偏移访问任意成员。
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
    // 二维字段接受数组 {x, y} 或命名表 {x=..., y=...} 两种输入形式。
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
    // 布尔字段要求 Lua boolean，不接受数字 0/1 的隐式转换。
    BoolStyleField{ "DockingNodeHasCloseButton",
                    &ImGuiStyle::DockingNodeHasCloseButton },
    BoolStyleField{ "AntiAliasedLines", &ImGuiStyle::AntiAliasedLines },
    BoolStyleField{ "AntiAliasedLinesUseTex",
                    &ImGuiStyle::AntiAliasedLinesUseTex },
    BoolStyleField{ "AntiAliasedFill", &ImGuiStyle::AntiAliasedFill },
};

constexpr std::array DIRECTION_STYLE_FIELDS{
    // 枚举仅通过稳定文本名称暴露，避免插件依赖 ImGuiDir 的数值 ABI。
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
    ///
    /// Patch
    /// 按字段类型分别保存，应用时只覆盖插件显式声明的成员；未出现的字段保留
    /// 已应用基底主题或 ImGui 默认值。颜色索引在解析阶段已验证。
    ///
    /// @param style 目标样式。
    /// @warning 主题切换低频路径：线性遍历已解析的小型增量，不访问 Lua
    /// 或文件系统。
    void apply(ImGuiStyle& style) const
    {
        // 成员指针避免为每个字段维护重复 setter 分支，类型由对应 vector 保证。
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
        // Colors 数组按已验证 ImGuiCol 索引更新，不触碰 ImGuiCol_COUNT
        // 之外内存。
        for ( const auto& overrideValue : colors ) {
            style.Colors[overrideValue.index] = overrideValue.value;
        }
    }
};

/// @brief 判断 Lua 对象是否为有限数值。
///
/// 先以 double 读取 Lua number，再验证转换为 float 后仍有限，防止超出 float
/// 范围的 有限 double 在样式中变为 inf。
///
/// @param object 待检查对象。
/// @param value 成功时写入的浮点值。
/// @return 对象为有限数值时返回 true。
bool readFiniteFloat(const sol::object& object, float& value)
{
    // 不接受字符串数值或 boolean 的 Lua 隐式转换，主题 schema 保持严格。
    if ( object.get_type() != sol::type::number ) return false;
    const double parsed = object.as<double>();
    // NaN/Inf 会破坏 ImGui 布局和裁剪计算，解析阶段直接拒绝。
    if ( !std::isfinite(parsed) ) return false;
    value = static_cast<float>(parsed);
    // double 到 float 的窄化仍可能溢出，再次验证输出表示。
    return std::isfinite(value);
}

/// @brief 从 Lua 表读取二维向量。
///
/// 优先读取 1-based 数组项，缺失时分别回退到 x/y 命名字段；两分量必须独立通过
/// readFiniteFloat。混合表示也允许，便于只为一个分量使用命名键。
///
/// @param object 数组或带 x、y 字段的表。
/// @param value 成功时写入的向量。
/// @return 格式有效时返回 true。
bool readVec2(const sol::object& object, ImVec2& value)
{
    // 非 table 不执行索引，避免 sol2 类型转换产生脚本错误。
    if ( !object.is<sol::table>() ) return false;
    const sol::table table = object.as<sol::table>();
    sol::object      x     = table[1];
    sol::object      y     = table[2];
    // Lua 数组从 1 开始；nil 与无效 object 都表示需要查找命名键。
    if ( !x.valid() || x.get_type() == sol::type::lua_nil ) x = table["x"];
    if ( !y.valid() || y.get_type() == sol::type::lua_nil ) y = table["y"];
    return readFiniteFloat(x, value.x) && readFiniteFloat(y, value.y);
}

/// @brief 从 Lua 表读取 RGBA 颜色。
///
/// 支持 {r,g,b,a} 的 1-based 数组和同名字段形式。所有分量既要有限，也必须位于
/// ImGui 颜色约定的闭区间 [0, 1]，不在加载时隐式钳制错误输入。
///
/// @param object 数组或带 r、g、b、a 字段的表。
/// @param value 成功时写入的颜色。
/// @return 四个分量均位于 0 到 1 时返回 true。
bool readColor(const sol::object& object, ImVec4& value)
{
    // 只有 table 才能同时表达四个颜色分量。
    if ( !object.is<sol::table>() ) return false;
    const sol::table table = object.as<sol::table>();
    sol::object      r     = table[1];
    sol::object      g     = table[2];
    sol::object      b     = table[3];
    sol::object      a     = table[4];
    // 数组项优先，缺失项逐个回退到命名字段，允许两种风格混合。
    if ( !r.valid() || r.get_type() == sol::type::lua_nil ) r = table["r"];
    if ( !g.valid() || g.get_type() == sol::type::lua_nil ) g = table["g"];
    if ( !b.valid() || b.get_type() == sol::type::lua_nil ) b = table["b"];
    if ( !a.valid() || a.get_type() == sol::type::lua_nil ) a = table["a"];
    // 先完成类型/有限性验证，任何分量失败都不接受部分颜色。
    if ( !readFiniteFloat(r, value.x) || !readFiniteFloat(g, value.y) ||
         !readFiniteFloat(b, value.z) || !readFiniteFloat(a, value.w) ) {
        return false;
    }
    // 范围检查与读取分离，使错误信息统一指向完整 RGBA schema。
    return value.x >= 0.0f && value.x <= 1.0f && value.y >= 0.0f &&
           value.y <= 1.0f && value.z >= 0.0f && value.z <= 1.0f &&
           value.w >= 0.0f && value.w <= 1.0f;
}

/// @brief 将 Lua 方向文本转换为 ImGuiDir。
///
/// 插件只依赖五个稳定英文枚举名称，不暴露 Dear ImGui 内部整数值。大小写必须
/// 精确匹配，未知字符串返回 false。
///
/// @param object 方向字符串。
/// @param direction 成功时写入的方向。
/// @return 为 None、Left、Right、Up 或 Down 时返回 true。
bool readDirection(const sol::object& object, ImGuiDir& direction)
{
    // 不接受 Lua number 到 enum 的直接转换，避免跨 ImGui 版本数值漂移。
    if ( !object.is<std::string>() ) return false;
    const std::string value = object.as<std::string>();
    // 显式分支完整列出主题 schema 当前允许的方向集合。
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
///
/// 颜色名称由运行时 ImGui::GetStyleColorName
/// 提供，白名单会自然跟随当前编译版本，
/// 插件无需依赖 ImGuiCol 枚举整数。
///
/// @param name Lua 颜色字段名称。
/// @return 找到时返回颜色索引，否则返回 ImGuiCol_COUNT。
ImGuiCol findColorIndex(std::string_view name)
{
    // ImGuiCol_COUNT 是唯一上界和未找到哨兵，逐项比较只发生在插件重载。
    for ( int index = 0; index < ImGuiCol_COUNT; ++index ) {
        if ( name == ImGui::GetStyleColorName(index) ) {
            return index;
        }
    }
    // 返回 COUNT 而非负值，便于调用者直接与合法枚举上界比较。
    return ImGuiCol_COUNT;
}

/// @brief 解析 Colors 表。
///
/// Colors 必须是“ImGui 稳定颜色名 -> RGBA 表”的映射。解析只构造 patch，不修改
/// 全局或目标 ImGuiStyle；遇到第一个未知键或非法颜色即返回明确错误。
///
/// @param colorsTable Lua 颜色覆盖表。
/// @param patch 目标样式增量。
/// @param error 失败时写入错误。
/// @return 全部颜色字段有效时返回 true。
bool parseColors(const sol::table& colorsTable, ImGuiThemeStylePatch& patch,
                 std::string& error)
{
    // sol::table 迭代顺序不影响结果，各颜色索引最终独立写入。
    for ( const auto& entry : colorsTable ) {
        // 数值键会使配置难以审计且无法映射稳定 ImGui 名称，因此严格拒绝。
        if ( !entry.first.is<std::string>() ) {
            error = "Colors 仅允许使用字符串键";
            return false;
        }
        // 查找只针对当前编译版本公开的 ImGuiCol 名称。
        const std::string colorName  = entry.first.as<std::string>();
        const ImGuiCol    colorIndex = findColorIndex(colorName);
        if ( colorIndex == ImGuiCol_COUNT ) {
            error = "未知 ImGui 颜色字段: " + colorName;
            return false;
        }
        // RGBA 在加入 patch 前必须完整通过类型、有限性和 [0,1] 范围检查。
        ImVec4 color;
        if ( !readColor(entry.second, color) ) {
            error = "颜色 " + colorName + " 必须是四个 0 到 1 的有限 RGBA 数值";
            return false;
        }
        // Patch 保存值副本，不保留任何 Lua object/table 生命周期。
        patch.colors.push_back({ colorIndex, color });
    }
    return true;
}

/// @brief 解析单个 ImGuiStyle 覆盖表。
///
/// 顶层键只允许 Colors 或四类字段白名单。每个值按目标成员类型严格验证并追加到
/// ImGuiThemeStylePatch；未知字段不会静默忽略，避免拼写错误产生部分主题。
///
/// @param styleTable Lua 样式表。
/// @param patch 目标样式增量。
/// @param error 失败时写入错误。
/// @return 所有字段均受支持且类型有效时返回 true。
bool parseStylePatch(const sol::table& styleTable, ImGuiThemeStylePatch& patch,
                     std::string& error)
{
    // 输入表只在插件加载期间存在，patch 必须复制出所有持久数据。
    for ( const auto& entry : styleTable ) {
        // 主题 schema 使用成员名作为字符串键，不支持数组式顶层样式。
        if ( !entry.first.is<std::string>() ) {
            error = "style 仅允许使用字符串键";
            return false;
        }
        const std::string fieldName = entry.first.as<std::string>();
        // Colors 是唯一嵌套的专用字段集合，交给颜色解析器处理。
        if ( fieldName == "Colors" ) {
            if ( !entry.second.is<sol::table>() ) {
                error = "style.Colors 必须是表";
                return false;
            }
            if ( !parseColors(entry.second.as<sol::table>(), patch, error) ) {
                return false;
            }
            // 成功后继续解析同一 style 表的其他标量/向量字段。
            continue;
        }

        // 浮点白名单把文本名直接映射到类型安全的 ImGuiStyle 成员指针。
        if ( auto field = std::find_if(FLOAT_STYLE_FIELDS.begin(),
                                       FLOAT_STYLE_FIELDS.end(),
                                       [&](const FloatStyleField& candidate) {
                                           return candidate.name == fieldName;
                                       });
             field != FLOAT_STYLE_FIELDS.end() ) {
            float value = 0.0f;
            // 不进行隐式字符串转换或范围钳制，只要求结果可由有限 float 表示。
            if ( !readFiniteFloat(entry.second, value) ) {
                error = fieldName + " 必须是有限数值";
                return false;
            }
            // 保存成员指针和值，稍后在基底主题之上按增量应用。
            patch.floats.push_back({ field->member, value });
            continue;
        }

        // 二维字段复用数组/命名键兼容解析规则。
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

        // 布尔字段仅接受 Lua boolean，避免 0、nil 等值被静默强制转换。
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

        // 方向字段只接受 readDirection 定义的稳定文本值。
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

        // 严格拒绝未知成员，让插件作者能立刻发现版本差异或拼写错误。
        error = "未知 ImGuiStyle 字段: " + fieldName;
        return false;
    }
    return true;
}

/// @brief 从 Lua 主题定义创建主题实例。
///
/// 定义必须提供合法的非空 id，可选 name 默认为 id，可选 base 默认为
/// DeepDark。style 被解析为完全脱离 Lua state 生命周期的值语义
/// patch，并捕获到主题 apply 回调中。主题来源固定标记为 Plugin。
///
/// @param definition 单个主题定义表。
/// @param sourcePath 插件文件路径。
/// @param error 失败时写入错误。
/// @return 成功时返回主题实例。
std::unique_ptr<ImGuiTheme> parseThemeDefinition(
    const sol::table& definition, const std::filesystem::path& sourcePath,
    std::string& error)
{
    // sol::optional 同时拒绝缺失字段和错误类型转换，空字符串另行禁止。
    const sol::optional<std::string> id = definition["id"];
    if ( !id || id->empty() ) {
        error = "主题定义缺少非空字符串 id";
        return nullptr;
    }

    // 展示名称允许缺省，但最终保证非空，避免 UI 列表出现不可识别项。
    std::string displayName = definition["name"].get_or(id.value());
    if ( displayName.empty() ) {
        displayName = id.value();
    }
    // 插件主题只能增量继承已注册内置主题，默认选择项目标准 DeepDark。
    std::string baseThemeId =
        definition["base"].get_or(std::string("DeepDark"));
    if ( baseThemeId.empty() ) {
        error = "主题 " + id.value() + " 的 base 不能为空";
        return nullptr;
    }

    // style 整体可省略，表示只重命名/继承基底；存在时必须是 table。
    ImGuiThemeStylePatch patch;
    sol::object          styleObject = definition["style"];
    if ( styleObject.valid() && styleObject.get_type() != sol::type::lua_nil ) {
        if ( !styleObject.is<sol::table>() ) {
            error = "主题 " + id.value() + " 的 style 必须是表";
            return nullptr;
        }
        if ( !parseStylePatch(styleObject.as<sol::table>(), patch, error) ) {
            // 在字段错误前加主题 ID，批量插件日志可准确定位定义。
            error = "主题 " + id.value() + ": " + error;
            return nullptr;
        }
    }

    // lambda 按值拥有 patch，Lua state
    // 在单个插件文件循环结束时销毁也不影响主题。
    return std::make_unique<ImGuiTheme>(
        id.value(),
        std::move(displayName),
        ImGuiThemeOrigin::Plugin,
        std::move(baseThemeId),
        sourcePath,
        [patch = std::move(patch)](ImGuiStyle& style) { patch.apply(style); });
}

/// @brief 追加插件错误并写入日志。
///
/// 错误同时进入全局 reload
/// 结果和可选单文件状态；文件状态保存首个错误便于列表摘要，全局 errors
/// 保留全部诊断。message 最后移动进结果以避免额外复制。
///
/// @param result 当前重载结果。
/// @param sourcePath 错误来源文件。
/// @param message 错误说明。
void appendPluginError(ThemePluginReloadResult&     result,
                       const std::filesystem::path& sourcePath,
                       std::string                  message,
                       ThemePluginInfo*             pluginInfo = nullptr)
{
    // 路径统一转换为 UTF-8，日志在 Windows 与 Unix 上保持一致。
    XERROR("Theme plugin load failed [{}]: {}",
           Config::pathToUtf8(sourcePath),
           message);
    if ( pluginInfo ) {
        // 同一文件可包含多个主题定义，因此错误计数可能大于一。
        ++pluginInfo->errorCount;
        // 仅保存首个摘要，详细错误仍全部进入 result.errors。
        if ( pluginInfo->firstError.empty() ) {
            pluginInfo->firstError = message;
        }
    }
    result.errors.push_back({ sourcePath, std::move(message) });
}

/// @brief 生成主题插件相对于配置根目录的稳定 ID。
///
/// ID
/// 不包含机器相关的绝对配置路径，并强制使用通用斜杠，可直接与持久化禁用列表
/// 跨平台比较。
///
/// @param pluginDirectory 主题插件根目录。
/// @param pluginPath 插件 Lua 文件路径。
/// @return 使用通用分隔符的 themes/<相对路径>。
std::string makeThemePluginId(const std::filesystem::path& pluginDirectory,
                              const std::filesystem::path& pluginPath)
{
    // lexically_relative 不访问文件系统，目录扫描已保证 pluginPath
    // 位于根目录下。
    const auto relativePath = pluginPath.lexically_relative(pluginDirectory);
    return "themes/" + Config::pathToUtf8Generic(relativePath);
}

}  // namespace

/// @brief 接管一个无基底、ID 合法且未重复的内置主题。
/// @param theme 待注册的 owning pointer。
/// @return 满足内置来源、不继承其他主题及唯一 ID 约束时返回 true。
bool ImGuiThemeRegistry::registerBuiltInTheme(std::unique_ptr<ImGuiTheme> theme)
{
    // 内置主题是继承链根节点，baseThemeId 必须为空；Auto
    // 是设置哨兵而非实体主题。
    if ( !theme || theme->origin() != ImGuiThemeOrigin::BuiltIn ||
         !theme->baseThemeId().empty() || !isValidThemeId(theme->id()) ||
         contains(theme->id()) ) {
        return false;
    }
    // vector 保留注册顺序，UI 主题列表据此稳定展示内置项。
    m_themes.push_back(std::move(theme));
    return true;
}

/// @brief 清除旧插件主题并确定性地重扫、执行和注册目录中的 Lua 主题插件。
///
/// 扫描使用 error_code 和 skip_permission_denied，避免异常机制；Lua 文件先按
/// UTF-8 路径排序，再逐个建立隔离 sol::state
/// 执行。单文件可返回一个主题定义或带 themes 数组的插件表，局部错误被收集但不会
/// 阻止后续文件和定义继续加载。
///
/// @param pluginDirectory 配置根目录下的主题插件目录。
/// @param disabledPluginIds 按稳定相对 ID 禁止执行的插件集合。
/// @return 文件发现、禁用、成功主题和全部错误的本轮快照。
/// @warning 低频显式重载路径：创建目录、递归扫描、读取文件并执行 Lua，禁止从
/// UI 或渲染每帧调用。
ThemePluginReloadResult ImGuiThemeRegistry::reloadThemePlugins(
    const std::filesystem::path& pluginDirectory,
    std::span<const std::string> disabledPluginIds)
{
    // 每轮从仅含内置主题的确定基线开始，旧插件对象和文件状态不跨重载保留。
    clearPluginThemes();
    m_plugins.clear();
    ThemePluginReloadResult result;

    // 使用 error_code 重载避免 filesystem exception，符合项目禁用异常约束。
    std::error_code filesystemError;
    std::filesystem::create_directories(pluginDirectory, filesystemError);
    if ( filesystemError ) {
        // 根目录无法建立时没有可继续扫描的安全目标，返回目录级错误。
        appendPluginError(result,
                          pluginDirectory,
                          "无法创建主题插件目录: " + filesystemError.message());
        return result;
    }

    // 先只收集路径，排序后再执行，避免文件系统遍历顺序影响重复 ID 的胜出者。
    std::vector<std::filesystem::path>            pluginFiles;
    std::filesystem::recursive_directory_iterator iterator(
        pluginDirectory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystemError);
    const std::filesystem::recursive_directory_iterator end;
    if ( filesystemError ) {
        // iterator 构造失败表示无法进入根目录，本轮直接结束。
        appendPluginError(result,
                          pluginDirectory,
                          "无法扫描主题插件目录: " + filesystemError.message());
        return result;
    }

    while ( iterator != end ) {
        // 每个条目的状态查询使用独立
        // error_code，单点权限错误不终止整个递归扫描。
        const auto      path = iterator->path();
        std::error_code fileError;
        const bool      regularFile = iterator->is_regular_file(fileError);
        if ( fileError ) {
            appendPluginError(
                result, path, "无法读取插件文件状态: " + fileError.message());
        } else if ( regularFile && path.extension() == ".lua" ) {
            // 只执行普通 .lua
            // 文件，目录、符号目标的其他资源和大小写不同扩展跳过。
            pluginFiles.push_back(path);
        }
        // 显式 increment(error_code) 避免递归迭代器抛出异常。
        iterator.increment(filesystemError);
        if ( filesystemError ) {
            appendPluginError(
                result,
                pluginDirectory,
                "扫描插件目录时发生错误: " + filesystemError.message());
            // 清错后继续遍历 iterator 能到达的后续条目。
            filesystemError.clear();
        }
    }

    // UTF-8 路径排序为重复主题 ID、插件列表和测试提供跨运行确定顺序。
    std::sort(pluginFiles.begin(),
              pluginFiles.end(),
              [](const auto& lhs, const auto& rhs) {
                  return Config::pathToUtf8(lhs) < Config::pathToUtf8(rhs);
              });
    // discovered 包括之后被用户禁用或执行失败的全部 Lua 文件。
    result.discoveredPluginFiles = pluginFiles.size();

    for ( const auto& pluginPath : pluginFiles ) {
        // 稳定 ID 与绝对配置根无关，可直接匹配设置中持久化的禁用列表。
        const std::string pluginId =
            makeThemePluginId(pluginDirectory, pluginPath);
        // disabledPluginIds
        // 采用精确匹配；禁用目录前缀不会隐式禁用其下其他插件。
        const bool enabled = std::find(disabledPluginIds.begin(),
                                       disabledPluginIds.end(),
                                       pluginId) == disabledPluginIds.end();
        // 无论启用与否都记录插件信息，使设置界面能展示完整发现列表。
        m_plugins.push_back({
            .id         = pluginId,
            .sourcePath = pluginPath,
            .enabled    = enabled,
        });
        ThemePluginInfo& pluginInfo = m_plugins.back();
        if ( !enabled ) {
            // 禁用插件完全不读取也不执行，避免其副作用和语法错误影响本轮。
            ++result.disabledPluginFiles;
            continue;
        }

        // 二进制模式保留脚本原始字节和换行，Lua 按 UTF-8 文本自行解析。
        std::ifstream pluginFile(pluginPath, std::ios::in | std::ios::binary);
        if ( !pluginFile ) {
            // 文件级打开失败计入 failedThemeCount
            // 一次，因为尚不能知道内部定义数。
            ++result.failedThemeCount;
            appendPluginError(
                result, pluginPath, "无法打开 Lua 插件文件", &pluginInfo);
            continue;
        }
        // 文件内容完整读入当前迭代局部字符串，执行完成后即可释放。
        const std::string script((std::istreambuf_iterator<char>(pluginFile)),
                                 std::istreambuf_iterator<char>());

        // 每个插件使用独立 Lua state，插件之间不能通过全局变量隐式耦合。
        sol::state lua;
        // 仅开放声明主题所需的基础/table/string/math 库，不开放 io、os 或
        // package。
        lua.open_libraries(
            sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
        const std::string chunkName = Config::pathToUtf8(pluginPath);
        // safe_script 将 Lua 错误封装为 protected result，不通过 C++ 异常传播。
        sol::protected_function_result scriptResult =
            lua.safe_script(script, sol::script_pass_on_error, chunkName);
        if ( !scriptResult.valid() ) {
            // 脚本级错误计为一个失败主题，并保留 Lua 提供的诊断文本。
            // protected result 在转换为 sol::error 后仍由当前 lua state
            // 管理相关对象， 因此诊断在离开文件循环前复制进 C++ 字符串。
            ++result.failedThemeCount;
            const sol::error error = scriptResult;
            appendPluginError(result, pluginPath, error.what(), &pluginInfo);
            continue;
        }

        // 插件入口必须以 return 提供描述表，不能依赖全局注册副作用。
        sol::object pluginObject = scriptResult;
        if ( !pluginObject.is<sol::table>() ) {
            ++result.failedThemeCount;
            appendPluginError(
                result, pluginPath, "插件入口必须返回一个 Lua 表", &pluginInfo);
            continue;
        }
        const sol::table pluginTable = pluginObject.as<sol::table>();
        // type 字段防止将其他 Lua 插件种类误当作主题定义执行。
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

        // 支持一个文件返回 themes 数组，也支持把插件表自身作为单个主题定义。
        std::vector<sol::table> definitions;
        sol::object             themesObject = pluginTable["themes"];
        if ( themesObject.valid() &&
             themesObject.get_type() != sol::type::lua_nil ) {
            if ( !themesObject.is<sol::table>() ) {
                ++result.failedThemeCount;
                appendPluginError(result,
                                  pluginPath,
                                  "themes 必须是主题定义数组",
                                  &pluginInfo);
                continue;
            }
            const sol::table themes = themesObject.as<sol::table>();
            // 数组从索引 1 连续读取，第一个 nil 终止；散列键不属于定义数组。
            for ( std::size_t index = 1;; ++index ) {
                sol::object definition = themes[index];
                if ( !definition.valid() ||
                     definition.get_type() == sol::type::lua_nil ) {
                    break;
                }
                if ( !definition.is<sol::table>() ) {
                    // 单个坏元素不丢弃同文件其他定义，记录后继续扫描后续索引。
                    ++result.failedThemeCount;
                    appendPluginError(result,
                                      pluginPath,
                                      "themes[" + std::to_string(index) +
                                          "] 必须是主题定义表",
                                      &pluginInfo);
                    continue;
                }
                // table 句柄只在当前 lua state 生命周期内使用，随后立即解析为
                // C++ 值。
                definitions.push_back(definition.as<sol::table>());
            }
        } else {
            // 单主题简写复用插件表的 id/name/base/style 字段，type
            // 字段会被样式解析 之外的主题定义逻辑自然忽略。
            definitions.push_back(pluginTable);
        }

        if ( definitions.empty() ) {
            // 显式空 themes 数组不是有效插件，避免无声加载成功。
            ++result.failedThemeCount;
            appendPluginError(
                result, pluginPath, "插件没有定义任何主题", &pluginInfo);
            continue;
        }

        for ( const auto& definition : definitions ) {
            // 每个定义独立解析和注册，一个错误不会回滚同文件已成功的其他主题。
            std::string error;
            auto theme = parseThemeDefinition(definition, pluginPath, error);
            if ( !theme ) {
                ++result.failedThemeCount;
                appendPluginError(
                    result, pluginPath, std::move(error), &pluginInfo);
                continue;
            }
            // 移动 owning pointer 前复制 ID，注册失败时仍能生成准确诊断。
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
            // 全局与单文件计数同步递增，均只统计真正进入 registry 的主题。
            ++result.loadedThemeCount;
            ++pluginInfo.loadedThemeCount;
        }
    }

    // 汇总日志便于重载入口展示整体结果，详细错误已在发生时逐条记录。
    // failedThemeCount 表示无法载入的定义/文件入口数，errors.size()
    // 是实际诊断条数； 两者通常一致，但保持独立字段便于未来扩展非致命警告。
    XINFO(
        "Theme plugins reloaded: {} file(s), {} disabled, {} theme(s), {} "
        "error(s)",
        result.discoveredPluginFiles,
        result.disabledPluginFiles,
        result.loadedThemeCount,
        result.errors.size());
    return result;
}

/// @brief 按稳定插件 ID 查找最近一次扫描状态。
/// @param id 形如 themes/subdir/plugin.lua 的配置根相对 ID。
/// @return 找到时返回由 m_plugins 持有的观察指针，否则返回 nullptr。
/// @warning 低频设置查询路径：线性扫描插件列表；下一次 reload
/// 会使返回指针失效。
const ThemePluginInfo* ImGuiThemeRegistry::findPlugin(std::string_view id) const
{
    // m_plugins 已按路径排序，当前规模较小时线性查找保持数据结构简单。
    const auto plugin =
        std::find_if(m_plugins.begin(), m_plugins.end(), [&](const auto& item) {
            return item.id == id;
        });
    return plugin == m_plugins.end() ? nullptr : &*plugin;
}

/// @brief 按主题 ID 查找内置或插件主题。
/// @param id 持久化配置使用的主题 ID。
/// @return 找到时返回由 registry unique_ptr 持有的观察指针，否则返回 nullptr。
/// @warning 主题查询路径：线性扫描稳定注册顺序；清理插件主题会使对应指针失效。
const ImGuiTheme* ImGuiThemeRegistry::findTheme(std::string_view id) const
{
    // 防御 vector 中空 unique_ptr，只比较有效主题 ID。
    const auto theme =
        std::find_if(m_themes.begin(), m_themes.end(), [&](const auto& item) {
            return item && item->id() == id;
        });
    return theme == m_themes.end() ? nullptr : theme->get();
}

/// @brief 从 ImGui 默认样式开始应用指定主题及其必要内置基底。
///
/// 每次先重置为 ImGuiStyle
/// 默认值，防止前一个主题未覆盖的字段泄漏。插件主题只
/// 允许继承一个内置主题，先应用基底再应用 patch；内置主题直接应用自身。
///
/// @param id 要应用的已注册主题 ID。
/// @param style 接收完整结果的样式对象。
/// @return 主题存在且插件基底仍为有效内置主题时返回 true。
bool ImGuiThemeRegistry::applyTheme(std::string_view id,
                                    ImGuiStyle&      style) const
{
    // 找不到主题时保持调用方 style 不变。
    const ImGuiTheme* theme = findTheme(id);
    if ( !theme ) return false;

    // 只有主题存在后才重置，失败查找不会意外清空当前 UI 样式。
    style = ImGuiStyle();
    if ( theme->origin() == ImGuiThemeOrigin::Plugin ) {
        // 注册时已经验证 base；应用时重新检查防御 registry 状态变化。
        const ImGuiTheme* baseTheme = findTheme(theme->baseThemeId());
        if ( !baseTheme || baseTheme->origin() != ImGuiThemeOrigin::BuiltIn ) {
            return false;
        }
        // 插件增量的未声明字段继承内置基底，而非上一个活动主题。
        baseTheme->apply(style);
    }
    // 内置主题覆盖默认 style；插件主题在基底之上只写解析出的 patch。
    theme->apply(style);
    return true;
}

/// @brief 接管 ID 唯一且继承已注册内置基底的插件主题。
/// @param theme 待注册的 owning pointer。
/// @return 来源、ID、唯一性与基底约束全部满足时返回 true。
bool ImGuiThemeRegistry::registerPluginTheme(std::unique_ptr<ImGuiTheme> theme)
{
    // 插件不可伪装为内置主题，也不可使用 Auto 哨兵或覆盖任何现有 ID。
    if ( !theme || theme->origin() != ImGuiThemeOrigin::Plugin ||
         !isValidThemeId(theme->id()) || contains(theme->id()) ) {
        return false;
    }
    // 基底必须已经注册且来源为 BuiltIn，禁止插件链式继承和循环依赖。
    const ImGuiTheme* baseTheme = findTheme(theme->baseThemeId());
    if ( !baseTheme || baseTheme->origin() != ImGuiThemeOrigin::BuiltIn ) {
        return false;
    }
    // 插件按确定的文件/定义处理顺序追加在内置主题之后。
    m_themes.push_back(std::move(theme));
    return true;
}

/// @brief 删除所有插件来源主题，保留内置主题和其相对顺序。
/// @warning 低频重载路径：线性压缩 owning
/// vector，会使所有插件主题观察指针失效。
void ImGuiThemeRegistry::clearPluginThemes()
{
    // erase_if 销毁匹配 unique_ptr，内置条目不移动出相对顺序。
    std::erase_if(m_themes, [](const auto& theme) {
        return theme && theme->origin() == ImGuiThemeOrigin::Plugin;
    });
}

/// @brief 验证主题 ID 可安全用于设置持久化、日志和精确查找。
///
/// ID 长度限制为 1..128，只允许 ASCII
/// 字母、数字、点、下划线和连字符，并明确排除 UI_THEME_AUTO_ID
/// 设置哨兵。
///
/// @param id 待验证主题 ID。
/// @return 满足长度、字符集和保留值约束时返回 true。
bool ImGuiThemeRegistry::isValidThemeId(std::string_view id)
{
    // Auto 代表按系统/设置选择，不对应 registry 中的实际主题实例。
    if ( id.empty() || id.size() > 128 || id == Config::UI_THEME_AUTO_ID ) {
        return false;
    }
    // unsigned char 避免非 ASCII 负 char 参与范围比较产生实现相关结果。
    return std::all_of(id.begin(), id.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' ||
               character == '_' || character == '-';
    });
}

}  // namespace MMM::Graphic
