#pragma once

#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorConfig.h"
#include <glm/glm.hpp>

namespace MMM::Logic::System
{

using TextureID = MMM::Logic::TextureID;

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
        bool currentInAtlas =
            snapshot->uvMap.count(static_cast<uint32_t>(currentTex));
        bool nextInAtlas = snapshot->uvMap.count(static_cast<uint32_t>(tex));

        bool needSplit = false;
        if ( currentCmd.indexCount > 0 ) {
            if ( currentInAtlas && nextInAtlas ) {
                needSplit = false;
            } else if ( currentTex != tex ) {
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

    /// @brief 推送一个矩形 (y 为底边坐标，向上绘制)
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
        // 注意：由于 pushQuad 是底边坐标向上画，这里传入 y+h
        pushQuad(left, top + thickness, w, thickness, color);
        pushQuad(left, bottom, w, thickness, color);
        pushQuad(left, bottom, thickness, h, color);
        pushQuad(right - thickness, bottom, thickness, h, color);
    }

    /// @brief 推送一个矩形，带自定义 UV (y 为底边坐标，向上绘制)
    void pushUVQuad(float x, float y, float w, float h, glm::vec2 uvMin,
                    glm::vec2 uvMax, glm::vec4 color)
    {
        uint32_t baseIndex = static_cast<uint32_t>(snapshot->vertices.size());

        Graphic::Vertex::VKBasicVertex v1, v2, v3, v4;
        // p1: 左下, p2: 右下, p3: 右上, p4: 左上
        v1.pos = { x, y, 0.0f };
        v2.pos = { x + w, y, 0.0f };
        v3.pos = { x + w, y - h, 0.0f };
        v4.pos = { x, y - h, 0.0f };

        // 对应 UV: v1(左下)->uv(minX, maxY), v3(右上)->uv(maxX, minY)
        v1.uv = { uvMin.x, uvMax.y };
        v2.uv = { uvMax.x, uvMax.y };
        v3.uv = { uvMax.x, uvMin.y };
        v4.uv = { uvMin.x, uvMin.y };

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

    /// @brief 推送一个矩形，根据填充模式自动计算 UV
    void pushFilledQuad(float x, float y, float w, float h, glm::vec2 texSize,
                        BackgroundFillMode fillMode, glm::vec4 color)
    {
        if ( texSize.x <= 0 || texSize.y <= 0 ) {
            pushQuad(x, y, w, h, color);
            return;
        }

        float     viewAspect = w / h;
        float     texAspect  = texSize.x / texSize.y;
        glm::vec2 uvMin(0.0f, 0.0f);
        glm::vec2 uvMax(1.0f, 1.0f);
        float     drawX = x, drawY = y, drawW = w, drawH = h;

        switch ( fillMode ) {
        case BackgroundFillMode::Stretch: break;
        case BackgroundFillMode::AspectFit:
            if ( texAspect > viewAspect ) {
                drawH = w / texAspect;
                drawY -= (h - drawH) * 0.5f;  // 向上偏移调整
            } else {
                drawW = h * texAspect;
                drawX += (w - drawW) * 0.5f;
            }
            break;
        case BackgroundFillMode::AspectFill:
            if ( texAspect > viewAspect ) {
                float showW = viewAspect / texAspect;
                uvMin.x     = (1.0f - showW) * 0.5f;
                uvMax.x     = uvMin.x + showW;
            } else {
                float showH = texAspect / viewAspect;
                uvMin.y     = (1.0f - showH) * 0.5f;
                uvMax.y     = uvMin.y + showH;
            }
            break;
        case BackgroundFillMode::Center:
            drawW = texSize.x;
            drawH = texSize.y;
            drawX += (w - drawW) * 0.5f;
            drawY -= (h - drawH) * 0.5f;
            break;
        }

        auto it = snapshot->uvMap.find(static_cast<uint32_t>(currentTex));
        if ( it != snapshot->uvMap.end() ) {
            float u  = it->second.x;
            float v  = it->second.y;
            float tw = it->second.z;
            float th = it->second.w;

            glm::vec2 finalUvMin(u + uvMin.x * tw, v + uvMin.y * th);
            glm::vec2 finalUvMax(u + uvMax.x * tw, v + uvMax.y * th);

            pushUVQuad(
                drawX, drawY, drawW, drawH, finalUvMin, finalUvMax, color);
        } else {
            pushUVQuad(drawX, drawY, drawW, drawH, uvMin, uvMax, color);
        }
    }

    void pushFreeQuad(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4,
                      glm::vec4 color)
    {
        uint32_t baseIndex = static_cast<uint32_t>(snapshot->vertices.size());

        Graphic::Vertex::VKBasicVertex v1, v2, v3, v4;
        v1.pos = { p1.x, p1.y, 0.0f };
        v2.pos = { p2.x, p2.y, 0.0f };
        v3.pos = { p3.x, p3.y, 0.0f };
        v4.pos = { p4.x, p4.y, 0.0f };

        glm::vec2 uvMin(0.0f, 0.0f);
        glm::vec2 uvMax(1.0f, 1.0f);

        auto it = snapshot->uvMap.find(static_cast<uint32_t>(currentTex));
        if ( it != snapshot->uvMap.end() ) {
            float u = it->second.x;
            float v = it->second.y;
            float w = it->second.z;
            float h = it->second.w;

            const float texSize    = 2048.0f;
            const float halfPixelU = 0.5f / texSize;
            const float halfPixelV = 0.5f / texSize;

            uvMin = glm::vec2(u + halfPixelU, v + halfPixelV);
            uvMax = glm::vec2(u + w - halfPixelU, v + h - halfPixelV);
        }

        v1.uv = { uvMin.x, uvMax.y };
        v2.uv = { uvMax.x, uvMax.y };
        v3.uv = { uvMax.x, uvMin.y };
        v4.uv = { uvMin.x, uvMin.y };

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
