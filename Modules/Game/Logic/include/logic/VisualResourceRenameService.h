#pragma once

#include <filesystem>
#include <string>

namespace MMM
{
class Project;
struct BaseMapMeta;
}  // namespace MMM

namespace MMM::Logic
{

/// @brief 一次视觉资源改名的结果及旧、新路径身份。
struct VisualResourceRenameResult {
    /// @brief 失败原因；为空表示磁盘事务已完成。
    std::string m_errorMessage;

    /// @brief 改名前的项目内绝对路径。
    std::filesystem::path m_oldPath;

    /// @brief 改名后的项目内绝对路径。
    std::filesystem::path m_newPath;
};

/// @brief 项目谱面视觉资源的低频磁盘事务和元数据路径重映射。
class VisualResourceRenameService
{
public:
    /// @brief 改名文件并事务更新受管谱面的封面和背景引用。
    /// @param project 当前正式项目，不修改其资源列表。
    /// @param oldPath 项目相对路径或绝对路径。
    /// @param newFileName 单个新文件名，可省略旧扩展名。
    /// @return 磁盘提交状态及用于会话同步的绝对路径。
    /// @warning 低频文件操作；调用方需串行化同项目保存与资源变更。
    static VisualResourceRenameResult rename(
        const Project& project, const std::filesystem::path& oldPath,
        const std::string& newFileName);

    /// @brief 将指向指定资源的封面与背景路径投影到改名后的位置。
    /// @param metadata 待更新的谱面元数据，其他字段保持原值。
    /// @param projectRoot 当前项目根路径。
    /// @param oldPath 改名前的绝对路径。
    /// @param newPath 改名后的绝对路径。
    /// @return 至少一个引用发生变化时返回 true。
    static bool remapMetadata(BaseMapMeta&                 metadata,
                              const std::filesystem::path& projectRoot,
                              const std::filesystem::path& oldPath,
                              const std::filesystem::path& newPath);
};

}  // namespace MMM::Logic
