#include "ui/plugin/ToolPluginView.h"
#include "config/AppPaths.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
// 这个测试不创建图形设备：加载器应先构造声明式缓存，窗口只有在 UI
// update 中才提交 ImGui 绘制。目录由测试注入的 MMM_CONFIG_ROOT 隔离。
// 内置脚本与外置脚本经过同一份清单校验，不依赖应用实际用户配置。
/// @brief 在内存清单中查找稳定插件 ID。
/// @param plugins 最近一次加载的工具插件快照。
/// @param id 待查找插件标识。
/// @return 清单包含该插件时为 true。
bool contains(const std::vector<MMM::UI::ToolPluginInfo>& plugins,
              const std::string&                          id)
{
    // 只比较稳定 ID，不根据可见名称判断；名称可能本地化或修改。
    return std::any_of(plugins.begin(),
                       plugins.end(),
                       [&id](const auto& plugin) { return plugin.id == id; });
}
}  // namespace

/// @brief 验证两个内置工具和用户脚本使用同一声明式加载流程。
/// @return 失败时返回非零，交给 CTest 报告。
int main()
{
    // CTest 为当前套件注入独立 MMM_CONFIG_ROOT，测试只写该目录。
    // 不使用用户本机插件目录，避免无关脚本或最近目录影响结果。
    const auto      root = MMM::Config::AppPaths::pluginsRootPath() / "tools";
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if ( error ) return 1;

    MMM::UI::ToolPluginView view;
    // 内置脚本经过 sol::safe_script、类型检查和 build 回调构造。
    // 两个工具应在没有 assets 文件复制到配置目录时仍能加载。
    if ( !contains(view.plugins(), "mmm.audio_transcoder") ||
         !contains(view.plugins(), "mmm.beatmap_converter") ||
         !view.openPlugin("mmm.audio_transcoder") ||
         view.openPlugin("missing.plugin") ) {
        // 不存在的 ID 不能意外打开上一次的窗口。
        return 2;
    }

    // 用户脚本从隔离目录读取；重复 ID 不得替换内置工具实例。
    // 此处还检查 Lua base 库里的文件读取函数确实被移除。
    // 若沙箱意外暴露 dofile/loadfile，脚本会在入口阶段主动失败。
    const auto    path = root / "test-tool.lua";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "assert(io == nil and os == nil and package == nil and "
              "dofile == nil and loadfile == nil); "
              "return { type='tool', id='test.tool', name='测试工具', "
              "build=function(api) return {{type='text', id='hint', "
              "label='hello'}} end, "
              "on_action=function(id, value, api) end }";
    output.close();
    if ( !output ) return 3;
    view.reload();
    // 显式重载后重新扫描测试目录；没有轮询或逐帧文件检测。
    const bool loaded = contains(view.plugins(), "test.tool") &&
                        view.openPlugin("test.tool") &&
                        contains(view.plugins(), "mmm.audio_transcoder");
    // 外置脚本成功不应移除内置脚本；两种来源必须并存于同一清单。
    // 语法错误必须留在列表中供开发者定位，且不能生成空操作窗口。
    // 这项诊断避免用户必须查看终端日志才能发现插件没有出现的原因。
    const auto    invalidPath = root / "invalid-tool.lua";
    std::ofstream invalid(invalidPath, std::ios::binary | std::ios::trunc);
    invalid << "return {";
    invalid.close();
    if ( !invalid ) return 4;
    view.reload();
    // 失败项只存在于列表快照，available=false 表示不能打开窗口。
    const bool diagnosed = std::any_of(
        view.plugins().begin(), view.plugins().end(), [](const auto& info) {
            return !info.available && !info.error.empty() &&
                   info.name.find("invalid-tool.lua") != std::string::npos;
        });
    // 错误项名称含文件路径，便于定位并区分多个同时失败的脚本。
    const bool removedValid = std::filesystem::remove(path, error) && !error;
    // 两个清理结果分别判断，不让第二次成功掩盖第一次的残留文件。
    error.clear();
    const bool removedInvalid =
        std::filesystem::remove(invalidPath, error) && !error;
    // 清理失败与脚本未加载都作为失败，避免后续测试看到残留插件。
    // CTest 隔离目录会在套件结束后删除，但本测试仍负责自身临时脚本。
    return loaded && diagnosed && removedValid && removedInvalid ? 0 : 5;
}
