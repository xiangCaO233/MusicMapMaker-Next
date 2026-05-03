#pragma once

#include "config/EditorConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include <glm/glm.hpp>

namespace MMM::Logic::System
{

using TextureID          = MMM::Logic::TextureID;
using BackgroundFillMode = MMM::Config::BackgroundFillMode;

// 内部批处理器，负责根据 TextureID 自动切分 DrawCall
struct Batcher {
    RenderSnapshot*                snapshot;
    std::vector<UI::BrushDrawCmd>* targetCmds;
    TextureID                      currentTex = TextureID::None;
    UI::BrushDrawCmd               currentCmd;

    Batcher(RenderSnapshot* s, std::vector<UI::BrushDrawCmd>* cmds = nullptr)
        : snapshot(s)
    {
        targetCmds                 = cmds ? cmds : &s->cmds;
        currentCmd.indexOffset     = static_cast<uint32_t>(s->indices.size());
        currentCmd.vertexOffset    = 0;
        currentCmd.indexCount      = 0;
        currentCmd.texture         = VK_NULL_HANDLE;
        currentCmd.customTextureId = 0;
        currentCmd.scissor         = vk::Rect2D{ { 0, 0 }, { 8192, 8192 } };
    }

    void setScissor(float x, float y, float w, float h)
    {
        int32_t ix = static_cast<int32_t>(std::max(0.0f, std::floor(x)));
        int32_t iy = static_cast<int32_t>(std::max(0.0f, std::floor(y)));
        int32_t ir = static_cast<int32_t>(std::max(0.0f, std::ceil(x + w)));
        int32_t ib = static_cast<int32_t>(std::max(0.0f, std::ceil(y + h)));

        vk::Rect2D nextScissor;
        nextScissor.offset.x      = ix;
        nextScissor.offset.y      = iy;
        nextScissor.extent.width  = static_cast<uint32_t>(std::max(0, ir - ix));
        nextScissor.extent.height = static_cast<uint32_t>(std::max(0, ib - iy));

        if ( currentCmd.indexCount > 0 && currentCmd.scissor != nextScissor ) {
            targetCmds->push_back(currentCmd);
            currentCmd.indexCount   = 0;
            currentCmd.indexOffset  = snapshot->indices.size();
            currentCmd.vertexOffset = 0;
        }
        currentCmd.scissor = nextScissor;
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
            targetCmds->push_back(currentCmd);
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
        float minH    = 1.5f;
        float actualH = std::max(h, minH);
        pushFreeQuad({ x, y },
                     { x + w, y },
                     { x + w, y - actualH },
                     { x, y - actualH },
                     color);
    }

    /// @brief 推送一个空心边框
    void pushStrokeRect(float left, float top, float right, float bottom,
                        float thickness, glm::vec4 color)
    {
        // 采样边界情况：保证最小线宽为 1.5f
        float t = std::max(thickness, 1.5f);
        float w = right - left;
        float h = bottom - top;
        // 注意：由于 pushQuad 是底边坐标向上画，这里传入 y+h
        pushQuad(left, top + t, w, t, color);
        pushQuad(left, bottom, w, t, color);
        pushQuad(left, bottom, t, h, color);
        pushQuad(right - t, bottom, t, h, color);
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
        float minH    = 1.5f;
        float actualH = std::max(h, minH);
        float actualW = std::max(w, 1.0f);

        if ( texSize.x <= 0 || texSize.y <= 0 ) {
            pushQuad(x, y, actualW, actualH, color);
            return;
        }

        float     viewAspect = actualW / actualH;
        float     texAspect  = texSize.x / texSize.y;
        glm::vec2 uvMin(0.0f, 0.0f);
        glm::vec2 uvMax(1.0f, 1.0f);
        float     drawX = x, drawY = y, drawW = actualW, drawH = actualH;

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

    /// @brief 推送一个圆角矩形
    void pushRoundedQuad(float x, float y, float w, float h, float r,
                         glm::vec4 color)
    {
        if ( r <= 0.05f ) {
            pushQuad(x, y, w, h, color);
            return;
        }
        r = std::min({ r, std::abs(w) * 0.5f, std::abs(h) * 0.5f });

        // Center cross
        pushQuad(x + r, y, w - 2 * r, h, color);
        // Left & Right bars
        pushQuad(x, y - r, r, h - 2 * r, color);
        pushQuad(x + w - r, y - r, r, h - 2 * r, color);

        auto pushCorner =
            [&](float cx, float cy, float startAng, float endAng) {
                const int segments = 6;
                uint32_t  centerIdx =
                    static_cast<uint32_t>(snapshot->vertices.size());
                Graphic::Vertex::VKBasicVertex center;
                center.pos   = { cx, cy, 0.0f };
                center.color = { color.r, color.g, color.b, color.a };

                // 固定使用 TextureID::None 的 UV (白色像素)
                glm::vec2 whiteUv(0, 0);
                auto      it = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::None));
                if ( it != snapshot->uvMap.end() ) {
                    whiteUv = { it->second.x + it->second.z * 0.5f,
                                it->second.y + it->second.w * 0.5f };
                }
                center.uv = { whiteUv.x, whiteUv.y };
                snapshot->vertices.push_back(center);

                for ( int i = 0; i <= segments; ++i ) {
                    float ang =
                        startAng + (endAng - startAng) * (float)i / segments;
                    Graphic::Vertex::VKBasicVertex v;
                    v.pos   = { cx + r * std::cos(ang),
                                cy + r * std::sin(ang),
                                0.0f };
                    v.color = { color.r, color.g, color.b, color.a };
                    v.uv    = { whiteUv.x, whiteUv.y };
                    snapshot->vertices.push_back(v);

                    if ( i > 0 ) {
                        uint32_t cur  = centerIdx + i + 1;
                        uint32_t prev = cur - 1;
                        snapshot->indices.push_back(centerIdx);
                        snapshot->indices.push_back(prev);
                        snapshot->indices.push_back(cur);
                        currentCmd.indexCount += 3;
                    }
                }
            };

        const float PI = 3.14159265f;
        pushCorner(x + r, y - r, PI, 1.5f * PI);             // Top-Left
        pushCorner(x + w - r, y - r, 1.5f * PI, 2.0f * PI);  // Top-Right
        pushCorner(x + w - r, y - h + r, 0, 0.5f * PI);      // Bottom-Right
        pushCorner(x + r, y - h + r, 0.5f * PI, PI);         // Bottom-Left
    }

    /// @brief 推送一个圆角空心矩形 (y 为底边)
    void pushRoundedStrokeRect(float x, float y, float w, float h, float r,
                               float thickness, glm::vec4 color)
    {
        float t = std::max(thickness, 1.5f);
        if ( r <= 0.05f ) {
            pushStrokeRect(x, y - h, x + w, y, t, color);
            return;
        }
        r = std::min({ r, std::abs(w) * 0.5f, std::abs(h) * 0.5f });

        // 4 Straight edges
        pushQuad(x + r, y, w - 2 * r, t, color);
        pushQuad(x + r, y - h + t, w - 2 * r, t, color);
        pushQuad(x, y - r, t, h - 2 * r, color);
        pushQuad(x + w - t, y - r, t, h - 2 * r, color);

        // 4 Rounded corners (Arc strokes)
        auto pushArc = [&](float cx, float cy, float startAng, float endAng) {
            const int segments = 6;
            for ( int i = 0; i < segments; ++i ) {
                float a1 = startAng + (endAng - startAng) * (float)i / segments;
                float a2 =
                    startAng + (endAng - startAng) * (float)(i + 1) / segments;

                glm::vec2 p1(cx + (r - t) * std::cos(a1),
                             cy + (r - t) * std::sin(a1));
                glm::vec2 p2(cx + r * std::cos(a1), cy + r * std::sin(a1));
                glm::vec2 p3(cx + r * std::cos(a2), cy + r * std::sin(a2));
                glm::vec2 p4(cx + (r - t) * std::cos(a2),
                             cy + (r - t) * std::sin(a2));

                pushFreeQuad(p1, p2, p3, p4, color);
            }
        };

        const float PI = 3.14159265f;
        pushArc(x + r, y - r, PI, 1.5f * PI);             // Top-Left
        pushArc(x + w - r, y - r, 1.5f * PI, 2.0f * PI);  // Top-Right
        pushArc(x + w - r, y - h + r, 0, 0.5f * PI);      // Bottom-Right
        pushArc(x + r, y - h + r, 0.5f * PI, PI);         // Bottom-Left
    }

    void flush()
    {
        if ( currentCmd.indexCount > 0 ) {
            targetCmds->push_back(currentCmd);
            currentCmd.indexCount = 0;
            currentCmd.indexOffset =
                static_cast<uint32_t>(snapshot->indices.size());
        }
    }
};

}  // namespace MMM::Logic::System
