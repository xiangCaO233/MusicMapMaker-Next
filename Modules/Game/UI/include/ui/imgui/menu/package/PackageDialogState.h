#pragma once

#include "common/LogicCommands.h"
#include "mmm/Metadata.h"
#include "mmm/beatmap/MalodyMode.h"
#include "mmm/project/PackageFileTypes.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MMM::UI
{

/// @brief 打包候选文件条目。
struct PackageCandidateFile {
    /// @brief 项目相对路径，使用 UTF-8 编码和通用分隔符。
    std::string relativePath;

    /// @brief 用户界面显示的资源分类名称。
    std::string typeLabel;

    /// @brief 打包资源分类，用于区分谱面与可被谱面绑定的资源。
    PackageResourceType resourceType{ PackageResourceType::Beatmap };

    /// @brief 是否将该文件写入最终包；候选构建后按前台画布策略初始化。
    bool selected{ false };

    /// @brief 选中该谱面时必须一起打包的资源相对路径列表。
    std::vector<std::string> dependencyRelativePaths;

    /// @brief 当前格式候选列表中找不到的谱面依赖资源路径。
    std::vector<std::string> missingDependencyRelativePaths;

    /// @brief 当前文件被多少个已选中谱面依赖；大于 0 时不可取消。
    uint32_t requiredBySelectedBeatmaps{ 0 };

    /// @brief 谱面是否含 Flick/折线，决定是否显示 MCZ 上架 mode_ext 选项。
    bool hasStoreModeExtEligibleElements{ false };
};

/// @brief 打包转换前临时编辑的目标谱面元数据。
struct PackageBeatmapMetadataEdit {
    /// @brief 项目相对谱面路径，使用 UTF-8 编码和通用分隔符。
    std::string relativePath;

    /// @brief 当前编辑中的基础谱面元数据。
    BaseMapMeta baseMeta;

    /// @brief 目标谱面标题输入缓存。
    std::array<char, 192> titleBuffer{};

    /// @brief 目标谱面原标题输入缓存。
    std::array<char, 192> titleUnicodeBuffer{};

    /// @brief 目标谱面艺术家输入缓存。
    std::array<char, 192> artistBuffer{};

    /// @brief 目标谱面原艺术家输入缓存。
    std::array<char, 192> artistUnicodeBuffer{};

    /// @brief 目标谱面谱师输入缓存。
    std::array<char, 192> creatorBuffer{};

    /// @brief 目标谱面难度名输入缓存。
    std::array<char, 192> versionBuffer{};
};

/// @brief 主菜单打包流程的全部 UI 状态。
struct PackageDialogState {
    /// @brief 是否在下一帧打开打包格式选择弹窗。
    bool showFormatPicker{ false };

    /// @brief 打包格式选择弹窗当前是否保持打开。
    bool formatPickerOpen{ false };

    /// @brief 是否显示打包文件复选列表窗口。
    bool showFileSelectionWindow{ false };

    /// @brief 是否在下一帧打开打包文件复选列表弹窗。
    bool openFileSelectionWindow{ false };

    /// @brief 是否显示打包前目标谱面元数据补充窗口。
    bool showBeatmapMetadataWindow{ false };

    /// @brief 是否等待显示 Key 模式自动转换兼容性警告。
    bool showMalodyCompatibilityWarning{ false };

    /// @brief 当前打包目标格式。
    PackageFileType selectedFileType{ PackageFileType::Osz };

    /// @brief MCZ 包内谱面统一使用的 Malody 模式。
    MalodyMode selectedMalodyMode{ MalodyMode::Slide };

    /// @brief 当前打包格式下可选择的候选文件。
    std::vector<PackageCandidateFile> candidateFiles;

    /// @brief 等待输出路径确认的已选项目相对文件路径。
    std::vector<std::string> pendingRelativePaths;

    /// @brief 等待打包命令使用的元数据覆盖项。
    std::vector<Logic::PackageBeatmapMetadataOverride> pendingMetadataOverrides;

    /// @brief 打包前正在编辑的目标谱面元数据项。
    std::vector<PackageBeatmapMetadataEdit> beatmapMetadataEdits;

    /// @brief 是否将打包转换产物保存回项目目录。
    bool saveConvertedBeatmapsToProject{ false };

    /// @brief MCZ 打包时是否额外在包内写入旧皮肤兼容的 IMD 谱面。
    bool includeLegacyImdBeatmaps{ false };
};

}  // namespace MMM::UI
