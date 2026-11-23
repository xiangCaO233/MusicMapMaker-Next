#include "common/MessageBox.h"

#include "log/colorful-log.h"

namespace MMM::UI
{

/// @brief 在 macOS 和其余后备平台写入已初始化的日志。
/// @details 图形启动可能尚不完整，诊断不能依赖应用窗口、Vulkan 或桌面工具。
/// @param title 保留在致命日志中的 UTF-8 标题。
/// @param message 记录在对应标题下的完整正文。
void showFatalError(const std::string& title, const std::string& message)
{
    // 调用方继续既有退出流程，日志保存早期初始化失败的具体原因。
    XERROR("{}: {}", title, message);
}

}  // namespace MMM::UI
