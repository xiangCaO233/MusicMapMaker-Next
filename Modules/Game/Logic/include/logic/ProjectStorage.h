#pragma once

#include "mmm/project/Project.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace MMM::Logic
{

/// @brief 负责项目配置的分片读写和旧单文件兼容。
class ProjectStorage
{
public:
    /// @brief 项目配置实际读取来源。
    enum class Source {
        None,
        Split,
        Legacy,
    };

    /// @brief 单次项目配置读取结果。
    struct LoadResult {
        /// @brief 是否成功读取出完整项目配置。
        bool m_success{ false };

        /// @brief 配置读取来源。
        Source m_source{ Source::None };

        /// @brief 反序列化后的项目配置。
        Project m_project;

        /// @brief 合并为旧顶层结构的原始 JSON，用于字段级迁移判断。
        nlohmann::json m_serializedProject;

        /// @brief 读取失败原因；没有任何配置时为空。
        std::string m_errorMessage;
    };

    /// @brief 获取项目内隐藏配置目录。
    [[nodiscard]] static std::filesystem::path storageDirectory(
        const std::filesystem::path& projectRoot);

    /// @brief 获取新分片格式入口文件。
    [[nodiscard]] static std::filesystem::path manifestPath(
        const std::filesystem::path& projectRoot);

    /// @brief 获取旧版根目录单文件配置路径。
    [[nodiscard]] static std::filesystem::path legacyProjectFilePath(
        const std::filesystem::path& projectRoot);

    /// @brief 判断目录中是否存在新分片或旧单文件项目配置。
    [[nodiscard]] static bool hasProjectConfiguration(
        const std::filesystem::path& projectRoot);

    /// @brief 优先读取新分片配置，失败时回退到旧单文件。
    [[nodiscard]] LoadResult load(
        const std::filesystem::path& projectRoot) const;

    /// @brief 将项目按职责写入隐藏目录中的多个 JSON 分片。
    /// @param project 待保存项目。
    /// @param projectRoot 项目根目录。
    /// @param errorMessage 失败时接收原因。
    /// @return 所有分片和入口文件均写入成功时返回 true。
    [[nodiscard]] bool save(const Project&               project,
                            const std::filesystem::path& projectRoot,
                            std::string&                 errorMessage) const;

    /// @brief 删除迁移完成后的旧版根目录单文件。
    /// @param projectRoot 项目根目录。
    /// @param errorMessage 删除失败时接收原因。
    /// @return 文件不存在或删除成功时返回 true。
    [[nodiscard]] bool removeLegacyProjectFile(
        const std::filesystem::path& projectRoot,
        std::string&                 errorMessage) const;

private:
    /// @brief 读取并组装新分片项目配置。
    [[nodiscard]] LoadResult loadSplit(
        const std::filesystem::path& projectRoot) const;

    /// @brief 读取旧版根目录单文件配置。
    [[nodiscard]] LoadResult loadLegacy(
        const std::filesystem::path& projectRoot) const;
};

}  // namespace MMM::Logic
