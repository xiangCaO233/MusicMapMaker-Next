#pragma once
#include "ui/workspace/TouchReadPress.h"
namespace MMM::Canvas
{
/// @brief 画布批注与 UI 资源行共用同一个轻点／长按判定契约。
/// @details 实现归属 UI 的轻量工作区输入层，不建立 UI 到 Canvas 的依赖。
/// 兼容名称保留旧调用点，状态仍按值归视图独占。
using MobileReadPress = UI::TouchReadPress;
}  // namespace MMM::Canvas
