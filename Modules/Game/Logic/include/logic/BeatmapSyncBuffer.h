#pragma once

#include "common/render/RenderSnapshot.h"
#include "common/render/RenderSnapshotBuffer.h"

namespace MMM::Logic
{

using Common::Render::AnnotationRenderItem;
using Common::Render::AnnotationRenderMarker;
using Common::Render::CanvasComponentInstanceSnapshot;
using Common::Render::Hitbox;
using Common::Render::HoverBeatPoint;
using Common::Render::HoverInspectInfo;
using Common::Render::HoverInspectKind;
using Common::Render::HoverPart;
using Common::Render::HoverSubdivisionPreview;
using Common::Render::RenderSnapshot;
using Common::Render::TextureID;
using Common::Render::TimelineInteractiveElement;
using Common::Render::asciiGlyphTextureId;
using Common::Render::scaleInteractionHitbox;
using Common::Render::unicodeGlyphTextureId;
using BeatmapSyncBuffer = Common::Render::RenderSnapshotBuffer;

}  // namespace MMM::Logic
