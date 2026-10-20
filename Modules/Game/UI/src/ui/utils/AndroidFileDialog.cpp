#include "ui/utils/NativeFileDialog.h"

/// @brief 桌面文件选择器尚未接 Android SAF，初始化明确报告不可用。
nfdresult_t NFD_Init()
{
    return NFD_ERROR;
}
/// @brief 不存在桌面对话框实例，无需关闭原生后端。
void NFD_Quit() {}
/// @brief 返回固定的能力错误，供现有调用者显示失败原因。
const char* NFD_GetError()
{
    return "Android document picker is not available yet.";
}
/// @brief 本后端不产生路径；空值释放仍遵循 NFD 接口契约。
void NFD_FreePathU8(nfdu8char_t*) {}

namespace MMM::UI::NativeFileDialog
{
/// @brief Android 文件选择器不依赖桌面窗口父子关系。
void bindMainWindow(Graphic::PlatformWindowHandle*) {}
/// @brief 尚未接入 SAF 时明确返回能力错误，输出槽保持为空。
/// @details 输出路径不会分配内存，调用方不得把能力错误当作用户选中空路径。
/// 后续接 SAF 时需要处理 URI 访问权限，不能直接复用桌面绝对路径假设。
nfdresult_t openFile(nfdu8char_t** outPath, const nfdu8filteritem_t*,
                     nfdfiltersize_t, const nfdu8char_t*)
{
    if ( outPath ) *outPath = nullptr;
    return NFD_ERROR;
}
/// @brief 保存操作不能把 content URI 冒充路径，未适配前明确返回失败。
/// @details 现阶段不会创建目标文件，也不会请求共享存储权限。
/// 现有错误提示继续负责告诉用户该入口尚不可用。
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
