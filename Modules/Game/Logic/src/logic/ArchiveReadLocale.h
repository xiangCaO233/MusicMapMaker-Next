#pragma once

#include <clocale>
#include <string>
#ifndef _WIN32
#    include <locale.h>
#endif

namespace MMM::Logic
{
/// @brief 为 libarchive 的文件名转换提供线程局部 UTF-8 字符环境。
/// @note 不修改进程全局 locale；只在同步解压作用域内存在。
/// @warning 仅用于低频归档读取，创建 locale 可能分配内部转换表。
class ArchiveReadLocale
{
public:
    /// @brief 保存当前线程环境并安装 UTF-8 的 LC_CTYPE。
    ArchiveReadLocale()
    {
#ifdef _WIN32
        // Windows CRT 先启用线程局部状态，再调用 setlocale 才不影响其他线程。
        m_previousMode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
        if ( m_previousMode == -1 ) return;
        // setlocale 返回内部缓冲区，切换前必须复制原始名称。
        if ( const char* name = std::setlocale(LC_CTYPE, nullptr) )
            m_previousName = name;
        // UCRT 的 UTF-8 locale 与系统显示语言无关，避免 ACP 不能表示资源名。
        m_ready = std::setlocale(LC_CTYPE, ".UTF8") != nullptr;
#else
        // 复制线程当前 locale，以保留数字格式等非字符分类设置。
        // LC_GLOBAL_LOCALE 只能作为复制来源，不能作为释放对象。
        const auto previous = uselocale(nullptr);
        m_locale            = duplocale(previous);
        if ( !m_locale ) return;
        // Linux 通常提供 C.UTF-8，macOS 使用 en_US.UTF-8。
        // newlocale 失败不会消费传入句柄，可以安全尝试第二个名称。
        auto utf8 = newlocale(LC_CTYPE_MASK, "C.UTF-8", m_locale);
        if ( !utf8 ) utf8 = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", m_locale);
        if ( !utf8 ) return;
        m_locale   = utf8;
        m_previous = uselocale(m_locale);
        // 切换失败时原线程环境不变，但仍需释放新建的私有句柄。
        m_ready = m_previous != nullptr;
#endif
    }

    /// @brief 先恢复线程环境，再释放本作用域持有的转换表。
    ~ArchiveReadLocale()
    {
#ifdef _WIN32
        // 原模式可能是全局跟随，但恢复名称仍要在线程隔离期间完成。
        if ( m_previousMode != -1 ) {
            if ( !m_previousName.empty() )
                std::setlocale(LC_CTYPE, m_previousName.c_str());
            _configthreadlocale(m_previousMode);
        }
#else
        // 尚未成功切换时不恢复空句柄，避免把失败误当成查询调用。
        if ( m_previous ) uselocale(m_previous);
        if ( m_locale ) freelocale(m_locale);
#endif
    }
    /// @brief 禁止复制，避免两份对象重复恢复同一线程状态。
    ArchiveReadLocale(const ArchiveReadLocale&) = delete;
    /// @brief 禁止赋值，线程环境必须按栈顺序恢复。
    ArchiveReadLocale& operator=(const ArchiveReadLocale&) = delete;
    /// @brief 返回字符环境是否已成功安装。
    [[nodiscard]] bool ready() const { return m_ready; }

private:
    /// @brief UTF-8 字符环境已启用，失败时由调用方报告。
    // 析构根据各平台句柄状态处理部分初始化，不能只依赖此成功标志。
    bool m_ready{ false };
#ifdef _WIN32
    /// @brief CRT 原线程 locale 模式，负值表示切换失败。
    int m_previousMode{ -1 };
    /// @brief 原 LC_CTYPE 名称的独立副本。
    std::string m_previousName;
#else
    /// @brief 此作用域拥有的 locale 句柄，不与其他线程共享。
    locale_t m_locale{};
    /// @brief 原线程 locale，可为 LC_GLOBAL_LOCALE，仅观察不释放。
    // 成员顺序不决定恢复顺序，析构函数始终先恢复再释放。
    locale_t m_previous{};
#endif
};
}  // namespace MMM::Logic
