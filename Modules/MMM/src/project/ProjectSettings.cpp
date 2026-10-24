#include "mmm/project/ProjectSettings.h"

#include "config/EditorSettings.h"
#include "config/VisualConfig.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace MMM
{

/// @file
/// @brief 项目持久化的字段转换实现，仅在打开和保存项目时调用。
/// @details 模型仍按值存储；JSON 模板实例化不再传播到只访问数据的调用端。
/// 序列化只转换快照，不访问文件系统、窗口、会话或全局配置单例。
/// 兼容迁移的作用域保持在对应子对象，外层只组织字段，不额外规整子对象。

/// @brief 写出 ProjectWorkspaceAudioControllerState 的既有必需字段。
/// @param j 接收字段的 JSON 节点，已有的无关字段继续保留。
/// @param state 待保存的值对象。
void to_json(nlohmann::json&                             j,
             const ProjectWorkspaceAudioControllerState& state)
{
    // 保留原宏逐字段赋值的行为，不能用整体赋值清除调用方已有的键。
    // 资源 ID 用于重建控制器，名称仅用于恢复展示，二者不能相互代替。
    j["m_trackId"]   = state.m_trackId;
    j["m_trackName"] = state.m_trackName;
    // 类型沿用稳定文本，写入时不根据当前资源是否存在而过滤窗口记录。
    j["m_trackType"] = state.m_trackType;
}

/// @brief 读取 ProjectWorkspaceAudioControllerState 的既有必需字段。
/// @param j 来源 JSON 对象，字段缺失继续由 at 报告失败。
/// @param state 接收读取结果的值对象。
void from_json(const nlohmann::json&                 j,
               ProjectWorkspaceAudioControllerState& state)
{
    // 不引入默认回退或整对象重置，保持旧宏的必需字段及更新顺序。
    // 只有整个子对象缺失时，外层工作区才提供默认值；存在时三项均必需。
    j.at("m_trackId").get_to(state.m_trackId);
    j.at("m_trackName").get_to(state.m_trackName);
    j.at("m_trackType").get_to(state.m_trackType);
}

/// @brief 写出 ProjectWorkspaceWindowState 的既有必需字段。
/// @param j 接收字段的 JSON 节点，已有的无关字段继续保留。
/// @param state 待保存的值对象。
void to_json(nlohmann::json& j, const ProjectWorkspaceWindowState& state)
{
    // 保留原宏逐字段赋值的行为，不能用整体赋值清除调用方已有的键。
    // 无效快照仍保存完整几何，是否应用窗口位置由恢复流程根据标志决定。
    j["m_valid"] = state.m_valid;
    // 不在保存阶段夹取屏幕边界，允许项目在另一套显示器布局上再恢复。
    j["m_x"]         = state.m_x;
    j["m_y"]         = state.m_y;
    j["m_width"]     = state.m_width;
    j["m_height"]    = state.m_height;
    j["m_maximized"] = state.m_maximized;
}

/// @brief 读取 ProjectWorkspaceWindowState 的既有必需字段。
/// @param j 来源 JSON 对象，字段缺失继续由 at 报告失败。
/// @param state 接收读取结果的值对象。
void from_json(const nlohmann::json& j, ProjectWorkspaceWindowState& state)
{
    // 不引入默认回退或整对象重置，保持旧宏的必需字段及更新顺序。
    // m_valid 为 false 也不能跳过其余必需字段，否则会放宽已有格式契约。
    j.at("m_valid").get_to(state.m_valid);
    // 几何值原样读取；显示器和窗口约束仍由实际恢复窗口的调用端处理。
    j.at("m_x").get_to(state.m_x);
    j.at("m_y").get_to(state.m_y);
    j.at("m_width").get_to(state.m_width);
    j.at("m_height").get_to(state.m_height);
    j.at("m_maximized").get_to(state.m_maximized);
}

/// @brief 序列化项目工作区谱面状态。
/// @param j 接收对象的 JSON 节点。
/// @param state 待保存的谱面会话快照。
/// @note 只记录身份与逻辑视图状态，不捕获谱面或摄像机对象的所有权。
void to_json(nlohmann::json& j, const ProjectWorkspaceBeatmapState& state)
{
    // 所有坐标均保存逻辑量或比例，不持久化当前窗口的物理像素地址。
    j = nlohmann::json{
        { "m_filePath", state.m_filePath },
        { "m_cameraId", state.m_cameraId },
        { "m_displayName", state.m_displayName },
        { "m_playbackTime", state.m_playbackTime },
        // 比例跨窗口尺寸恢复，不能替换为保存瞬间的像素偏移。
        { "m_canvasHorizontalOffsetRatio",
          state.m_canvasHorizontalOffsetRatio },
    };
}

/// @brief 反序列化项目工作区谱面状态，并兼容旧项目文件。
/// @param j 来源 JSON 对象。
/// @param state 接收结果；缺失字段恢复为安全默认值。
/// @note 每个字段独立回退，因此复用对象不会继承其他谱面的视图身份。
void from_json(const nlohmann::json& j, ProjectWorkspaceBeatmapState& state)
{
    // value 读取使旧项目可以缺少后来加入的画布身份和播放状态字段。
    state.m_filePath    = j.value("m_filePath", std::string{});
    state.m_cameraId    = j.value("m_cameraId", std::string{});
    state.m_displayName = j.value("m_displayName", std::string{});
    // 播放位置的有效区间由会话恢复流程掌握，此处不额外引入时长限制。
    state.m_playbackTime = j.value("m_playbackTime", 0.0);
    state.m_canvasHorizontalOffsetRatio =
        j.value("m_canvasHorizontalOffsetRatio", 0.0F);
    if ( !std::isfinite(state.m_canvasHorizontalOffsetRatio) ) {
        // 非有限比例不能参与视口乘法，统一回到无横向平移。
        state.m_canvasHorizontalOffsetRatio = 0.0F;
    }
}

/// @brief 序列化项目音频工具方块布局。
/// @param json 接收对象的 JSON 节点。
/// @param placement 待保存的逻辑布局。
/// @note 空的独立音量与显式 1.0 倍具有不同迁移语义，不能互相替换。
void to_json(nlohmann::json&                      json,
             const ProjectAudioToolItemPlacement& placement)
{
    // 稳定资源 ID 同时关联几何和采样倍率，不保存瞬态选择与拖动状态。
    json = nlohmann::json{
        { "m_audioResourceId", placement.m_audioResourceId },
        // 宽高零值表示自动布局，保存时不依据当前字体或文件名预先展开。
        { "m_x", placement.m_x },
        { "m_y", placement.m_y },
        { "m_width", placement.m_width },
        { "m_height", placement.m_height },
        { "m_zOrder", placement.m_zOrder },
    };
    // 缺失独立音量的旧记录保持缺失，供工具只迁移原选中采样的画笔倍率。
    if ( placement.m_audioVolume ) {
        json["m_audioVolume"] = *placement.m_audioVolume;
    }
}

/// @brief 反序列化项目音频工具方块布局并兼容缺失字段。
/// @param json 来源 JSON 对象。
/// @param placement 接收结果；缺失几何字段按自动布局处理。
/// @note 几何字段保留历史读取行为，只有独立音量在这里执行倍率规整。
void from_json(const nlohmann::json&          json,
               ProjectAudioToolItemPlacement& placement)
{
    // 逐字段 value 读取允许旧项目仅保存资源身份。
    placement.m_audioResourceId =
        json.value("m_audioResourceId", std::string{});
    placement.m_x = json.value("m_x", 0.0F);
    placement.m_y = json.value("m_y", 0.0F);
    // 保留零尺寸的自动布局标记，避免旧记录升级后变成固定大小。
    placement.m_width  = json.value("m_width", 0.0F);
    placement.m_height = json.value("m_height", 0.0F);
    placement.m_zOrder = json.value("m_zOrder", 0);
    // 复用对象先清空旧值；不把别的采样音量带入缺失字段的历史记录。
    placement.m_audioVolume.reset();
    const auto volume = json.find("m_audioVolume");
    if ( volume != json.end() && volume->is_number() ) {
        // 只接收有限倍率；损坏值按原音量恢复，显示和播放统一限于 0～200。
        const float value = volume->get<float>();
        placement.m_audioVolume =
            std::isfinite(value) ? std::clamp(value, 0.0F, 2.0F) : 1.0F;
    }
}

/// @brief 序列化工具栏工作区状态。
/// @param j 接收对象的 JSON 节点。
/// @param state 待保存的工具栏状态。
/// @note 兼容布尔字段由文本显示模式派生，不能使用可能过时的缓存布尔值。
void to_json(nlohmann::json& j, const ProjectWorkspaceToolbarState& state)
{
    // 同时写出旧布尔字段和新文本模式，兼容尚未升级的读取端。
    j = nlohmann::json{
        { "m_valid", state.m_valid },
        { "m_reverseScroll", state.m_reverseScroll },
        { "m_scrollSnap", state.m_scrollSnap },
        // 滚动吸附和物件吸附独立保存，旧项目兼容仅发生在读取一侧。
        { "m_objectPlacementSnap", state.m_objectPlacementSnap },
        { "m_objectPlacementSnapMode", state.m_objectPlacementSnapMode },
        { "m_commonBeatDivisorMask", state.m_commonBeatDivisorMask },
        // 吸附方向与线性映射各自保留，不能从分拍选择推导。
        { "m_snapFloor", state.m_snapFloor },
        { "m_enableLinearScrollMapping", state.m_enableLinearScrollMapping },
        { "m_drawBeatLines", state.m_beatLineDisplayMode != "Hidden" },
        { "m_beatLineDisplayMode", state.m_beatLineDisplayMode },
        // 播放行为和特效开关属于项目编辑现场，而非软件级工具栏外观。
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
/// @note 先迁移旧字段，再确定新字段的权威值，最后刷新兼容缓存。
void from_json(const nlohmann::json& j, ProjectWorkspaceToolbarState& state)
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
    // 映射默认值沿用旧工作区，不根据当前全局配置改变历史项目现场。
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
    // 分拍与缩放保持原始恢复契约，实际使用端继续承担自身范围约束。
    state.m_beatDivisor  = j.value("m_beatDivisor", 4);
    state.m_timelineZoom = j.value("m_timelineZoom", 1.0f);
    state.m_syncSameMainAudioCanvases =
        j.value("m_syncSameMainAudioCanvases", true);
}

/// @brief 序列化项目工作区状态。
/// @param j 接收对象的 JSON 节点。
/// @param workspace 待保存的完整工作区快照。
/// @note 列表顺序即标签与控制器恢复顺序，不能在持久化层排序或去重。
/// @warning 仅在低频保存快照时调用；不得移入每帧或每 update 路径。
void to_json(nlohmann::json& j, const ProjectWorkspaceState& workspace)
{
    // 按职责分组写出会话、原生窗口、音频工具和编辑工具状态。
    j = nlohmann::json{
        { "m_openBeatmaps", workspace.m_openBeatmaps },
        { "m_activeBeatmapPath", workspace.m_activeBeatmapPath },
        { "m_activePlaybackTime", workspace.m_activePlaybackTime },
        // 停靠文本和原生窗口位置分开保存，分别交给对应恢复入口消费。
        { "m_imguiIniData", workspace.m_imguiIniData },
        { "m_mainWindow", workspace.m_mainWindow },
        { "m_audioControllers", workspace.m_audioControllers },
        // 波形与频谱是独立工具窗口，不依赖音轨控制器是否处于打开状态。
        { "m_audioWaveformOpen", workspace.m_audioWaveformOpen },
        { "m_audioSpectrumOpen", workspace.m_audioSpectrumOpen },
        { "m_sidebarActiveTab", workspace.m_sidebarActiveTab },
        { "m_activeEditTool", workspace.m_activeEditTool },
        // 选择资源与画笔快照一起保存，但画笔倍率不覆盖各采样独立倍率。
        { "m_projectAudioToolSelectedResourceId",
          workspace.m_projectAudioToolSelectedResourceId },
        { "m_projectAudioToolBrushVolume",
          workspace.m_projectAudioToolBrushVolume },
        { "m_projectAudioToolPreviewEffectOnSelection",
          workspace.m_projectAudioToolPreviewEffectOnSelection },
        { "m_projectAudioToolSnapEnabled",
          workspace.m_projectAudioToolSnapEnabled },
        { "m_projectAudioToolShowVolume",
          workspace.m_projectAudioToolShowVolume },
        { "m_projectAudioToolOpen", workspace.m_projectAudioToolOpen },
        // 方块布局与工具窗口开关独立；关闭窗口后仍保留用户排布。
        { "m_projectAudioToolPlacements",
          workspace.m_projectAudioToolPlacements },
        { "m_toolbarState", workspace.m_toolbarState },
        { "m_bpmMeasurementToolOpen", workspace.m_bpmMeasurementToolOpen },
        // 测量目标用稳定资源身份记录，恢复时再解析实际音频资源。
        { "m_bpmMeasurementAudioTrackId",
          workspace.m_bpmMeasurementAudioTrackId },
        { "m_timingPointsTableOpen", workspace.m_timingPointsTableOpen },
        // 工具打开标志只描述编辑现场，不把对应表格内容重复写入项目设置。
        { "m_annotationTableOpen", workspace.m_annotationTableOpen },
        { "m_overlapCheckOpen", workspace.m_overlapCheckOpen },
        { "m_metadataEditorOpen", workspace.m_metadataEditorOpen },
        { "m_noteMetadataEditorOpen", workspace.m_noteMetadataEditorOpen }
    };
}

/// @brief 反序列化项目工作区状态，并兼容旧项目文件。
/// @param j 来源 JSON 对象。
/// @param workspace 接收结果；缺失字段使用首次打开时的默认值。
/// @note 缺失子对象与缺失子字段分别由外层默认值和子对象转换器处理。
/// @warning 仅在低频加载项目时调用；不得在播放循环中反复执行兼容迁移。
void from_json(const nlohmann::json& j, ProjectWorkspaceState& workspace)
{
    // 会话列表保持保存顺序，用于重建同样的画布标签顺序。
    workspace.m_openBeatmaps =
        j.value("m_openBeatmaps", std::vector<ProjectWorkspaceBeatmapState>{});
    workspace.m_activeBeatmapPath =
        j.value("m_activeBeatmapPath", std::string{});
    workspace.m_activePlaybackTime = j.value("m_activePlaybackTime", 0.0);
    workspace.m_imguiIniData       = j.value("m_imguiIniData", std::string{});
    // 不在解析阶段操作原生窗口或 ImGui，项目打开后再按生命周期恢复。
    // 复合状态由各自 from_json 继续执行字段级兼容迁移。
    workspace.m_mainWindow =
        j.value("m_mainWindow", ProjectWorkspaceWindowState{});
    workspace.m_audioControllers =
        j.value("m_audioControllers",
                std::vector<ProjectWorkspaceAudioControllerState>{});
    workspace.m_audioWaveformOpen = j.value("m_audioWaveformOpen", false);
    workspace.m_audioSpectrumOpen = j.value("m_audioSpectrumOpen", false);
    // 默认页签和编辑工具保持历史首次打开行为，不读取当前界面瞬态值。
    workspace.m_sidebarActiveTab =
        j.value("m_sidebarActiveTab", std::string{ "FileExplorer" });
    workspace.m_activeEditTool =
        j.value("m_activeEditTool", std::string{ "Move" });
    workspace.m_projectAudioToolSelectedResourceId =
        j.value("m_projectAudioToolSelectedResourceId", std::string{});
    // 资源可能尚未扫描完成，此处保留身份，不因暂时不可解析而清空选择。
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
    // 历史工作区没有此字段，默认开启以保持升级前的方块对齐行为。
    workspace.m_projectAudioToolSnapEnabled =
        j.value("m_projectAudioToolSnapEnabled", true);
    // 只恢复显示偏好，不从画笔快照推导各采样的实际音量。
    workspace.m_projectAudioToolShowVolume =
        j.value("m_projectAudioToolShowVolume", false);
    workspace.m_projectAudioToolOpen = j.value("m_projectAudioToolOpen", false);
    workspace.m_projectAudioToolPlacements =
        j.value("m_projectAudioToolPlacements",
                std::vector<ProjectAudioToolItemPlacement>{});
    // 子对象各自处理旧版字段，外层不重复推导工具栏或采样配置。
    // 工具栏子对象负责显示模式和位掩码等内部规整。
    workspace.m_toolbarState =
        j.value("m_toolbarState", ProjectWorkspaceToolbarState{});
    workspace.m_bpmMeasurementToolOpen =
        j.value("m_bpmMeasurementToolOpen", false);
    workspace.m_bpmMeasurementAudioTrackId =
        j.value("m_bpmMeasurementAudioTrackId", std::string{});
    // 后增工具默认关闭，不能从是否存在工具数据推断窗口打开状态。
    workspace.m_timingPointsTableOpen =
        j.value("m_timingPointsTableOpen", false);
    workspace.m_annotationTableOpen = j.value("m_annotationTableOpen", false);
    workspace.m_overlapCheckOpen    = j.value("m_overlapCheckOpen", false);
    workspace.m_metadataEditorOpen  = j.value("m_metadataEditorOpen", false);
    workspace.m_noteMetadataEditorOpen =
        j.value("m_noteMetadataEditorOpen", false);
}

/// @brief 序列化项目设置。
/// @param j 接收对象的 JSON 节点。
/// @param settings 待保存的项目覆盖配置和工作区。
/// @note 过滤只作用于序列化副本，调用方的覆盖配置和值对象保持原样。
/// 软件级隐私与工具栏外观不能随着项目文件转移到另一位用户的环境。
void to_json(nlohmann::json& j, const ProjectSettings& settings)
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
    // 视觉与备份覆盖沿用各自配置转换器，只有编辑器覆盖需要字段级过滤。
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
/// @note 继承状态用空 optional 表示，不在这里复制当前软件全局配置。
/// 子对象存在时复用配置层转换器，项目层只处理其拥有范围内的兼容规则。
void from_json(const nlohmann::json& j, ProjectSettings& settings)
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
        // 必须先读取旧项目的编辑器覆盖，再重置其中不属于项目的选项。
        // 不能跳过整个覆盖对象，否则会丢失允许随项目迁移的编辑行为。
        settings.m_editorOverride = it->get<Config::EditorSettings>();
        // 即使旧项目曾保存工具栏外观，也恢复为当前软件全局默认语义。
        Config::preserveGlobalToolbarDisplaySettings(*settings.m_editorOverride,
                                                     Config::EditorSettings{});
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
        // 项目备份覆盖仍由配置层转换器验证，不与软件隐私字段一起清除。
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

}  // namespace MMM
