#pragma once
namespace MMM::UI
{
/// @brief 绘制客户端音频预设入口及其编辑弹层。
/// @warning 每帧 UI 入口；仅用户操作边沿应用音频图和保存配置。
/// @details 入口不依赖活动项目，预设归当前客户端所有。
void drawAudioPresetControls();
}  // namespace MMM::UI
