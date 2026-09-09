#pragma once

#include "common/render/RenderSnapshot.h"
#include "common/render/RenderSnapshotBuffer.h"

namespace MMM::Logic
{

/// @brief 将公共渲染快照类型导入逻辑层，保持既有调用点的命名空间兼容。
/// 类型本身仍由 Common::Render 定义，此处不创建第二套数据或同步协议。
/// 新增共享字段时应修改公共定义，避免逻辑与画布分别维护不一致的副本。
/// @brief 批注正文及定位标记，与完整谱面会话对象解耦。
using Common::Render::AnnotationRenderItem;
using Common::Render::AnnotationRenderMarker;
/// @brief 组件实例的渲染值快照，不保留编辑器对象的所有权。
using Common::Render::CanvasComponentInstanceSnapshot;
/// @brief 拾取区域与悬停结果，供渲染与逻辑交互使用同一坐标约定。
using Common::Render::Hitbox;
using Common::Render::HoverBeatPoint;
using Common::Render::HoverInspectInfo;
using Common::Render::HoverInspectKind;
using Common::Render::HoverPart;
using Common::Render::HoverSubdivisionPreview;
/// @brief 单次发布的画布渲染数据，具体存储契约见公共快照定义。
using Common::Render::RenderSnapshot;
/// @brief 纹理索引及时间线拾取元素，避免逻辑层依赖图形后端句柄。
using Common::Render::TextureID;
using Common::Render::TimelineInteractiveElement;
/// @brief 复用公共字形标识及命中区域缩放函数，保持生产和消费侧一致。
using Common::Render::asciiGlyphTextureId;
using Common::Render::scaleInteractionHitbox;
using Common::Render::unicodeGlyphTextureId;
/// @brief 兼容旧缓冲类型名，所有读写与发布操作由公共实现承担。
using BeatmapSyncBuffer = Common::Render::RenderSnapshotBuffer;

}  // namespace MMM::Logic
