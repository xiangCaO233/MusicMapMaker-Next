#include "logic/ecs/system/SampleRenderSystem.h"

#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{

/// @brief 使用小容差比较纹理坐标。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个坐标足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 验证自动采样本体与玩家 Tap 使用相同纹理和尺寸公式。
/// @return 采样本体的图集 UV、宽高与玩家 Tap 一致时返回 true。
bool testSampleBodyMatchesTapTextureAndSize()
{
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig       config;
    MMM::Logic::System::ScrollCache cache;
    cache.rebuild(timelineRegistry, config, nullptr);

    entt::registry sampleRegistry;
    const auto     sampleEntity = sampleRegistry.create();
    sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 0.0,
            .m_track           = 4,
            .m_audioResourceId = "sample.wav",
        });
    const std::vector<entt::entity> sortedEntities{ sampleEntity };
    const std::vector<double>       maxEndPrefix{ 0.0 };

    MMM::Logic::RenderSnapshot snapshot;
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    const glm::vec4 noteUv{ 0.25F, 0.35F, 0.2F, 0.1F };
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note), noteUv);

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        800.0F, 4, 1, 0.1F, 0.5F, 0.0F);
    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::SampleRenderSystem::renderSamples(sampleRegistry,
                                                          sortedEntities,
                                                          maxEndPrefix,
                                                          &snapshot,
                                                          batcher,
                                                          projection,
                                                          &cache,
                                                          config,
                                                          0.0,
                                                          300.0F,
                                                          800.0F,
                                                          0.0F,
                                                          600.0F,
                                                          1.0F);
    batcher.flush();

    if ( snapshot.vertices.size() != 4U || snapshot.indices.size() != 6U ) {
        XERROR("Zero-offset sample rendered decorations over its Note texture");
        return false;
    }

    bool  foundNoteTopLeft     = false;
    bool  foundNoteBottomRight = false;
    float minX                 = snapshot.vertices.front().pos.x;
    float maxX                 = minX;
    float minY                 = snapshot.vertices.front().pos.y;
    float maxY                 = minY;
    for ( const auto& vertex : snapshot.vertices ) {
        foundNoteTopLeft = foundNoteTopLeft || (near(vertex.uv.u, noteUv.x) &&
                                                near(vertex.uv.v, noteUv.y));
        foundNoteBottomRight =
            foundNoteBottomRight || (near(vertex.uv.u, noteUv.x + noteUv.z) &&
                                     near(vertex.uv.v, noteUv.y + noteUv.w));
        minX = std::min(minX, vertex.pos.x);
        maxX = std::max(maxX, vertex.pos.x);
        minY = std::min(minY, vertex.pos.y);
        maxY = std::max(maxY, vertex.pos.y);
    }
    if ( !foundNoteTopLeft || !foundNoteBottomRight ) {
        XERROR("Sample body did not use the configured Note texture UV");
        return false;
    }

    const float laneWidth     = projection.player.singleTrackWidth;
    const float expectedWidth = laneWidth * config.visual.noteScaleX;
    const float expectedHeight =
        laneWidth / (noteUv.z / noteUv.w) * config.visual.noteScaleY;
    if ( !near(maxX - minX, expectedWidth) ||
         !near(maxY - minY, expectedHeight) ) {
        XERROR("Sample body size diverged from the player Tap size");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行自动采样渲染系统回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testSampleBodyMatchesTapTextureAndSize() ? 0 : 1;
}
