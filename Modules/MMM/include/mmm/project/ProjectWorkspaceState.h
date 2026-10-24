#pragma once

#include "config/ObjectPlacementSnap.h"
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace MMM
{

/// @file ProjectWorkspaceState.h
/// @brief 可按值复制的项目编辑现场，不依赖完整编辑器配置和视觉配置。
/// @details 字段与 JSON 契约由项目序列化实现维护；此处不拥有会话或 UI 对象。

/// @brief 项目工作区中一个已打开谱面的运行时状态。
/// @details 路径用于重新建立会话，cameraId 和显示名用于恢复对应画布标签身份。
struct ProjectWorkspaceBeatmapState {
    /// @brief 谱面文件路径，优先使用相对项目根目录的 UTF-8 路径。
    std::string m_filePath;

    /// @brief 谱面上次绑定的画布 ID，用于匹配 ImGui 停靠布局。
    std::string m_cameraId;

    /// @brief 画布标题或谱面显示名称。
    std::string m_displayName;

    /// @brief 该谱面上次停留的逻辑播放时间（秒）。
    double m_playbackTime{ 0.0 };

    /// @brief 主画布横向平移量相对保存时视口宽度的比例。
    float m_canvasHorizontalOffsetRatio{ 0.0F };

    /// @brief 序列化项目工作区谱面状态。
    /// @param j 接收对象的 JSON 节点。
    /// @param state 待保存的谱面会话快照。
    friend void to_json(nlohmann::json&                     j,
                        const ProjectWorkspaceBeatmapState& state);

    /// @brief 反序列化项目工作区谱面状态，并兼容旧项目文件。
    /// @param j 来源 JSON 对象。
    /// @param state 接收结果；缺失字段恢复为安全默认值。
    friend void from_json(const nlohmann::json&         j,
                          ProjectWorkspaceBeatmapState& state);
};

/// @brief 项目工作区中的主窗口位置和尺寸。
/// @details 只有 m_valid 为 true 时恢复逻辑才应用其余字段，默认值适合首次启动。
struct ProjectWorkspaceWindowState {
    /// @brief 是否已经记录过有效窗口状态。
    bool m_valid{ false };

    /// @brief 窗口左上角 X 坐标。
    int m_x{ 100 };

    /// @brief 窗口左上角 Y 坐标。
    int m_y{ 100 };

    /// @brief 窗口宽度。
    int m_width{ 1400 };

    /// @brief 窗口高度。
    int m_height{ 900 };

    /// @brief 保存时窗口是否最大化。
    bool m_maximized{ false };

    /// @brief 写出稳定字段，保留输出对象中其他已有的键。
    friend void to_json(nlohmann::json&                    j,
                        const ProjectWorkspaceWindowState& state);

    /// @brief 读取全部必需字段，缺失字段保持原有失败语义。
    friend void from_json(const nlohmann::json&        j,
                          ProjectWorkspaceWindowState& state);
};

/// @brief 项目工作区中已打开的音轨控制器窗口状态。
/// @details 只保存窗口重建所需的资源身份，不保存解码器或播放器运行态。
struct ProjectWorkspaceAudioControllerState {
    /// @brief 音频资源 ID 或常驻音效 ID。
    std::string m_trackId;

    /// @brief 控制器窗口显示名称。
    std::string m_trackName;

    /// @brief 音轨类型，使用 Main 或 Effect 的稳定文本。
    std::string m_trackType{ "Main" };

    /// @brief 写出稳定字段，保留输出对象中其他已有的键。
    friend void to_json(nlohmann::json&                             j,
                        const ProjectWorkspaceAudioControllerState& state);

    /// @brief 读取全部必需字段，缺失字段保持原有失败语义。
    friend void from_json(const nlohmann::json&                 j,
                          ProjectWorkspaceAudioControllerState& state);
};

/// @brief 项目音频工具中一个资源方块的持久化布局与采样音量。
/// @details
/// 坐标和尺寸使用工具画布逻辑单位；宽高为零保留自动布局语义。资源缺失时记录
/// 仍可保留，待同一稳定 ID 再次出现后恢复位置。
struct ProjectAudioToolItemPlacement {
    /// @brief 音频资源稳定 ID。
    std::string m_audioResourceId;

    /// @brief 方块左上角相对工具画布的逻辑 X 坐标。
    float m_x{ 0.0F };

    /// @brief 方块左上角相对工具画布的逻辑 Y 坐标。
    float m_y{ 0.0F };

    /// @brief 用户自定义逻辑宽度；零表示继续按文件名自动计算。
    float m_width{ 0.0F };

    /// @brief 用户自定义逻辑高度；零表示继续使用类型默认高度。
    float m_height{ 0.0F };

    /// @brief 方块叠层顺序，数值越大越靠上。
    std::int32_t m_zOrder{ 0 };

    /// @brief 该采样试听及新建物件使用的倍率；空值表示旧版本尚未独立保存。
    std::optional<float> m_audioVolume;

    /// @brief 序列化项目音频工具方块布局。
    /// @param json 接收对象的 JSON 节点。
    /// @param placement 待保存的逻辑布局。
    friend void to_json(nlohmann::json&                      json,
                        const ProjectAudioToolItemPlacement& placement);

    /// @brief 反序列化项目音频工具方块布局并兼容缺失字段。
    /// @param json 来源 JSON 对象。
    /// @param placement 接收结果；缺失几何字段按自动布局处理。
    friend void from_json(const nlohmann::json&          json,
                          ProjectAudioToolItemPlacement& placement);
};

/// @brief 项目工作区中的工具栏运行时开关状态。
/// @details
/// 这些值是项目关闭时的编辑现场，而不是项目级 EditorSettings 覆盖项。稳定文本
/// 用于保存可扩展枚举；旧版布尔 drawBeatLines 只作为新显示模式的迁移来源。
struct ProjectWorkspaceToolbarState {
    /// @brief 是否已经记录过有效工具栏状态。
    bool m_valid{ false };

    /// @brief 是否反转鼠标滚动方向。
    bool m_reverseScroll{ false };

    /// @brief 是否开启滚动吸附。
    bool m_scrollSnap{ false };

    /// @brief 是否开启物件放置磁吸。
    bool m_objectPlacementSnap{ false };

    /// @brief 物件放置磁吸模式，使用 CurrentBeatDivisor 或 CommonBeatDivisors。
    std::string m_objectPlacementSnapMode{ "CurrentBeatDivisor" };

    /// @brief 1/2 至 1/24 常用分拍线的选择位。
    std::uint32_t m_commonBeatDivisorMask{
        Config::COMMON_BEAT_DIVISOR_MASK_DEFAULT
    };

    /// @brief 是否吸附到早于鼠标位置的分拍线。
    bool m_snapFloor{ false };

    /// @brief 是否启用线性滚动映射。
    bool m_enableLinearScrollMapping{ false };

    /// @brief 是否绘制分拍线；仅用于兼容旧版项目工作区。
    bool m_drawBeatLines{ true };

    /// @brief 分拍线显示模式，使用 Always、NearCursor 或 Hidden 的稳定文本。
    std::string m_beatLineDisplayMode{ "Always" };

    /// @brief 是否在滚动时停止播放。
    bool m_stopPlaybackOnScroll{ false };

    /// @brief 是否启用打击特效动画。
    bool m_enableHitEffects{ true };

    /// @brief 当前分拍数量。
    int m_beatDivisor{ 4 };

    /// @brief 画布时间线缩放倍率。
    float m_timelineZoom{ 1.0f };

    /// @brief 是否同步使用同一主音轨的多个画布时间。
    bool m_syncSameMainAudioCanvases{ true };

    /// @brief 序列化工具栏工作区状态。
    /// @param j 接收对象的 JSON 节点。
    /// @param state 待保存的工具栏状态。
    friend void to_json(nlohmann::json&                     j,
                        const ProjectWorkspaceToolbarState& state);

    /// @brief 反序列化工具栏工作区状态，并兼容旧项目文件。
    /// @param j 来源 JSON 对象。
    /// @param state 接收并规整后的工具栏状态。
    friend void from_json(const nlohmann::json&         j,
                          ProjectWorkspaceToolbarState& state);
};

/// @brief 项目级工作区状态，用于再次打开项目时恢复编辑现场。
/// @details
/// 打开谱面顺序、活动会话、停靠布局和工具窗口状态作为一个项目级快照保存。
/// 字段不携带业务对象所有权；恢复时必须由 EditorEngine 根据当前项目资源重建。
struct ProjectWorkspaceState {
    /// @brief 上次打开的谱面列表，顺序对应画布标签顺序。
    std::vector<ProjectWorkspaceBeatmapState> m_openBeatmaps;

    /// @brief 上次激活谱面的项目相对路径。
    std::string m_activeBeatmapPath;

    /// @brief 上次激活谱面的播放时间（秒）。
    double m_activePlaybackTime{ 0.0 };

    /// @brief 项目专属 Dear ImGui ini 数据，包含窗口位置和 Docking 节点。
    std::string m_imguiIniData;

    /// @brief 主原生窗口的位置、尺寸和最大化状态。
    ProjectWorkspaceWindowState m_mainWindow;

    /// @brief 上次打开的音轨控制器窗口列表。
    std::vector<ProjectWorkspaceAudioControllerState> m_audioControllers;

    /// @brief 上次是否打开了主音轨波形窗口。
    bool m_audioWaveformOpen{ false };

    /// @brief 上次是否打开了主音轨频谱窗口。
    bool m_audioSpectrumOpen{ false };

    /// @brief 上次打开的侧边栏页签，None 表示侧边栏内容收起。
    std::string m_sidebarActiveTab{ "FileExplorer" };

    /// @brief 上次选中的编辑工具。
    std::string m_activeEditTool{ "Move" };

    /// @brief 项目音频工具上次选中的资源 ID。
    std::string m_projectAudioToolSelectedResourceId;

    /// @brief 当前选中采样的画笔倍率快照；旧项目用它迁移该采样独立音量。
    float m_projectAudioToolBrushVolume{ 1.0F };

    /// @brief 选中 Effect 音频资源时是否自动试听一次采样。
    bool m_projectAudioToolPreviewEffectOnSelection{ false };

    /// @brief 方块移动、批量移动及尺寸调整是否自动对齐，旧项目默认保持开启。
    bool m_projectAudioToolSnapEnabled{ true };

    /// @brief 是否在音频工具方块上显示各采样当前的独立音量，旧项目默认隐藏。
    bool m_projectAudioToolShowVolume{ false };

    /// @brief 上次是否打开了项目音频工具窗口。
    bool m_projectAudioToolOpen{ false };

    /// @brief 项目音频工具资源方块的持久化布局。
    std::vector<ProjectAudioToolItemPlacement> m_projectAudioToolPlacements;

    /// @brief 上次工具栏上的运行时开关状态。
    ProjectWorkspaceToolbarState m_toolbarState;

    /// @brief 上次是否打开了 BPM 测量工具。
    bool m_bpmMeasurementToolOpen{ false };

    /// @brief BPM 测量工具上次选中的音频资源 ID。
    std::string m_bpmMeasurementAudioTrackId;

    /// @brief 上次是否打开了时间点批量编辑表格。
    bool m_timingPointsTableOpen{ false };

    /// @brief 上次是否打开了批注表。
    bool m_annotationTableOpen{ false };

    /// @brief 上次是否打开了重叠检测工具窗口。
    bool m_overlapCheckOpen{ false };

    /// @brief 上次是否打开了谱面额外元数据编辑窗口。
    bool m_metadataEditorOpen{ false };

    /// @brief 上次是否打开了音符元数据编辑窗口。
    bool m_noteMetadataEditorOpen{ false };

    /// @brief 序列化项目工作区状态。
    /// @param j 接收对象的 JSON 节点。
    /// @param workspace 待保存的完整工作区快照。
    friend void to_json(nlohmann::json&              j,
                        const ProjectWorkspaceState& workspace);

    /// @brief 反序列化项目工作区状态，并兼容旧项目文件。
    /// @param j 来源 JSON 对象。
    /// @param workspace 接收结果；缺失字段使用首次打开时的默认值。
    friend void from_json(const nlohmann::json&  j,
                          ProjectWorkspaceState& workspace);
};

}  // namespace MMM
