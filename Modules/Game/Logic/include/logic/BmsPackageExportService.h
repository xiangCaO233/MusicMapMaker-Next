#pragma once

#include "mmm/beatmap/BmsPackageAlignmentMode.h"

#include <filesystem>
#include <string>
#include <vector>

namespace MMM
{
class Project;
}

namespace MMM::Logic
{
struct PackageBeatmapMetadataOverride;

/// @brief BMS 资源包写出结果。
struct BmsPackageExportResult {
    /// @brief 所有谱面、引用资源和归档均成功写出时为真。
    bool success{ false };

    /// @brief 失败时可直接反馈给用户的原因。
    std::string errorMessage;
};

/// @brief 将选中的谱面与资源转换为首拍对齐的 BMS ZIP 包。
/// @note 只变换导出副本，保留源谱面、原音频以及非拍线物件的相对位置。
class BmsPackageExportService final
{
public:
    /// @brief 导出所有已选谱面，按互斥策略调整主音乐内容或触发位置。
    /// @param project 提供稳定音频资源 ID 与项目根目录。
    /// @param selectedPaths 已选项目相对路径，音频依赖也必须明确选择。
    /// @param outputPath 目标 ZIP 文件；全部准备成功前不打开目标。
    /// @param overrides 仅作用于本次转换的逐谱面基础元数据。
    /// @param alignmentMode 必须明确选择的首拍对齐策略。
    /// @return 完整归档是否写出及失败原因。
    /// @warning 用户触发的低频路径：完整加载谱面、离线编码及压缩文件，禁止
    /// 从播放回调或常态 UI 更新调用。项目资源表在调用期间必须保持稳定。
    [[nodiscard]] static BmsPackageExportResult exportPackage(
        const Project& project, const std::vector<std::string>& selectedPaths,
        const std::filesystem::path&                       outputPath,
        const std::vector<PackageBeatmapMetadataOverride>& overrides,
        BmsPackageAlignmentMode                            alignmentMode);
};
}  // namespace MMM::Logic
