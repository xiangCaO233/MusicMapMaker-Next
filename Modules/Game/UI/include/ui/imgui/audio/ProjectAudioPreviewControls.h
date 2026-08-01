#pragma once

#include "imgui.h"

#include <string>
#include <string_view>

namespace MMM
{
class Project;
}

namespace MMM::UI
{

/// @brief 项目音频单资源试听动作。
enum class ProjectAudioPreviewAction {
    Play,
    Pause,
    Stop,
};

/// @brief 单资源试听按钮组本帧交互结果。
struct ProjectAudioPreviewControlsResult {
    /// @brief 鼠标是否悬浮或正在操作任一按钮。
    bool hovered{ false };

    /// @brief 本帧是否触发了任一试听动作。
    bool activated{ false };

    /// @brief 音量弹窗本帧是否保持打开。
    bool volumeEditorOpen{ false };

    /// @brief 音量值是否已经由输入框提交或由 25% 步长按钮修改。
    bool volumeChanged{ false };
};

/// @brief 单资源试听控制条的绝对屏幕布局。
struct ProjectAudioPreviewControlsLayout {
    /// @brief 进度条左上角屏幕坐标。
    ImVec2 topLeft;

    /// @brief 进度条与整组控制按钮的总宽度。
    float width{ 0.0F };

    /// @brief 单个方形按钮边长。
    float buttonSize{ 0.0F };

    /// @brief 相邻按钮的水平间距。
    float buttonSpacing{ 0.0F };

    /// @brief 进度条高度。
    float progressHeight{ 0.0F };

    /// @brief 进度条与按钮行之间的垂直间距。
    float progressSpacing{ 0.0F };
};

/// @brief 构造不会与项目资源、皮肤音效或 HitEffect 冲突的试听池标识。
/// @param previewInstanceId 独立试听实例 ID。
/// @return 可缓存并传给试听控制与进度查询的 AudioManager 池标识。
[[nodiscard]] std::string makeProjectAudioPreviewPoolKey(
    std::string_view previewInstanceId);

/// @brief 控制项目内单个音频资源的独立试听池。
/// @param project 当前项目。
/// @param audioResourceId 项目音频资源 ID。
/// @param previewPoolKey 由 makeProjectAudioPreviewPoolKey 构造的独立池标识。
/// @param action 播放、暂停或停止动作。
/// @param volumeFactor 物件自身叠加到资源音量之上的倍率。
/// @return 找到资源且动作已成功提交时返回 true。
/// @warning 低频按钮动作路径：Play 首次触发时可能同步解码文件并执行资源
/// DSP；只能由明确的用户操作调用，禁止放入每帧路径。
[[nodiscard]] bool controlProjectAudioPreview(const Project&   project,
                                              std::string_view audioResourceId,
                                              const std::string& previewPoolKey,
                                              ProjectAudioPreviewAction action,
                                              float volumeFactor = 1.0F);

/// @brief 绘制项目音频播放、暂停、停止和音量按钮。
/// @param idScope 按钮组稳定 ImGui ID。
/// @param project 当前项目。
/// @param audioResourceId 项目音频资源 ID。
/// @param previewPoolKey 由 makeProjectAudioPreviewPoolKey 构造的独立池标识。
/// @param volumeFactor 物件自身试听音量倍率。
/// @param editableVolume 非空时显示音量按钮，并直接编辑指向的倍率。
/// @param layout 进度条与按钮行的绝对屏幕布局。
/// @param acceptExplicitPointerHit 调用方已确认控件位于当前最上层对象时，
/// 允许绕过 ImGui 窗口悬浮推断并直接使用控件矩形命中。
/// @return 本帧按钮组悬浮和动作状态。
/// @warning UI 热路径：每帧仅查询已缓存试听池并提交一个进度条和三个绝对
/// 定位按钮，不分配池标识；音量弹窗仅在用户明确打开后提交控件。为遵守
/// ImGui 边界约束，返回后游标停留在最后
/// 一个按钮之后，调用方不得依赖原流式布局游标。资源查找和加载只在点击后
/// 发生。
[[nodiscard]] ProjectAudioPreviewControlsResult
renderProjectAudioPreviewControls(
    const char* idScope, const Project& project,
    std::string_view audioResourceId, const std::string& previewPoolKey,
    float volumeFactor, float* editableVolume,
    const ProjectAudioPreviewControlsLayout& layout,
    bool                                     acceptExplicitPointerHit = false);

}  // namespace MMM::UI
