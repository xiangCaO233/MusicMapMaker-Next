#include "graphic/theme/ImGuiThemeRegistry.h"

#include "log/colorful-log.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

namespace
{

/// @brief 记录主题插件测试断言失败。
///
/// 测试不使用异常或外部断言框架，所有场景通过布尔值累积失败；只有失败条件写入
/// 日志，使成功运行保持安静且主函数最终返回统一退出码。
///
/// @param condition 待验证条件。
/// @param message 失败说明。
/// @return 条件原值。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) {
        XERROR("ThemePluginLoaderTest failed: {}", message);
    }
    return condition;
}

/// @brief 判断两个浮点值近似相等。
///
/// Lua number 转换为 float 后允许固定小误差，避免用二进制浮点精确相等判断主题
/// 字段解析是否正确。
///
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 误差小于阈值时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 0.0001f;
}

/// @brief 写入 Lua 测试插件。
///
/// 使用二进制截断模式确保每次重载看到完整的新脚本，不受平台换行转换或上一轮
/// 较长文件尾部残留影响。
///
/// @param path 输出路径。
/// @param content Lua 源码。
/// @return 写入成功时返回 true。
bool writePlugin(const std::filesystem::path& path, std::string_view content)
{
    // 流状态同时覆盖目录不存在、权限错误和打开失败，不使用异常文件系统路径。
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    // 写入完成后检查最终流状态，避免把短写误报为可加载夹具。
    file << content;
    return file.good();
}

/// @brief 注册测试使用的最小 DeepDark 与 Light 内置主题。
///
/// 两个主题只设置后续场景会观察的字段：DeepDark 验证插件覆盖与未覆盖字段继承，
/// Light 保证注册表存在多个合法内置项而非依赖单主题特例。
///
/// @param registry 目标注册表。
/// @return 两个基底主题均注册成功时返回 true。
bool registerBaseTheme(MMM::Graphic::ImGuiThemeRegistry& registry)
{
    // DeepDark 同时给出标量、向量和颜色基值，让 Lua
    // 局部覆盖的边界可被逐项断言。
    const bool deepDarkRegistered = registry.registerBuiltInTheme(
        std::make_unique<MMM::Graphic::ImGuiTheme>(
            "DeepDark",
            "DeepDark",
            MMM::Graphic::ImGuiThemeOrigin::BuiltIn,
            std::string(),
            std::filesystem::path(),
            [](ImGuiStyle& style) {
                style.Alpha                 = 0.9f;
                style.WindowPadding         = { 1.0f, 2.0f };
                style.Colors[ImGuiCol_Text] = ImVec4(0.1f, 0.2f, 0.3f, 1.0f);
                style.Colors[ImGuiCol_WindowBg] =
                    ImVec4(0.4f, 0.5f, 0.6f, 1.0f);
            }));
    // Light 使用独立 ID 与样式，验证内置注册不会因第二项覆盖首项。
    const bool lightRegistered = registry.registerBuiltInTheme(
        std::make_unique<MMM::Graphic::ImGuiTheme>(
            "Light",
            "Light",
            MMM::Graphic::ImGuiThemeOrigin::BuiltIn,
            std::string(),
            std::filesystem::path(),
            [](ImGuiStyle& style) {
                style.Alpha                 = 1.0f;
                style.Colors[ImGuiCol_Text] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
                style.Colors[ImGuiCol_WindowBg] =
                    ImVec4(0.94f, 0.94f, 0.94f, 1.0f);
            }));
    // 两项都成功才允许继续，避免后续插件失败被误判为 Lua 解析问题。
    return deepDarkRegistered && lightRegistered;
}

/// @brief 验证 Lua 主题字段解析与内置基底继承。
///
/// 场景同时写入一个合法插件和一个引用缺失基底的非法插件，验证扫描计数、主题
/// 注册、插件清单诊断以及标量/向量/枚举/布尔/颜色字段的转换。最后确认合法插件
/// 只覆盖声明字段，其他颜色继续继承 DeepDark。
///
/// @param registry 主题注册表。
/// @param pluginDirectory 测试插件目录。
/// @return 首轮加载和样式断言全部成功时返回 true。
bool testPluginLoad(MMM::Graphic::ImGuiThemeRegistry& registry,
                    const std::filesystem::path&      pluginDirectory)
{
    // 文件名前缀固定扫描顺序，使插件清单和失败定位在不同文件系统上保持稳定。
    const auto validPlugin   = pluginDirectory / "01-valid.lua";
    const auto invalidPlugin = pluginDirectory / "02-invalid.lua";
    // 合法脚本覆盖各类代表字段，并故意只覆盖 Text 颜色以保留继承断言空间。
    bool ok = check(writePlugin(validPlugin,
                                R"lua(return {
    type = "theme",
    id = "test.ocean",
    name = "Test Ocean",
    base = "DeepDark",
    style = {
        Alpha = 0.75,
        WindowPadding = { 12.0, 13.0 },
        WindowMenuButtonPosition = "Right",
        AntiAliasedFill = false,
        Colors = {
            Text = { 0.9, 0.8, 0.7, 1.0 },
        },
    },
})lua"),
                    "有效插件写入失败");
    // 非法脚本本身语法正确，唯一错误是基底不存在，确保失败归因于语义校验。
    ok &= check(writePlugin(invalidPlugin,
                            R"lua(return {
    type = "theme",
    id = "test.invalid",
    name = "Invalid",
    base = "MissingBuiltIn",
    style = {},
})lua"),
                "无效插件写入失败");
    // 夹具不完整时停止加载，避免把写入故障放大为无关扫描和解析断言。
    if ( !ok ) return false;

    // 一次重载必须发现两个文件，仅发布合法主题，同时保留非法插件诊断条目。
    const auto result = registry.reloadThemePlugins(pluginDirectory);
    ok &= check(result.discoveredPluginFiles == 2, "插件扫描数量不匹配");
    ok &= check(result.loadedThemeCount == 1, "有效主题载入数量不匹配");
    ok &= check(result.failedThemeCount == 1, "无效主题数量不匹配");
    ok &= check(registry.findTheme("test.ocean") != nullptr,
                "有效主题实例未进入注册表");
    ok &= check(registry.findTheme("test.invalid") == nullptr,
                "无效主题实例不应进入注册表");
    ok &= check(registry.plugins().size() == 2, "插件清单数量不匹配");
    // 插件稳定 ID 以配置根下 themes/ 相对路径表示，不暴露临时绝对目录。
    const auto* validPluginInfo = registry.findPlugin("themes/01-valid.lua");
    const auto* invalidPluginInfo =
        registry.findPlugin("themes/02-invalid.lua");
    // 合法文件应处于启用状态、贡献一个主题且没有错误。
    ok &= check(validPluginInfo && validPluginInfo->enabled &&
                    validPluginInfo->loadedThemeCount == 1 &&
                    validPluginInfo->errorCount == 0,
                "有效插件清单状态不匹配");
    // 加载失败不等于被用户禁用；清单仍应保留首个错误供设置界面展示。
    ok &= check(invalidPluginInfo && invalidPluginInfo->enabled &&
                    invalidPluginInfo->loadedThemeCount == 0 &&
                    invalidPluginInfo->errorCount == 1 &&
                    !invalidPluginInfo->firstError.empty(),
                "无效插件清单状态不匹配");

    // 使用新样式对象应用主题，随后分别检查显式覆盖与基底继承结果。
    ImGuiStyle style;
    ok &= check(registry.applyTheme("test.ocean", style), "自定义主题应用失败");
    ok &= check(near(style.Alpha, 0.75f), "Alpha 覆盖失败");
    ok &= check(near(style.WindowPadding.x, 12.0f) &&
                    near(style.WindowPadding.y, 13.0f),
                "WindowPadding 覆盖失败");
    ok &= check(style.WindowMenuButtonPosition == ImGuiDir_Right,
                "方向字段覆盖失败");
    ok &= check(!style.AntiAliasedFill, "布尔字段覆盖失败");
    ok &= check(near(style.Colors[ImGuiCol_Text].x, 0.9f), "颜色字段覆盖失败");
    // WindowBg 未在 Lua 中声明，其 DeepDark 红色分量应保持 0.4。
    ok &= check(near(style.Colors[ImGuiCol_WindowBg].x, 0.4f),
                "未覆盖颜色应继承内置基底");
    return ok;
}

/// @brief 验证重载会删除旧插件主题实例。
///
/// 场景删除非法文件并用同一路径覆写合法插件，但更换主题 ID。第二次扫描必须先
/// 清除所有旧插件主题，再发布新实例；否则设置列表会积累已经不存在的主题。
///
/// @param registry 主题注册表。
/// @param pluginDirectory 测试插件目录。
/// @return 旧实例删除且新实例载入时返回 true。
bool testReloadClearsOldThemes(MMM::Graphic::ImGuiThemeRegistry& registry,
                               const std::filesystem::path& pluginDirectory)
{
    // 使用 error_code 接口保持测试符合项目禁用异常约束。
    std::error_code filesystemError;
    std::filesystem::remove(pluginDirectory / "02-invalid.lua",
                            filesystemError);
    if ( filesystemError ) {
        return check(false, "无法删除无效测试插件");
    }

    // 保持文件路径不变但改变 ID，专门覆盖“按文件重载却残留旧实例”的风险。
    const bool written = writePlugin(pluginDirectory / "01-valid.lua",
                                     R"lua(return {
    type = "theme",
    id = "test.reloaded",
    name = "Reloaded Theme",
    base = "DeepDark",
    style = {
        FrameRounding = 8.0,
    },
})lua");
    if ( !check(written, "重载插件写入失败") ) return false;

    // 新扫描现在只含一个文件且不应携带上一轮非法插件的失败计数。
    const auto result = registry.reloadThemePlugins(pluginDirectory);
    bool       ok     = check(result.success(), "第二次重载不应失败");
    ok &= check(result.loadedThemeCount == 1, "第二次重载数量不匹配");
    ok &= check(registry.findTheme("test.ocean") == nullptr,
                "重载后旧主题实例仍然存在");
    ok &= check(registry.findTheme("test.reloaded") != nullptr,
                "重载后新主题实例不存在");
    return ok;
}

/// @brief 验证插件文件禁用后不执行，重新启用后立即恢复实例。
///
/// 禁用集合使用插件稳定 ID，而不是临时目录绝对路径。禁用扫描仍保留插件清单项，
/// 但不能执行 Lua 或注册主题；下一次不传禁用集合等价于重新启用并恢复实例。
///
/// @param registry 主题注册表。
/// @param pluginDirectory 测试插件目录。
/// @return 禁用与重新启用状态均正确时返回 true。
bool testPluginHotToggle(MMM::Graphic::ImGuiThemeRegistry& registry,
                         const std::filesystem::path&      pluginDirectory)
{
    // 该 ID 与 loader 对 themes 目录的规范化规则一致。
    const std::vector<std::string> disabledPluginIds{ "themes/01-valid.lua" };
    const auto                     disabledResult =
        registry.reloadThemePlugins(pluginDirectory, disabledPluginIds);
    // 被禁用文件不是失败，success 应保持 true，另由 disabled 计数表达状态。
    bool ok = check(disabledResult.success(), "禁用插件不应产生加载错误");
    ok &= check(disabledResult.disabledPluginFiles == 1,
                "禁用插件文件计数不匹配");
    ok &=
        check(disabledResult.loadedThemeCount == 0, "禁用插件不应创建主题实例");
    ok &= check(registry.findTheme("test.reloaded") == nullptr,
                "禁用插件后主题实例仍然存在");
    // 设置界面仍需显示禁用项，因此清单必须存在并明确 enabled=false。
    const auto* disabledPlugin = registry.findPlugin("themes/01-valid.lua");
    ok &= check(disabledPlugin && !disabledPlugin->enabled &&
                    disabledPlugin->loadedThemeCount == 0,
                "插件清单未保留禁用状态");

    // 空禁用集合重新执行同一文件，旧的禁用清单状态和主题集合都应被替换。
    const auto enabledResult = registry.reloadThemePlugins(pluginDirectory);
    ok &= check(enabledResult.success(), "重新启用插件应成功加载");
    ok &= check(enabledResult.disabledPluginFiles == 0,
                "重新启用后不应保留禁用计数");
    ok &= check(registry.findTheme("test.reloaded") != nullptr,
                "重新启用后主题实例未恢复");
    const auto* enabledPlugin = registry.findPlugin("themes/01-valid.lua");
    ok &= check(enabledPlugin && enabledPlugin->enabled &&
                    enabledPlugin->loadedThemeCount == 1,
                "重新启用后的插件清单状态不匹配");
    return ok;
}

/// @brief 验证文档附带的完整示例始终符合实际加载接口。
///
/// 每个示例复制到独立临时目录并使用全新注册表加载，避免前序测试插件、禁用状态
/// 或主题 ID 影响结果。该检查把文档示例当作可执行契约，防止字段格式随实现演进
/// 后示例静默过时。
///
/// @param examplePath 仓库内示例插件路径。
/// @param expectedThemeId 示例应注册的主题稳定 ID。
/// @param pluginDirectory 独立测试插件目录。
/// @return 示例被复制、解析并注册时返回 true。
bool testDocumentedExample(const std::filesystem::path& examplePath,
                           std::string_view             expectedThemeId,
                           const std::filesystem::path& pluginDirectory)
{
    // 重建专用目录保证只扫描当前一个示例，并清除旧运行可能留下的文件。
    std::error_code filesystemError;
    std::filesystem::remove_all(pluginDirectory, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directories(pluginDirectory, filesystemError);
    if ( filesystemError ) {
        return check(false, "无法创建示例插件测试目录");
    }
    // 复制而非直接加载源码目录，避免 loader 的 themes 相对 ID 受仓库层级影响。
    std::filesystem::copy_file(
        examplePath,
        pluginDirectory / examplePath.filename(),
        std::filesystem::copy_options::overwrite_existing,
        filesystemError);
    if ( filesystemError ) {
        return check(false, "无法复制文档示例插件");
    }

    // 独立注册表只包含测试基底和当前示例，成功计数应精确为一。
    MMM::Graphic::ImGuiThemeRegistry registry;
    bool ok = check(registerBaseTheme(registry), "示例测试基底主题注册失败");
    const auto result = registry.reloadThemePlugins(pluginDirectory);
    ok &= check(result.success(), "文档示例插件应成功载入");
    ok &= check(result.loadedThemeCount == 1, "文档示例应创建一个主题实例");
    ok &= check(registry.findTheme(expectedThemeId) != nullptr,
                "文档示例主题实例未进入注册表");
    return ok;
}

}  // namespace

/// @brief 主题 Lua 插件加载与重载回归测试入口。
///
/// 测试输出根由 CMake/CTest 指向构建目录，两个示例路径指向只读源码文件。入口先
/// 重建主夹具目录，再在同一注册表上串行覆盖加载、重载和热切换状态机，最后用
/// 两个独立注册表验证文档示例。
///
/// @param argc 命令行参数数量。
/// @param argv 首个附加参数为测试输出目录，后两个为文档示例插件路径。
/// @return 全部断言通过时返回 0。
int main(int argc, char* argv[])
{
    // 三个路径缺一不可；在构造 filesystem::path 前拒绝空指针。
    if ( argc < 4 || !argv[1] || !argv[2] || !argv[3] ) {
        XERROR("ThemePluginLoaderTest requires output and two example paths");
        return 1;
    }

    // 主输出目录可清理，示例源路径只读取；所有生成文件都留在
    // build/test_output。
    const std::filesystem::path pluginDirectory = argv[1];
    const std::filesystem::path examplePath     = argv[2];
    const std::filesystem::path ivmExamplePath  = argv[3];
    std::error_code             filesystemError;
    // 清理失败可能由目录不存在产生可忽略状态，随后清空 error_code 并以创建结果
    // 作为真正的前置条件。
    std::filesystem::remove_all(pluginDirectory, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directories(pluginDirectory, filesystemError);
    if ( filesystemError ) {
        XERROR("Failed to prepare theme plugin test directory: {}",
               filesystemError.message());
        return 1;
    }

    // 相关状态机场景共享注册表，才能验证每轮 reload 确实替换上一轮实例。
    MMM::Graphic::ImGuiThemeRegistry registry;
    bool ok = check(registerBaseTheme(registry), "内置基底主题注册失败");
    ok &= testPluginLoad(registry, pluginDirectory);
    ok &= testReloadClearsOldThemes(registry, pluginDirectory);
    ok &= testPluginHotToggle(registry, pluginDirectory);
    // 文档示例使用相邻但独立的输出目录，不会被主插件场景的 remove_all 删除。
    ok &= testDocumentedExample(
        examplePath,
        "example.twilight",
        pluginDirectory.parent_path() / "theme_plugin_example");
    ok &= testDocumentedExample(
        ivmExamplePath,
        "example.ivm",
        pluginDirectory.parent_path() / "ivm_theme_plugin_example");
    return ok ? 0 : 1;
}
