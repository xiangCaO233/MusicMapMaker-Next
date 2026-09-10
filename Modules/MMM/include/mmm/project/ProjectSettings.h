#pragma once
#include "config/EditorSettings.h"
#include "config/VisualConfig.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace MMM
{

/**
 * @file ProjectSettings.h
 * @brief 定义随项目保存的覆盖配置与编辑工作区快照。
 *
 * 工作区字段只描述再次打开项目时可恢复的编辑现场，不参与谱面内容语义。
 * 项目覆盖配置仅保存允许随项目迁移的选项；软件级隐私、诊断、欢迎页与工具栏
 * 外观偏好必须继续由全局配置拥有。反序列化需要兼容旧项目缺失字段，并在读取
 * 后规整非有限数值、未知枚举文本与越界位掩码，避免持久化数据直接污染运行态。
 */

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
                        const ProjectWorkspaceBeatmapState& state)
    {
        // 所有坐标均保存逻辑量或比例，不持久化当前窗口的物理像素地址。
        j = nlohmann::json{
            { "m_filePath", state.m_filePath },
            { "m_cameraId", state.m_cameraId },
            { "m_displayName", state.m_displayName },
            { "m_playbackTime", state.m_playbackTime },
            { "m_canvasHorizontalOffsetRatio",
              state.m_canvasHorizontalOffsetRatio },
        };
    }

    /// @brief 反序列化项目工作区谱面状态，并兼容旧项目文件。
    /// @param j 来源 JSON 对象。
    /// @param state 接收结果；缺失字段恢复为安全默认值。
    friend void from_json(const nlohmann::json&         j,
                          ProjectWorkspaceBeatmapState& state)
    {
        // value 读取使旧项目可以缺少后来加入的画布身份和播放状态字段。
        state.m_filePath     = j.value("m_filePath", std::string{});
        state.m_cameraId     = j.value("m_cameraId", std::string{});
        state.m_displayName  = j.value("m_displayName", std::string{});
        state.m_playbackTime = j.value("m_playbackTime", 0.0);
        state.m_canvasHorizontalOffsetRatio =
            j.value("m_canvasHorizontalOffsetRatio", 0.0F);
        if ( !std::isfinite(state.m_canvasHorizontalOffsetRatio) ) {
            // 非有限比例不能参与视口乘法，统一回到无横向平移。
            state.m_canvasHorizontalOffsetRatio = 0.0F;
        }
    }
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

    /// @brief 使用稳定字段名序列化和反序列化原生窗口状态。
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectWorkspaceWindowState, m_valid, m_x,
                                   m_y, m_width, m_height, m_maximized)
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

    /// @brief 使用稳定字段名序列化和反序列化控制器窗口身份。
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectWorkspaceAudioControllerState,
                                   m_trackId, m_trackName, m_trackType)
};

/// @brief 项目音频工具中一个资源方块的持久化布局。
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

    /// @brief 序列化项目音频工具方块布局。
    /// @param json 接收对象的 JSON 节点。
    /// @param placement 待保存的逻辑布局。
    friend void to_json(nlohmann::json&                      json,
                        const ProjectAudioToolItemPlacement& placement)
    {
        // 仅保存稳定资源 ID 和逻辑几何，不保存瞬态选择与拖动状态。
        json = nlohmann::json{
            { "m_audioResourceId", placement.m_audioResourceId },
            { "m_x", placement.m_x },
            { "m_y", placement.m_y },
            { "m_width", placement.m_width },
            { "m_height", placement.m_height },
            { "m_zOrder", placement.m_zOrder },
        };
    }

    /// @brief 反序列化项目音频工具方块布局并兼容缺失字段。
    /// @param json 来源 JSON 对象。
    /// @param placement 接收结果；缺失几何字段按自动布局处理。
    friend void from_json(const nlohmann::json&          json,
                          ProjectAudioToolItemPlacement& placement)
    {
        // 逐字段 value 读取允许旧项目仅保存资源身份。
        placement.m_audioResourceId =
            json.value("m_audioResourceId", std::string{});
        placement.m_x      = json.value("m_x", 0.0F);
        placement.m_y      = json.value("m_y", 0.0F);
        placement.m_width  = json.value("m_width", 0.0F);
        placement.m_height = json.value("m_height", 0.0F);
        placement.m_zOrder = json.value("m_zOrder", 0);
    }
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
                        const ProjectWorkspaceToolbarState& state)
    {
        // 同时写出旧布尔字段和新文本模式，兼容尚未升级的读取端。
        j = nlohmann::json{
            { "m_valid", state.m_valid },
            { "m_reverseScroll", state.m_reverseScroll },
            { "m_scrollSnap", state.m_scrollSnap },
            { "m_objectPlacementSnap", state.m_objectPlacementSnap },
            { "m_objectPlacementSnapMode", state.m_objectPlacementSnapMode },
            { "m_commonBeatDivisorMask", state.m_commonBeatDivisorMask },
            { "m_snapFloor", state.m_snapFloor },
            { "m_enableLinearScrollMapping",
              state.m_enableLinearScrollMapping },
            { "m_drawBeatLines", state.m_beatLineDisplayMode != "Hidden" },
            { "m_beatLineDisplayMode", state.m_beatLineDisplayMode },
            { "m_stopPlaybackOnScroll", state.m_stopPlaybackOnScroll },
            { "m_enableHitEffects", state.m_enableHitEffects },
            { "m_beatDivisor", state.m_beatDivisor },
            { "m_timelineZoom", state.m_timelineZoom },
            { "m_syncSameMainAudioCanvases", state.m_syncSameMainAudioCanvases }
        };
    }

    /// @brief 反序列化工具栏工作区状态，并兼容旧项目文件。
    /// @param j 来源 JSON 对象。
    /// @param state 接收并规整后的工具栏状态。
    friend void from_json(const nlohmann::json&         j,
                          ProjectWorkspaceToolbarState& state)
    {
        // 基础开关逐项读取默认值，避免缺失字段继承复用对象的旧状态。
        state.m_valid         = j.value("m_valid", false);
        state.m_reverseScroll = j.value("m_reverseScroll", false);
        state.m_scrollSnap    = j.value("m_scrollSnap", false);
        state.m_objectPlacementSnap =
            j.value("m_objectPlacementSnap", state.m_scrollSnap);
        // 旧项目没有独立物件磁吸开关时沿用原滚动吸附值。
        state.m_objectPlacementSnapMode = j.value(
            "m_objectPlacementSnapMode", std::string{ "CurrentBeatDivisor" });
        if ( state.m_objectPlacementSnapMode != "CurrentBeatDivisor" &&
             state.m_objectPlacementSnapMode != "CommonBeatDivisors" ) {
            // 未知模式回退到当前分拍，不能把任意文本带入命令分派。
            state.m_objectPlacementSnapMode = "CurrentBeatDivisor";
        }
        // 与已知位集合相交，清除未来或损坏项目中的未定义选择位。
        state.m_commonBeatDivisorMask =
            j.value("m_commonBeatDivisorMask",
                    Config::COMMON_BEAT_DIVISOR_MASK_DEFAULT) &
            Config::COMMON_BEAT_DIVISOR_MASK_ALL;
        state.m_snapFloor = j.value("m_snapFloor", false);
        state.m_enableLinearScrollMapping =
            j.value("m_enableLinearScrollMapping", false);
        state.m_drawBeatLines = j.value("m_drawBeatLines", true);
        if ( j.contains("m_beatLineDisplayMode") ) {
            // 新项目以稳定文本为权威来源，旧布尔值只用于异常回退。
            state.m_beatLineDisplayMode =
                j.value("m_beatLineDisplayMode", std::string{ "Always" });
            if ( state.m_beatLineDisplayMode != "Always" &&
                 state.m_beatLineDisplayMode != "NearCursor" &&
                 state.m_beatLineDisplayMode != "Hidden" ) {
                // 未知文本按旧字段恢复可见或隐藏，不默认为半自动模式。
                state.m_beatLineDisplayMode =
                    state.m_drawBeatLines ? "Always" : "Hidden";
            }
        } else {
            // 旧项目没有文本模式，直接从 drawBeatLines 迁移二态语义。
            state.m_beatLineDisplayMode =
                state.m_drawBeatLines ? "Always" : "Hidden";
        }
        // 重新派生兼容布尔值，保证它与最终规整后的文本模式一致。
        state.m_drawBeatLines        = state.m_beatLineDisplayMode != "Hidden";
        state.m_stopPlaybackOnScroll = j.value("m_stopPlaybackOnScroll", false);
        state.m_enableHitEffects     = j.value("m_enableHitEffects", true);
        state.m_beatDivisor          = j.value("m_beatDivisor", 4);
        state.m_timelineZoom         = j.value("m_timelineZoom", 1.0f);
        state.m_syncSameMainAudioCanvases =
            j.value("m_syncSameMainAudioCanvases", true);
    }
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

    /// @brief 项目音频工具为后续新建物件设置的音量倍率。
    float m_projectAudioToolBrushVolume{ 1.0F };

    /// @brief 选中 Effect 音频资源时是否自动试听一次采样。
    bool m_projectAudioToolPreviewEffectOnSelection{ false };

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
                        const ProjectWorkspaceState& workspace)
    {
        // 按职责分组写出会话、原生窗口、音频工具和编辑工具状态。
        j = nlohmann::json{
            { "m_openBeatmaps", workspace.m_openBeatmaps },
            { "m_activeBeatmapPath", workspace.m_activeBeatmapPath },
            { "m_activePlaybackTime", workspace.m_activePlaybackTime },
            { "m_imguiIniData", workspace.m_imguiIniData },
            { "m_mainWindow", workspace.m_mainWindow },
            { "m_audioControllers", workspace.m_audioControllers },
            { "m_audioWaveformOpen", workspace.m_audioWaveformOpen },
            { "m_audioSpectrumOpen", workspace.m_audioSpectrumOpen },
            { "m_sidebarActiveTab", workspace.m_sidebarActiveTab },
            { "m_activeEditTool", workspace.m_activeEditTool },
            { "m_projectAudioToolSelectedResourceId",
              workspace.m_projectAudioToolSelectedResourceId },
            { "m_projectAudioToolBrushVolume",
              workspace.m_projectAudioToolBrushVolume },
            { "m_projectAudioToolPreviewEffectOnSelection",
              workspace.m_projectAudioToolPreviewEffectOnSelection },
            { "m_projectAudioToolOpen", workspace.m_projectAudioToolOpen },
            { "m_projectAudioToolPlacements",
              workspace.m_projectAudioToolPlacements },
            { "m_toolbarState", workspace.m_toolbarState },
            { "m_bpmMeasurementToolOpen", workspace.m_bpmMeasurementToolOpen },
            { "m_bpmMeasurementAudioTrackId",
              workspace.m_bpmMeasurementAudioTrackId },
            { "m_timingPointsTableOpen", workspace.m_timingPointsTableOpen },
            { "m_annotationTableOpen", workspace.m_annotationTableOpen },
            { "m_overlapCheckOpen", workspace.m_overlapCheckOpen },
            { "m_metadataEditorOpen", workspace.m_metadataEditorOpen },
            { "m_noteMetadataEditorOpen", workspace.m_noteMetadataEditorOpen }
        };
    }

    /// @brief 反序列化项目工作区状态，并兼容旧项目文件。
    /// @param j 来源 JSON 对象。
    /// @param workspace 接收结果；缺失字段使用首次打开时的默认值。
    friend void from_json(const nlohmann::json&  j,
                          ProjectWorkspaceState& workspace)
    {
        // 会话列表保持保存顺序，用于重建同样的画布标签顺序。
        workspace.m_openBeatmaps = j.value(
            "m_openBeatmaps", std::vector<ProjectWorkspaceBeatmapState>{});
        workspace.m_activeBeatmapPath =
            j.value("m_activeBeatmapPath", std::string{});
        workspace.m_activePlaybackTime = j.value("m_activePlaybackTime", 0.0);
        workspace.m_imguiIniData = j.value("m_imguiIniData", std::string{});
        // 复合状态由各自 from_json 继续执行字段级兼容迁移。
        workspace.m_mainWindow =
            j.value("m_mainWindow", ProjectWorkspaceWindowState{});
        workspace.m_audioControllers =
            j.value("m_audioControllers",
                    std::vector<ProjectWorkspaceAudioControllerState>{});
        workspace.m_audioWaveformOpen = j.value("m_audioWaveformOpen", false);
        workspace.m_audioSpectrumOpen = j.value("m_audioSpectrumOpen", false);
        workspace.m_sidebarActiveTab =
            j.value("m_sidebarActiveTab", std::string{ "FileExplorer" });
        workspace.m_activeEditTool =
            j.value("m_activeEditTool", std::string{ "Move" });
        workspace.m_projectAudioToolSelectedResourceId =
            j.value("m_projectAudioToolSelectedResourceId", std::string{});
        // 画笔音量来自持久化输入，先读取再执行有限性和下边界规整。
        workspace.m_projectAudioToolBrushVolume =
            j.value("m_projectAudioToolBrushVolume", 1.0F);
        if ( !std::isfinite(workspace.m_projectAudioToolBrushVolume) ) {
            // NaN 或无穷无法作为音量倍率，回退到原音量。
            workspace.m_projectAudioToolBrushVolume = 1.0F;
        }
        // 负倍率没有播放语义，最接近的有效边界是静音零值。
        workspace.m_projectAudioToolBrushVolume =
            std::max(0.0F, workspace.m_projectAudioToolBrushVolume);
        workspace.m_projectAudioToolPreviewEffectOnSelection =
            j.value("m_projectAudioToolPreviewEffectOnSelection", false);
        workspace.m_projectAudioToolOpen =
            j.value("m_projectAudioToolOpen", false);
        workspace.m_projectAudioToolPlacements =
            j.value("m_projectAudioToolPlacements",
                    std::vector<ProjectAudioToolItemPlacement>{});
        // 工具栏子对象负责显示模式和位掩码等内部规整。
        workspace.m_toolbarState =
            j.value("m_toolbarState", ProjectWorkspaceToolbarState{});
        workspace.m_bpmMeasurementToolOpen =
            j.value("m_bpmMeasurementToolOpen", false);
        workspace.m_bpmMeasurementAudioTrackId =
            j.value("m_bpmMeasurementAudioTrackId", std::string{});
        workspace.m_timingPointsTableOpen =
            j.value("m_timingPointsTableOpen", false);
        workspace.m_annotationTableOpen =
            j.value("m_annotationTableOpen", false);
        workspace.m_overlapCheckOpen   = j.value("m_overlapCheckOpen", false);
        workspace.m_metadataEditorOpen = j.value("m_metadataEditorOpen", false);
        workspace.m_noteMetadataEditorOpen =
            j.value("m_noteMetadataEditorOpen", false);
    }
};

/// @brief 项目级的特定偏好设置
/// @details
/// 包含可以覆盖全局配置的可选项，以及最后一次的状态记录。软件级隐私、诊断、
/// PGO 上传、欢迎页和工具栏外观不会随项目传播，序列化时显式剔除这些字段。
struct ProjectSettings {
    /// @brief 覆盖全局视觉配置 (若为 nullopt 则继承全局 .config/mmm 配置)
    std::optional<Config::VisualConfig> m_visualOverride;

    /// @brief 覆盖全局编辑器行为 (若为 nullopt 则继承全局 .config/mmm 配置)
    std::optional<Config::EditorSettings> m_editorOverride;

    /// @brief 覆盖软件级谱面自动备份配置；空值表示继承软件配置。
    std::optional<Config::AutoBackupConfig> m_autoBackupOverride;

    /// @brief 项目中最后一次打开的谱面名称 (BeatmapEntry::m_name)
    std::string m_lastOpenedBeatmap;

    /// @brief 项目打开时应用的调色方案；新项目默认留空以继承软件默认。
    std::string m_colorPaletteSchemeName;

    /// @brief 项目级工作区状态。
    ProjectWorkspaceState m_workspace;

    /// @brief 序列化项目设置。
    /// @param j 接收对象的 JSON 节点。
    /// @param settings 待保存的项目覆盖配置和工作区。
    friend void to_json(nlohmann::json& j, const ProjectSettings& settings)
    {
        // 空覆盖保持 JSON null，表示打开项目后继续继承全局设置。
        nlohmann::json editorOverrideJson = nullptr;
        if ( settings.m_editorOverride ) {
            // 使用副本剔除软件级字段，不能修改会话正在使用的覆盖对象。
            auto editorOverride = *settings.m_editorOverride;
            // 调色板内容由皮肤和项目方案名管理，不复制完整全局色表。
            editorOverride.colorPalettes = Config::ColorPaletteConfig();
            editorOverride.defaultColorPaletteSchemeName =
                Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
            // 先复用 EditorSettings 序列化，再删除不允许项目拥有的字段。
            editorOverrideJson = editorOverride;
            // 工具栏展示属于用户界面偏好，同一项目在不同用户间不应强制同步。
            editorOverrideJson.erase("showToolLabels");
            editorOverrideJson.erase("fixedToolWindow");
            editorOverrideJson.erase("showManagerLabels");
            editorOverrideJson.erase("toolbarVisibility");
            // 上传同意、诊断和欢迎页均是软件级隐私或启动行为。
            editorOverrideJson.erase("autoUploadPgoProfiles");
            editorOverrideJson.erase("pgoProfileUploadConsentAsked");
            editorOverrideJson.erase("showWelcomeOnStartup");
            editorOverrideJson.erase("rtcDiagnosticLogging");
        }
        // 最终对象只包含允许项目迁移的覆盖项和可恢复工作区。
        j = nlohmann::json{
            { "m_visualOverride", settings.m_visualOverride },
            { "m_editorOverride", editorOverrideJson },
            { "m_autoBackupOverride", settings.m_autoBackupOverride },
            { "m_lastOpenedBeatmap", settings.m_lastOpenedBeatmap },
            { "m_colorPaletteSchemeName", settings.m_colorPaletteSchemeName },
            { "m_workspace", settings.m_workspace }
        };
    }

    /// @brief 反序列化项目设置，并兼容缺少工作区字段的旧项目。
    /// @param j 来源 JSON 对象。
    /// @param settings 接收并完成软件级字段隔离后的项目设置。
    friend void from_json(const nlohmann::json& j, ProjectSettings& settings)
    {
        // 可选覆盖以 null 或缺失表示继承，不保留复用对象中的旧值。
        if ( auto it = j.find("m_visualOverride");
             it != j.end() && !it->is_null() ) {
            settings.m_visualOverride = it->get<Config::VisualConfig>();
        } else {
            settings.m_visualOverride = std::nullopt;
        }

        if ( auto it = j.find("m_editorOverride");
             it != j.end() && !it->is_null() ) {
            settings.m_editorOverride = it->get<Config::EditorSettings>();
            // 即使旧项目曾保存工具栏外观，也恢复为当前软件全局默认语义。
            Config::preserveGlobalToolbarDisplaySettings(
                *settings.m_editorOverride, Config::EditorSettings{});
            // 项目文件不得启用上传、伪造同意状态或修改启动欢迎页策略。
            settings.m_editorOverride->autoUploadPgoProfiles        = false;
            settings.m_editorOverride->pgoProfileUploadConsentAsked = false;
            settings.m_editorOverride->m_showWelcomeOnStartup       = true;
            settings.m_editorOverride->rtcDiagnosticLogging         = false;
        } else {
            settings.m_editorOverride = std::nullopt;
        }

        if ( auto it = j.find("m_autoBackupOverride");
             it != j.end() && !it->is_null() ) {
            settings.m_autoBackupOverride = it->get<Config::AutoBackupConfig>();
        } else {
            settings.m_autoBackupOverride = std::nullopt;
        }

        // 标量后增字段使用 value，旧项目可无损获得当前默认值。
        settings.m_lastOpenedBeatmap =
            j.value("m_lastOpenedBeatmap", std::string{});
        settings.m_colorPaletteSchemeName =
            j.value("m_colorPaletteSchemeName",
                    std::string(Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID));
        // 缺少工作区的旧项目按首次打开处理，不复用其他项目现场。
        settings.m_workspace = j.value("m_workspace", ProjectWorkspaceState{});
    }
};

}  // namespace MMM
