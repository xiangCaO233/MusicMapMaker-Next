#pragma once

#include "imgui.h"

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
};

/// @brief 控制项目内单个音频资源的独立试听池。
/// @param project 当前项目。
/// @param audioResourceId 项目音频资源 ID。
/// @param action 播放、暂停或停止动作。
/// @param volumeFactor 物件自身叠加到资源音量之上的倍率。
/// @return 找到资源且动作已成功提交时返回 true。
/// @warning 低频按钮动作路径：Play 首次触发时可能同步解码文件并执行资源
/// DSP；只能由明确的用户操作调用，禁止放入每帧路径。
[[nodiscard]] bool controlProjectAudioPreview(const Project&   project,
                                              std::string_view audioResourceId,
                                              ProjectAudioPreviewAction action,
                                              float volumeFactor = 1.0F);

/// @brief 绘制项目音频播放、暂停和停止按钮。
/// @param idScope 按钮组稳定 ImGui ID。
/// @param project 当前项目。
/// @param audioResourceId 项目音频资源 ID。
/// @param volumeFactor 物件自身试听音量倍率。
/// @param topLeft 按钮组左上角屏幕坐标。
/// @param buttonSize 单个方形按钮边长。
/// @param spacing 按钮间距。
/// @return 本帧按钮组悬浮和动作状态。
/// @warning UI 热路径：每帧仅提交三个按钮；资源查找和加载只在点击后发生。
[[nodiscard]] ProjectAudioPreviewControlsResult
renderProjectAudioPreviewControls(const char* idScope, const Project& project,
                                  std::string_view audioResourceId,
                                  float volumeFactor, ImVec2 topLeft,
                                  float buttonSize, float spacing);

}  // namespace MMM::UI
