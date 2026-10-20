#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace MMM::Common
{
/// @brief 将已写好的工作副本发布到其授权文档；普通磁盘路径无额外操作。
/// @param path 已由对应格式 writer 关闭的本地文件。
/// @param error 失败时返回平台诊断，调用者必须保留编辑脏状态。
/// @warning 保存低频路径：Android 包含 provider I/O，禁止在逐帧展示路径调用。
#ifdef __ANDROID__
bool publishDocument(const std::filesystem::path& path, std::string& error);
/// @brief 启动平台选择器；0 打开文件、1 创建目标、2 授权目录。
/// @details 请求只调度 Java UI 操作，不阻塞 SDL 事件分发。
bool beginDocumentSelection(int kind, std::string_view defaultName);
/// @brief 查询已由用户授权的本机共享目录根，未授权时返回空值。
/// @warning 仅在显式选择器操作或初始化调用，不用于每帧权限探测。
std::optional<std::string> localFileRoot();
/// @brief 从设置按钮进入本包的所有文件访问授权页，不自动授予权限。
/// @return 成功调度系统设置时为 true，用户实际选择须另行查询。
bool requestLocalFileAccess();
/// @brief 取走已经完成的 UTF-8 结果，未就绪时立即返回空值。
/// @warning 只在有在途选择时由 UI 帧调用，不访问文件或等待 provider。
std::optional<std::string> takeDocumentSelection();
#else
/// @brief 桌面 writer 直接使用文件系统，不引入 Android 文档发布步骤。
inline bool publishDocument(const std::filesystem::path&, std::string&)
{
    return true;
}
#endif
}  // namespace MMM::Common
