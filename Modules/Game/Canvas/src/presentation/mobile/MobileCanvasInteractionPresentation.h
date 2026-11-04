#pragma once
#include <memory>
namespace MMM::Canvas
{
class CanvasInteractionPresentation;
/// @brief 移动实现私有入口，生产安卓工厂与无 GPU 回归测试共用。
/// 调用方在完整中立基类可见的实现文件中销毁返回对象。
/// 平台头不包含画布控制器、UI 管理器或手势布局的定义。
/// @details 公共画布头只认识中立基类，不传播此头或触摸状态定义。
std::unique_ptr<CanvasInteractionPresentation>
createMobileCanvasInteractionPresentation();
}  // namespace MMM::Canvas
