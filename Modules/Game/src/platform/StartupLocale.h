#pragma once

namespace MMM::Platform
{

/// @brief 为启动过程设置 C 运行库的 UTF-8 字符分类与多字节转换。
/// @return 成功时为 true；无可用 UTF-8 locale 时保留原字符设置并返回 false。
/// @note 只改变 LC_CTYPE，避免影响持久化数据的数值解析和其他区域类别。
/// @warning 在工作线程启动前调用，避免与运行库中的字符转换并发。
[[nodiscard]] bool initializeStartupCharacterLocale() noexcept;

}  // namespace MMM::Platform
