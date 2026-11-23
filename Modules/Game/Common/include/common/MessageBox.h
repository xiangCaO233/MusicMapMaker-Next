#pragma once

#include <string>

namespace MMM::UI
{

/// @brief 记录图形界面不可用时的致命错误，并在支持的桌面平台尝试提示用户。
/// @param title UTF-8 标题，直接写入日志并作为桌面提示的标题。
/// @param message UTF-8 诊断正文，允许换行。调用方负责随后的失败退出流程。
/// @warning 只用于低频初始化失败。桌面消息框可能等待用户确认。
void showFatalError(const std::string& title, const std::string& message);

}  // namespace MMM::UI
