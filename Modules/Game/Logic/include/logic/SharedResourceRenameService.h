#pragma once

#include "logic/ProjectResourceTypes.h"
#include "mmm/metadata/BaseMapMeta.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace MMM
{
class BeatMap;
class Project;
struct AudioResource;
}  // namespace MMM

namespace MMM::Logic
{
/// @brief 谱面设置改名的只读预检结果，禁止据此跳过确认时的重新检查。
struct SharedResourceRenamePlan {
    /// @brief 预检失败原因；失败结果不能创建文件或更新绑定。
    std::string m_error;
    /// @brief 已解析的工程内源文件，原文件在复制模式中始终保留。
    std::filesystem::path m_source;
    /// @brief 保留目录与格式后缀后的目标路径，不允许覆盖已有文件。
    std::filesystem::path m_destination;
    /// @brief 依赖原文件的其他谱面路径，用于向用户解释复制的原因。
    std::vector<std::string> m_references;
    /// @brief 共享引用或当前 IMD 隐式绑定要求先复制，必须另行取得用户确认。
    bool m_requiresCopy{ false };
};

/// @brief 从打开谱面提取的可跨线程值证据，不持有可变谱面。
struct OpenResourceReferenceSnapshot {
    /// @brief 路径与视觉绑定；不复制时间线和实体注册表。
    BaseMapMeta m_metadata;
    /// @brief 显式资源引用；只在检查音频时收集。
    std::vector<BeatmapAudioReference> m_audioReferences;
};

/// @brief 只检查改名是否需要隔离副本，不写谱面或修改项目资源。
class SharedResourceRenameService
{
public:
    /// @brief 复制 worker 所需的打开谱面证据，调用方保证来源在捕获期间稳定。
    static OpenResourceReferenceSnapshot capture(const BeatMap& map,
                                                 bool           audio);

    /// @brief 从独立的工程和打开谱面值快照检查磁盘及引用。
    /// @warning 可阻塞读盘，仅在文件任务或显式离线检查调用。
    static SharedResourceRenamePlan inspectCaptured(
        const Project& project, const std::filesystem::path& currentMap,
        const std::filesystem::path& source, const std::string& newName,
        const AudioResource*                           audio,
        std::span<const OpenResourceReferenceSnapshot> openMaps);

    /// @brief 在物理改名前检查其他谱面的磁盘与未保存内存引用。
    /// @param project 当前正式项目，登记列表决定磁盘检查范围。
    /// @param currentMap
    /// 当前被修改的谱面路径，其他谱面的绑定不能被复制操作改变。
    /// @param source 已选中的音频或视觉资源路径。
    /// @param newName 单个新文件名，省略后缀时沿用源后缀。
    /// @param audio 音频资源快照；为空时仅检查封面与背景。
    /// @param openMaps 已打开谱面的观察指针，调用方需在会话锁内保证其生命周期。
    /// @return 路径校验、共享来源以及是否需要复制的值快照。
    /// @warning 显式确认低频路径，包含文件查询和谱面加载，不得用于每帧渲染。
    static SharedResourceRenamePlan inspect(
        const Project& project, const std::filesystem::path& currentMap,
        const std::filesystem::path& source, const std::string& newName,
        const AudioResource* audio, std::span<const BeatMap* const> openMaps);
};
}  // namespace MMM::Logic
