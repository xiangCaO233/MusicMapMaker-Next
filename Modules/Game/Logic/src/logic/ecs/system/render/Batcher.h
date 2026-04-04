#pragma once

#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorConfig.h"
#include <glm/glm.hpp>

namespace MMM::Logic::System
{

enum class TextureID : uint32_t {
    None                = 0,
    Note                = 1,
    HoldHead            = 2,
    HoldBodyVertical    = 3,
    HoldEnd             = 4,
    FlickArrowLeft      = 5,
    FlickArrowRight     = 6,
    FlickArrowUp        = 7,
    FlickArrowDown      = 8,
    NoteSelectionBorder = 100
};

// 内部批处理器，负责根据 TextureID 自动切分 DrawCall
struct Batcher {
    RenderSnapshot*  snapshot;
    TextureID        currentTex = TextureID::None;
    UI::BrushDrawCmd currentCmd;

    Batcher(RenderSnapshot* s) : snapshot(s)
    {
        currentCmd.indexOffset     = 0;
        currentCmd.vertexOffset    = 0;
        currentCmd.indexCount      = 0;
        currentCmd.texture         = VK_NULL_HANDLE;
        currentCmd.customTextureId = 0;
    }

    void setTexture(TextureID tex)
    {
        // 如果当前纹理 ID
        // 发生了变化，且其中之一不在图集中（或者我们还没实现全图集化），则需要切分
        // 但为了简单起见，我们先假设：只要 TextureID
        // 变了，且不是都属于同一个图集逻辑，就切分。
        // 实际上，如果我们要实现真正的“一图集一
        // DrawCall”，这里应该判断是否属于同一个 DescriptorSet。
        // 由于逻辑层不知道 DescriptorSet，我们约定：
        // 所有非 None 的 TextureID 如果在 uvMap
        // 中有记录，则它们共享同一个大图集，不切分。

        bool currentInAtlas =
            snapshot->uvMap.count(static_cast<uint32_t>(currentTex));
        bool nextInAtlas = snapshot->uvMap.count(static_cast<uint32_t>(tex));

        bool needSplit = false;
        if ( currentCmd.indexCount > 0 ) {
            if ( currentInAtlas && nextInAtlas ) {
                // 都在图集中，不切分
                needSplit = false;
            } else if ( currentTex != tex ) {
                // 至少有一个不在图集，且 ID 变了，切分
                needSplit = true;
            }
        }

        if ( needSplit ) {
            snapshot->cmds.push_back(currentCmd);
            currentCmd.indexCount   = 0;
            currentCmd.indexOffset  = snapshot->indices.size();
            currentCmd.vertexOffset = 0;
        }

        if ( currentCmd.indexCount == 0 ) {
            currentCmd.customTextureId = static_cast<uint32_t>(tex);
        }
        currentTex = tex;
    }

    /// @brief 推送一个矩形，如果是带纹理的，颜色固定为白色
    void pushQuad(float x, float y, float w, float h, glm::vec4 color)
    {
        pushFreeQuad(
            { x, y }, { x + w, y }, { x + w, y - h }, { x, y - h }, color);
    }

    /// @brief 推送一个空心边框
    void pushStrokeRect(float left, float top, float right, float bottom,
                        float thickness, glm::vec4 color)
    {
        float w = right - left;
        float h = bottom - top;
        // 顶
        pushQuad(left, top + thickness, w, thickness, color);
        // 底
        pushQuad(left, bottom, w, thickness, color);
        // 左
        pushQuad(left, bottom, thickness, h, color);
        // 右
        pushQuad(right - thickness, bottom, thickness, h, color);
    }

    void pushFreeQuad(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4,
                      glm::vec4 color)
    {
        uint32_t baseIndex = snapshot->vertices.size();

        Graphic::Vertex::VKBasicVertex v1, v2, v3, v4;
        v1.pos = { p1.x, p1.y, 0.0f };
        v2.pos = { p2.x, p2.y, 0.0f };
        v3.pos = { p3.x, p3.y, 0.0f };
        v4.pos = { p4.x, p4.y, 0.0f };

        // 默认 UV
        glm::vec2 uvMin(0.0f, 0.0f);
        glm::vec2 uvMax(1.0f, 1.0f);

        // 如果在图集中，查找实际 UV
        auto it = snapshot->uvMap.find(static_cast<uint32_t>(currentTex));
        if ( it != snapshot->uvMap.end() ) {
            // it->second = (u, v, w, h)
            uvMin = glm::vec2(it->second.x, it->second.y);
            uvMax = uvMin + glm::vec2(it->second.z, it->second.w);
        }



        // Vulkan UV: (0,0) 左上, (1,1) 右下
        // v1: 左上 pos -> (uMin, vMin) uv
        // v2: 右上 pos -> (uMax, vMin) uv
        // v3: 右下 pos -> (uMax, vMax) uv
        // v4: 左下 pos -> (uMin, vMax) uv
        v1.uv = { uvMin.x, uvMin.y };
        v2.uv = { uvMax.x, uvMin.y };
        v3.uv = { uvMax.x, uvMax.y };
        v4.uv = { uvMin.x, uvMax.y };

        v1.color = v2.color = v3.color =
            v4.color        = { color.r, color.g, color.b, color.a };

        snapshot->vertices.push_back(v1);
        snapshot->vertices.push_back(v2);
        snapshot->vertices.push_back(v3);
        snapshot->vertices.push_back(v4);

        snapshot->indices.push_back(baseIndex + 0);
        snapshot->indices.push_back(baseIndex + 1);
        snapshot->indices.push_back(baseIndex + 2);
        snapshot->indices.push_back(baseIndex + 2);
        snapshot->indices.push_back(baseIndex + 3);
        snapshot->indices.push_back(baseIndex + 0);

        currentCmd.indexCount += 6;
    }

    void flush()
    {
        if ( currentCmd.indexCount > 0 ) {
            snapshot->cmds.push_back(currentCmd);
            currentCmd.indexCount = 0;
        }
    }
};

}  // namespace MMM::Logic::System
