#include "common/platform/DocumentStorage.h"
#include "config/AppConfig.h"
#include "ui/utils/NativeFileDialog.h"

/// @brief Android 异步 SAF 无需初始化桌面库，允许业务初始化继续。
nfdresult_t NFD_Init()
{
    return NFD_OKAY;
}
/// @brief 不存在桌面对话框实例，无需关闭原生后端。
void NFD_Quit() {}
/// @brief 返回固定的能力错误，供现有调用者显示失败原因。
const char* NFD_GetError()
{
    return "Use the asynchronous Android document picker.";
}
/// @brief 本后端不产生路径；空值释放仍遵循 NFD 接口契约。
void NFD_FreePathU8(nfdu8char_t*) {}

namespace MMM::UI::NativeFileDialog
{
/// @brief Android 文件选择器不依赖桌面窗口父子关系。
void bindMainWindow(Graphic::PlatformWindowHandle*)
{
    // 初始化时准备持久化选择器偏好，避免第一次统一浏览从私有工作目录开始。
    (void)requiresSystemPicker();
}

/// @brief 仅用户选中统一样式且系统已授权时，启用普通文件路径的兼容访问。
/// @details 设置引用和最近目录都属于 UI 线程，查询不执行共享目录枚举。
/// @warning 用户触发低频路径：不把权限探测带入逐帧绘制或逻辑更新。
bool requiresSystemPicker()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    if ( settings.filePickerStyle != Config::FilePickerStyle::Unified )
        return true;
    // 只查询用户已经授予的权限，选择目录不会隐式弹出授权页。
    const auto root = Common::localFileRoot();
    // 授权撤回后回到 SAF，不让内置浏览器尝试一个实际不可读的共享根。
    if ( !root ) return true;
    // 旧 SAF 副本和初始点目录不适合作为兼容选择器的共享存储入口。
    if ( settings.lastFilePickerPath.empty() ||
         settings.lastFilePickerPath == "." ||
         settings.lastFilePickerPath.starts_with("/data/") )
        // 普通共享目录由内置选择器继续浏览，后续保存直接沿用原生文件系统。
        settings.lastFilePickerPath = *root;
    return false;
}
/// @brief 旧同步入口不能阻塞 SDL 生命周期，未迁移调用明确返回能力错误。
/// @details 输出路径不会分配内存，调用方不得把能力错误当作用户选中空路径。
/// 调用方需迁移到异步接口，以等待系统授权及工作副本复制。
nfdresult_t openFile(nfdu8char_t** outPath, const nfdu8filteritem_t*,
                     nfdfiltersize_t, const nfdu8char_t*)
{
    if ( outPath ) *outPath = nullptr;
    return NFD_ERROR;
}
/// @brief 同步保存入口不能把 content URI 冒充路径，需使用 saveFileAsync。
/// @details 现阶段不会创建目标文件，也不会请求共享存储权限。
/// 异步入口独立管理系统创建目标和原生 writer 完成后的发布。
nfdresult_t saveFile(nfdu8char_t** outPath, const nfdu8filteritem_t*,
                     nfdfiltersize_t, const nfdu8char_t*, const nfdu8char_t*)
{
    if ( outPath ) *outPath = nullptr;
    return NFD_ERROR;
}
/// @brief 文件夹选择与普通文件选择保持同一错误约定。
/// @details 文件夹授权和文件打开授权不同，不能通过伪造目录字符串绕过平台授权。
/// 保持失败输出为空，使调用者已有清理逻辑安全可重复执行。
nfdresult_t pickFolder(nfdu8char_t** outPath, const nfdu8char_t*)
{
    if ( outPath ) *outPath = nullptr;
    return NFD_ERROR;
}
}  // namespace MMM::UI::NativeFileDialog
