#pragma once

#include "logic/session/tool/IEditTool.h"

namespace MMM::Logic
{

/// @brief 裁剪工具，负责将长条音符(LongNote/Polyline)切割成多个段落。
class CutTool : public IEditTool
{
public:
    // TODO: 实现具体的切割逻辑回调
};

}  // namespace MMM::Logic
