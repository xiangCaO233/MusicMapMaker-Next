#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

namespace MMM::Config
{

/// @brief 排他创建的工作目录，在作用域退出时回收本次工作内容。
/// @warning 低频文件系统操作。对象不转移所有权，也不接受外部目录供清理。
class ScopedTemporaryDirectory
{
public:
    /// @brief 在已存在的父目录内创建目录；只有实际创建成功才取得所有权。
    explicit ScopedTemporaryDirectory(const std::filesystem::path& parent,
                                      std::string_view             prefix);
    /// @brief 成功、失败和取消均执行清理。
    ~ScopedTemporaryDirectory();
    ScopedTemporaryDirectory(const ScopedTemporaryDirectory&) = delete;
    ScopedTemporaryDirectory& operator=(const ScopedTemporaryDirectory&) =
        delete;

    /// @brief 返回独占目录；创建失败时路径为空。
    const std::filesystem::path& path() const { return m_path; }
    /// @brief 返回不能取得目录所有权的原因；成功时为空。
    const std::error_code& error() const { return m_error; }

private:
    /// @brief 仅在成功创建目录后赋值，其他调用者的目录不得进入清理列表。
    std::filesystem::path m_path;
    /// @brief 记录最初的创建失败原因。
    std::error_code m_error;
};

/// @brief 同卷暂存输出，完整写入并关闭成功后用一次重命名替换目标。
/// @details 既有普通文件保留权限位，既有文件链接解析到真实目标。
/// 每次写入持有独占目录。同一目标并发保存时，最后成功重命名的完整版本生效。
/// @warning 低频文件系统操作，不保证断电持久性。
class StagedOutputFile
{
public:
    /// @brief 打开独占暂存文件，调用方负责提前创建父目录。
    explicit StagedOutputFile(const std::filesystem::path& target,
                              std::ios::openmode mode = std::ios::out);
    /// @brief 输出流须由调用方检查是否打开，写完后交给 commit 关闭。
    std::ofstream& stream() { return m_stream; }
    /// @brief 检查流状态及 close，随后原子发布；任一步失败均保留旧目标。
    bool commit();
    /// @brief 返回不能暂存、写出或发布的 I/O 错误。
    const std::error_code& error() const { return m_error; }

private:
    /// @brief 解析最终链接后的正式文件。
    std::filesystem::path m_target;
    /// @brief 打开或提交失败的错误码，首个失败决定事务结果。
    std::error_code m_error;
    /// @brief 原目标权限，缺失时使用文件系统默认权限。
    std::filesystem::perms m_permissions{ std::filesystem::perms::unknown };
    /// @brief 按声明顺序确保先关闭流，再回收独占目录。
    std::optional<ScopedTemporaryDirectory> m_directory;
    /// @brief 仅作为正式替换的源文件使用。
    std::filesystem::path m_staged;
    /// @brief 由序列化器借用的输出流。
    std::ofstream m_stream;
};

}  // namespace MMM::Config
