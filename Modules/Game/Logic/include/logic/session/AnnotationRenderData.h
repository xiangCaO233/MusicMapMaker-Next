#pragma once

#include "common/render/AnnotationRenderData.h"

namespace MMM::Logic
{
/// @brief 复用公共标注渲染条目，逻辑层不另行定义同构数据。
using Common::Render::AnnotationRenderItem;
/// @brief 复用公共标记类型，使会话与渲染消费者共享同一数据契约。
using Common::Render::AnnotationRenderMarker;
// 保留逻辑命名空间的使用入口，实际类型定义归属 Common::Render。
}  // namespace MMM::Logic
