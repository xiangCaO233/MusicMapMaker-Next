#pragma once

namespace MMM
{

/// @brief 正常完成主循环和资源收尾的进程退出码。
constexpr int EXIT_NORMAL = 0;

/// @brief 窗口或 Vulkan 上下文无法初始化时使用的进程退出码。
/// @note 保留既有拼写以维持调用点和外部脚本兼容。
constexpr int EXIT_WINDOW_EXEPTION = 1;

/**
 * @brief RAII 日志管理器
 *
 * 利用静态对象的生命周期自动初始化和关闭日志系统。
 * 构造时先完成平台控制台与崩溃处理配置，再解析当前可执行文件目录并切换
 * 工作目录，最后初始化日志。路径失败不会中止启动，而是在日志可用后告警。
 *
 * @warning 全局静态生命周期：构造发生在 main 之前，析构发生在其他业务静态
 * 对象收尾阶段；不得依赖尚未初始化或已经销毁的业务单例。
 */
struct RTTILogger {
    /// @brief 初始化平台控制台、崩溃处理、工作目录与日志系统。
    RTTILogger();

    /// @brief 在进程静态析构阶段关闭支持部分初始化状态的日志后端。
    ~RTTILogger();
};

/// @brief 程序启动时自动初始化、退出时自动关闭的全局日志管理器。
/// @warning 静态初始化顺序要求其他全局对象不要在本实例构造前写业务日志。
inline RTTILogger rttiLogger{};

}  // namespace MMM
