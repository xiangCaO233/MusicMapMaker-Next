#pragma once

#include "config/visual/BackgroundConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include <glm/glm.hpp>

namespace MMM::Logic::System
{

using TextureID          = MMM::Logic::TextureID;
using BackgroundFillMode = MMM::Config::BackgroundFillMode;

/// @brief 向快照追加几何，并按纹理资源与裁剪状态切分绘制命令。
/// @warning 快照生成热路径使用；调用方应复用缓冲容量，不在此加载资源。
/// @note 不持有输出对象所有权；构造后需显式 flush 提交最后一批。
/// @pre 快照由当前调用线程独占写入，多个批处理器不能交错追加未提交批次。
/// @note 命令描述连续索引区间，改变状态只分割命令，不搬移已有顶点。
struct Batcher {
    /// @brief 顶点、索引与图集映射的借用目标，生命周期覆盖本批处理器。
    RenderSnapshot* snapshot;
    /// @brief 命令输出可独立于快照默认命令表，但共享其顶点与索引空间。
    std::vector<Common::Render::CanvasDrawCmd>* targetCmds;
    /// @brief 后续图元使用的逻辑纹理，用于选择对应图集 UV。
    TextureID currentTex = TextureID::None;
    /// @brief 尚未提交的连续索引范围及其绘制状态。
    Common::Render::CanvasDrawCmd currentCmd;

    /// @brief 在现有快照末尾开始追加，不清除调用方已生成的几何。
    /// @param s 非空输出快照，图集映射须在绘制前准备完成。
    /// @param cmds 可选独立命令表；为空时使用快照自身命令表。
    /// @warning 不进行线程同步；快照和可选命令表都必须由调用方保证生命周期。
    Batcher(RenderSnapshot*                             s,
            std::vector<Common::Render::CanvasDrawCmd>* cmds = nullptr)
        : snapshot(s)
    {
        targetCmds             = cmds ? cmds : &s->cmds;
        currentCmd.indexOffset = static_cast<uint32_t>(s->indices.size());
        // 索引直接引用整个快照的顶点编号，不再叠加单批顶点基址。
        currentCmd.vertexOffset    = 0;
        currentCmd.indexCount      = 0;
        currentCmd.customTextureId = 0;
        // 初始裁剪是兼容范围，实际画布应在追加图元前设置所需裁剪区。
        currentCmd.scissor = { 0, 0, 8192U, 8192U };
    }

    /// @brief 设置后续图元的像素裁剪范围，必要时提交当前批次。
    /// @param x 裁剪区左边界，与图元坐标处于同一画布空间。
    /// @param y 裁剪区上边界，不采用 pushQuad 的底边约定。
    /// @param w 裁剪区宽度。
    /// @param h 裁剪区高度。
    /// @pre 输入为有限且可转换为整型的坐标。
    /// @warning 热路径仅转换边界及追加命令，不执行 GPU 同步。
    void setScissor(float x, float y, float w, float h)
    {
        // 向外取整保留边缘像素，负边界截到零后再计算非负宽高。
        int32_t ix = static_cast<int32_t>(std::max(0.0f, std::floor(x)));
        int32_t iy = static_cast<int32_t>(std::max(0.0f, std::floor(y)));
        int32_t ir = static_cast<int32_t>(std::max(0.0f, std::ceil(x + w)));
        int32_t ib = static_cast<int32_t>(std::max(0.0f, std::ceil(y + h)));

        Common::Render::CanvasScissor nextScissor;
        nextScissor.x      = ix;
        nextScissor.y      = iy;
        nextScissor.width  = static_cast<uint32_t>(std::max(0, ir - ix));
        nextScissor.height = static_cast<uint32_t>(std::max(0, ib - iy));

        // 旧图元必须继续使用旧裁剪；相同裁剪或空批次无需产生新命令。
        if ( currentCmd.indexCount > 0 && currentCmd.scissor != nextScissor ) {
            targetCmds->push_back(currentCmd);
            currentCmd.indexCount   = 0;
            currentCmd.indexOffset  = snapshot->indices.size();
            currentCmd.vertexOffset = 0;
        }
        currentCmd.scissor = nextScissor;
        // 空批次也保存新状态，下一次追加的首个图元即可使用它。
    }

    /// @brief 切换逻辑纹理，同一图集中的不同条目允许合并到一条命令。
    /// @param tex 后续图元的纹理标识，不负责验证外部纹理是否已加载。
    /// @warning 逐图元热路径只查询现有映射和更新状态。
    void setTexture(TextureID tex)
    {
        bool currentInAtlas =
            snapshot->uvMap.count(static_cast<uint32_t>(currentTex));
        bool nextInAtlas = snapshot->uvMap.count(static_cast<uint32_t>(tex));

        bool needSplit = false;
        if ( currentCmd.indexCount > 0 ) {
            // 图集条目共享底层纹理，差异已编码到各自顶点的 UV 中。
            if ( currentInAtlas && nextInAtlas ) {
                needSplit = false;
            } else if ( currentTex != tex ) {
                needSplit = true;
            }
        }

        if ( needSplit ) {
            // 先记录旧纹理范围，再从当前索引末尾开始新批次。
            targetCmds->push_back(currentCmd);
            currentCmd.indexCount   = 0;
            currentCmd.indexOffset  = snapshot->indices.size();
            currentCmd.vertexOffset = 0;
        }

        // 非空图集批次保留首个命令标识，currentTex 则继续跟踪新图元的 UV。
        if ( currentCmd.indexCount == 0 ) {
            currentCmd.customTextureId = static_cast<uint32_t>(tex);
        }
        currentTex = tex;
        // 即使无需切批，也必须更新逻辑纹理，否则后续 UV 会沿用旧条目。
    }

    /// @brief 推送一个矩形 (y 为底边坐标，向上绘制)
    /// @param x 矩形左边界。
    /// @param y 矩形底边坐标，高度向负 Y 方向展开。
    /// @param w 水平跨度，由调用方保证其符合所需方向。
    /// @param h 请求高度，低于最小可见高度时向上扩展。
    /// @param color 四个顶点共用的乘色，不在此预乘透明度。
    /// @warning 热路径追加几何，不改变当前纹理或裁剪状态。
    void pushQuad(float x, float y, float w, float h, glm::vec4 color)
    {
        // 最小高度用于保持细小图元可见；宽度在此接口中不作限幅。
        float minH    = 1.5f;
        float actualH = std::max(h, minH);
        pushFreeQuad({ x, y },
                     { x + w, y },
                     { x + w, y - actualH },
                     { x, y - actualH },
                     color);
    }

    /// @brief 推送一个空心边框
    /// @param left 外框左边界。
    /// @param top 外框上边界。
    /// @param right 外框右边界。
    /// @param bottom 外框下边界。
    /// @param thickness 边条厚度，至少按 1.5 个坐标单位生成。
    /// @param color 所有边条使用的同一乘色。
    /// @pre left/right 和 top/bottom 已按几何方向排列。
    /// @warning 热路径用四个矩形组成边框，不请求新的纹理资源。
    void pushStrokeRect(float left, float top, float right, float bottom,
                        float thickness, glm::vec4 color)
    {
        // 采样边界情况：保证最小线宽为 1.5f
        float t = std::max(thickness, 1.5f);
        float w = right - left;
        float h = bottom - top;
        // 上边条的底边位于 top+t，其余边条按各自底边提交。
        pushQuad(left, top + t, w, t, color);
        // 边条在角部相交，透明色也按普通图元顺序混合，不做轮廓布尔运算。
        pushQuad(left, bottom, w, t, color);
        pushQuad(left, bottom, t, h, color);
        pushQuad(right - t, bottom, t, h, color);
    }

    /// @brief 推送一个矩形，带自定义 UV (y 为底边坐标，向上绘制)
    /// @param x 左边界。
    /// @param y 底边坐标。
    /// @param w 直接提交的矩形宽度，本接口不施加最小宽度。
    /// @param h 直接提交的矩形高度，本接口不施加最小高度。
    /// @param uvMin 最终纹理坐标左上界，调用方已完成图集区域换算。
    /// @param uvMax 最终纹理坐标右下界，不再根据 currentTex 二次映射。
    /// @param color 顶点乘色，纹理采样颜色由渲染阶段相乘。
    /// @warning 热路径向共用缓冲追加四个顶点、六个索引。
    void pushUVQuad(float x, float y, float w, float h, glm::vec2 uvMin,
                    glm::vec2 uvMax, glm::vec4 color)
    {
        // 这里只追加几何，调用方必须先用 setTexture 选择实际采样资源。
        // UV 可超出常规范围，本函数不裁剪或自动修正调用方的采样策略。
        if ( currentCmd.indexCount == 0 ) {
            currentCmd.indexOffset =
                static_cast<uint32_t>(snapshot->indices.size());
        }
        // 顶点编号以快照整体为基准，保证跨绘制命令仍指向正确几何。
        uint32_t baseIndex = static_cast<uint32_t>(snapshot->vertices.size());

        Common::Render::CanvasVertex v1, v2, v3, v4;
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

        // 沿 v1 到 v3 的对角线拆成两个三角形，共享角点无需重复顶点。
        snapshot->indices.push_back(baseIndex + 0);
        snapshot->indices.push_back(baseIndex + 1);
        snapshot->indices.push_back(baseIndex + 2);
        snapshot->indices.push_back(baseIndex + 2);
        snapshot->indices.push_back(baseIndex + 3);
        snapshot->indices.push_back(baseIndex + 0);

        // 命令数量按索引计算，不是顶点数，也不是三角形数量。
        currentCmd.indexCount += 6;
    }

    /// @brief 推送一个矩形，根据填充模式自动计算 UV。
    /// @param x 目标绘制框左边界，居中模式可能在此基础上调整。
    /// @param y 目标绘制框底边，纵向居中需向负 Y 方向偏移。
    /// @param w 目标宽度，至少保留一个坐标单位。
    /// @param h 目标高度，至少保留 1.5 个坐标单位。
    /// @param texSize 背景纹理传入原始像素尺寸；音符纹理可传入
    /// `{aspect, 1.0f}` 作为比例基准。
    /// @param fillMode 选择拉伸、完整显示、裁剪填充或居中策略。
    /// @param color 填充图元的统一顶点乘色。
    /// @note 几何尺寸和纹理区域分别调整，裁剪填充不缩小目标绘制框。
    /// @warning 热路径：每次音符、背景或特效几何生成时执行；禁止加入资源加载、
    /// 文件系统访问、阻塞等待或 shared_ptr 所有权复制。
    void pushFilledQuad(float x, float y, float w, float h, glm::vec2 texSize,
                        BackgroundFillMode fillMode, glm::vec4 color)
    {
        float minH    = 1.5f;
        float actualH = std::max(h, minH);
        float actualW = std::max(w, 1.0f);

        // 没有可用纹理比例时退回普通矩形，避免宽高比计算除零。
        if ( texSize.x <= 0 || texSize.y <= 0 ) {
            pushQuad(x, y, actualW, actualH, color);
            return;
        }

        // 音符可只提供宽高比，Center 不应把比例数值当作像素尺寸。
        const bool usesAbsoluteTextureSize =
            currentTex == TextureID::Background || texSize.y > 1.0f;

        float     viewAspect = actualW / actualH;
        float     texAspect  = texSize.x / texSize.y;
        glm::vec2 uvMin(0.0f, 0.0f);
        glm::vec2 uvMax(1.0f, 1.0f);
        float     drawX = x, drawY = y, drawW = actualW, drawH = actualH;

        switch ( fillMode ) {
        // 初始状态即 Stretch：完整纹理覆盖整个目标框，无额外裁剪。
        case BackgroundFillMode::Stretch: break;
        case BackgroundFillMode::AspectFit:
            // 保留完整 UV，通过缩小绘制框的一条轴实现居中留白。
            if ( texAspect > viewAspect ) {
                drawH = actualW / texAspect;
                // 宽图以宽度为约束缩小高度，留白平均分配在上下两侧。
                drawY -= (actualH - drawH) * 0.5f;
            } else {
                drawW = actualH * texAspect;
                drawX += (actualW - drawW) * 0.5f;
            }
            break;
        case BackgroundFillMode::AspectFill:
            // 绘制框保持不变，截取纹理中间的部分以覆盖整个目标区域。
            if ( texAspect > viewAspect ) {
                float showW = viewAspect / texAspect;
                // 横向只显示中心区间，剩余纹理对称裁掉。
                uvMin.x = (1.0f - showW) * 0.5f;
                uvMax.x = uvMin.x + showW;
            } else {
                float showH = texAspect / viewAspect;
                uvMin.y     = (1.0f - showH) * 0.5f;
                uvMax.y     = uvMin.y + showH;
            }
            break;
        case BackgroundFillMode::Center:
            // 居中不等同于裁剪到目标框；超出部分依赖当前 Scissor。
            // 仅真实像素尺寸恢复原图大小；比例输入沿用当前目标框大小。
            if ( usesAbsoluteTextureSize ) {
                drawW = texSize.x;
                drawH = texSize.y;
            }
            drawX += (actualW - drawW) * 0.5f;
            drawY -= (actualH - drawH) * 0.5f;
            break;
        }

        auto it = snapshot->uvMap.find(static_cast<uint32_t>(currentTex));
        if ( it != snapshot->uvMap.end() ) {
            float u  = it->second.x;
            float v  = it->second.y;
            float tw = it->second.z;
            float th = it->second.w;

            // 填充策略输出局部 0～1 UV，再映射进当前纹理的图集子区域。
            glm::vec2 finalUvMin(u + uvMin.x * tw, v + uvMin.y * th);
            glm::vec2 finalUvMax(u + uvMax.x * tw, v + uvMax.y * th);

            pushUVQuad(
                drawX, drawY, drawW, drawH, finalUvMin, finalUvMax, color);
        } else {
            // 未登记到图集的资源使用独立纹理，局部 UV 就是最终 UV。
            pushUVQuad(drawX, drawY, drawW, drawH, uvMin, uvMax, color);
        }
    }

    /// @brief 按四个有序角点追加自由四边形，并自动选择当前纹理的 UV。
    /// @param p1 对应纹理左下角的几何点。
    /// @param p2 对应纹理右下角的几何点。
    /// @param p3 对应纹理右上角的几何点。
    /// @param p4 对应纹理左上角的几何点。
    /// @param color 自由四边形四角共用的颜色，不插入端点渐变。
    /// @pre 角点顺序形成预期三角形，函数不修复自交或退化形状。
    /// @warning 热路径直接写入快照，不做屏幕裁剪或资源加载。
    void pushFreeQuad(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4,
                      glm::vec4 color)
    {
        // 该入口不施加最小高度，斜边和折线连接体可以保持其原始形状。
        // 不重新排序角点，几何方向和纹理方向都由传入顺序决定。
        if ( currentCmd.indexCount == 0 ) {
            currentCmd.indexOffset =
                static_cast<uint32_t>(snapshot->indices.size());
        }
        uint32_t baseIndex = static_cast<uint32_t>(snapshot->vertices.size());

        Common::Render::CanvasVertex v1, v2, v3, v4;
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

            if ( currentTex == TextureID::None ) {
                // 纯色几何只需要采样白色贴图中心，避免线性过滤混到图集透明边缘。
                uvMin = glm::vec2(u + w * 0.5f, v + h * 0.5f);
                uvMax = uvMin;
            } else {
                // 图集边界向内缩半像素，避免线性过滤采到相邻条目。
                // 此换算依赖当前固定图集尺寸，调整图集时需同步维护。
                const float texSize    = 2048.0f;
                const float halfPixelU = 0.5f / texSize;
                const float halfPixelV = 0.5f / texSize;

                uvMin = glm::vec2(u + halfPixelU, v + halfPixelV);
                uvMax = glm::vec2(u + w - halfPixelU, v + h - halfPixelV);
            }
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
    /// @param x 外接矩形左边界。
    /// @param y 外接矩形底边坐标。
    /// @param w 外接矩形宽度。
    /// @param h 外接矩形高度。
    /// @param r 圆角半径，受宽高的一半限制。
    /// @param color 中央矩形、边条和圆角共用的颜色。
    /// @pre 纯色绘制应预先选择 TextureID::None；本接口不自动切换纹理。
    /// @warning 热路径使用固定分段数，不动态细分圆弧。
    void pushRoundedQuad(float x, float y, float w, float h, float r,
                         glm::vec4 color)
    {
        // 小到不可辨识的圆角直接退回矩形，省去固定扇形开销。
        if ( r <= 0.05f ) {
            pushQuad(x, y, w, h, color);
            return;
        }
        r = std::min({ r, std::abs(w) * 0.5f, std::abs(h) * 0.5f });

        // 中央十字区域。
        pushQuad(x + r, y, w - 2 * r, h, color);
        // 左右侧边条。
        pushQuad(x, y - r, r, h - 2 * r, color);
        pushQuad(x + w - r, y - r, r, h - 2 * r, color);

        /// @brief 用共享中心顶点和扇形三角形生成一个圆角。
        auto pushCorner =
            [&](float cx, float cy, float startAng, float endAng) {
                const int segments = 6;
                // 中心编号在写入前固定，后续环点编号均相对该位置连续增长。
                uint32_t centerIdx =
                    static_cast<uint32_t>(snapshot->vertices.size());
                Common::Render::CanvasVertex center;
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
                // 圆角全部采同一个白色像素，颜色由顶点提供，不铺展纹理图案。
                snapshot->vertices.push_back(center);

                // 包含两端圆弧顶点，六个间隔对应六个扇形三角形。
                for ( int i = 0; i <= segments; ++i ) {
                    float ang =
                        startAng + (endAng - startAng) * (float)i / segments;
                    Common::Render::CanvasVertex v;
                    v.pos   = { cx + r * std::cos(ang),
                                cy + r * std::sin(ang),
                                0.0f };
                    v.color = { color.r, color.g, color.b, color.a };
                    v.uv    = { whiteUv.x, whiteUv.y };
                    snapshot->vertices.push_back(v);

                    if ( i > 0 ) {
                        // 首个环点只建立起边，必须有第二个环点才形成三角形。
                        if ( currentCmd.indexCount == 0 ) {
                            currentCmd.indexOffset =
                                static_cast<uint32_t>(snapshot->indices.size());
                        }
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
    /// @param x 外框左边界。
    /// @param y 外框底边坐标。
    /// @param w 外框宽度。
    /// @param h 外框高度。
    /// @param r 外侧圆角半径。
    /// @param thickness 边框厚度，内弧半径按 r-thickness 计算。
    /// @param color 直边与弧线共享的乘色。
    /// @pre 调用方应提供适合边框厚度的尺寸，内部不修复负内弧半径。
    /// @warning 热路径固定生成直边与六段圆弧四边形。
    void pushRoundedStrokeRect(float x, float y, float w, float h, float r,
                               float thickness, glm::vec4 color)
    {
        // 无圆角路径也沿用相同的最小厚度，避免两种外观在阈值处线宽突变。
        float t = std::max(thickness, 1.5f);
        if ( r <= 0.05f ) {
            pushStrokeRect(x, y - h, x + w, y, t, color);
            return;
        }
        r = std::min({ r, std::abs(w) * 0.5f, std::abs(h) * 0.5f });

        // 4 条直边。
        // 直边长度扣去两端半径，给四个圆弧带预留位置。
        pushQuad(x + r, y, w - 2 * r, t, color);
        pushQuad(x + r, y - h + t, w - 2 * r, t, color);
        pushQuad(x, y - r, t, h - 2 * r, color);
        pushQuad(x + w - t, y - r, t, h - 2 * r, color);

        // 4 个圆角弧线。
        // 同一角度下内外两点构成径向边，相邻径向边围成一个弧带片段。
        /// @brief 在内外圆弧间生成四边形带，与直边共同形成边框。
        auto pushArc = [&](float cx, float cy, float startAng, float endAng) {
            const int segments = 6;
            for ( int i = 0; i < segments; ++i ) {
                // 最后一个片段使用 endAng，保证弧带末端与下一条直边接合。
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

    /// @brief 提交末尾非空批次，保留纹理和裁剪状态供后续追加。
    /// @note 不清除顶点、索引或已提交命令，重复空提交不会增加命令。
    /// @warning 这里只写 CPU 命令表，不等同于 GPU 提交或等待。
    void flush()
    {
        // 析构不执行隐式提交，调用方必须在输出命令被消费前调用此入口。
        if ( currentCmd.indexCount > 0 ) {
            // 只在有索引时提交，单纯设置纹理或裁剪不会制造空 DrawCall。
            targetCmds->push_back(currentCmd);
            currentCmd.indexCount = 0;
            currentCmd.indexOffset =
                static_cast<uint32_t>(snapshot->indices.size());
        }
    }
};

}  // namespace MMM::Logic::System
