#pragma once

#include "logic/BeatmapSyncBuffer.h"
#include "logic/session/CanvasCamera.h"

#include <entt/entt.hpp>
#include <vector>

namespace MMM::Config
{
struct EditorConfig;
}

namespace MMM::Logic::System
{

struct Batcher;
struct ScrollCache;

/// @brief 主画布 BGM 轨道与自动采样快照生成系统。
class SampleRenderSystem
{
public:
    /// @brief 绘制当前视口可见的 BGM 轨道静态布局。
    /// @param batcher 画布批处理器。
    /// @param projection 玩家区与 BGM 区统一轨道投影。
    /// @param persistentBgmTrackCount 持久化 BGM 轨道数量。
    /// @param viewportWidth 视口宽度。
    /// @param topY 轨道区上边界。
    /// @param bottomY 轨道区下边界。
    /// @warning 主画布快照热路径：只允许遍历当前可见 BGM 轨道。
    static void renderLaneLayout(Batcher&                    batcher,
                                 const CanvasLaneProjection& projection,
                                 std::int32_t persistentBgmTrackCount,
                                 float viewportWidth, float topY,
                                 float bottomY);

    /// @brief 绘制当前视口可见的自动采样并生成拾取盒。
    /// @param registry 自动采样注册表。
    /// @param sortedEntities 按采样覆盖区间起点排序的实体缓存。
    /// @param maxEndPrefix 采样覆盖区间终点的前缀最大值。
    /// @param snapshot 目标渲染快照。
    /// @param batcher 画布批处理器。
    /// @param projection 玩家区与 BGM 区统一轨道投影。
    /// @param cache 已构建的滚动坐标缓存。
    /// @param config 当前物件纹理显示配置。
    /// @param currentTime 当前动画时间。
    /// @param judgmentLineY 判定线纵坐标。
    /// @param viewportWidth 视口宽度。
    /// @param topY 轨道区上边界。
    /// @param bottomY 轨道区下边界。
    /// @param renderScaleY 纵向渲染倍率。
    /// @warning 主画布快照热路径：只能查询预排序采样索引并处理可见物件，
    /// 禁止完整遍历 Registry、排序或访问文件系统。
    static void renderSamples(entt::registry&                  registry,
                              const std::vector<entt::entity>& sortedEntities,
                              const std::vector<double>&       maxEndPrefix,
                              RenderSnapshot* snapshot, Batcher& batcher,
                              const CanvasLaneProjection& projection,
                              const ScrollCache*          cache,
                              const Config::EditorConfig& config,
                              double currentTime, float judgmentLineY,
                              float viewportWidth, float topY, float bottomY,
                              float renderScaleY);
};

}  // namespace MMM::Logic::System
