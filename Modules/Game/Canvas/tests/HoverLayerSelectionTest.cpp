#include "canvas/HoverLayerSelection.h"

#include <cstdint>
#include <vector>

namespace
{

/// @brief 验证同一自动采样的主体与偏移句柄只形成一个悬浮层。
/// @return 顶层偏移句柄被保留且主体未重复追加时返回 true。
bool testSameSampleHitboxesShareOneLayer()
{
    std::vector<MMM::Canvas::HoverLayerCandidate> candidates;
    const auto entity = static_cast<entt::entity>(7);
    if ( !MMM::Canvas::appendHoverLayerCandidate(
             candidates,
             { entity,
               MMM::Logic::ChartObjectKind::AudioSample,
               std::uint8_t{ 9 },
               -1 }) ||
         MMM::Canvas::appendHoverLayerCandidate(
             candidates,
             { entity,
               MMM::Logic::ChartObjectKind::AudioSample,
               std::uint8_t{ 8 },
               -1 }) ) {
        return false;
    }
    return candidates.size() == 1 && candidates.front().part == 9;
}

/// @brief 验证不同注册表、不同实体和玩家物件部位仍保持独立悬浮层。
/// @return 相同数值 ID 的玩家物件、其另一部位和另一自动采样均可追加时返回
/// true。
bool testDistinctObjectsRemainSeparateLayers()
{
    std::vector<MMM::Canvas::HoverLayerCandidate> candidates;
    const auto entity = static_cast<entt::entity>(11);
    return MMM::Canvas::appendHoverLayerCandidate(
               candidates,
               { entity,
                 MMM::Logic::ChartObjectKind::AudioSample,
                 std::uint8_t{ 1 },
                 -1 }) &&
           MMM::Canvas::appendHoverLayerCandidate(
               candidates,
               { entity,
                 MMM::Logic::ChartObjectKind::PlayerNote,
                 std::uint8_t{ 1 },
                 -1 }) &&
           MMM::Canvas::appendHoverLayerCandidate(
               candidates,
               { entity,
                 MMM::Logic::ChartObjectKind::PlayerNote,
                 std::uint8_t{ 2 },
                 -1 }) &&
           MMM::Canvas::appendHoverLayerCandidate(
               candidates,
               { static_cast<entt::entity>(12),
                 MMM::Logic::ChartObjectKind::AudioSample,
                 std::uint8_t{ 1 },
                 -1 }) &&
           candidates.size() == 4;
}

}  // namespace

/// @brief 覆盖主画布悬浮层按物件去重规则。
/// @return 全部断言通过时返回 0。
int main()
{
    return testSameSampleHitboxesShareOneLayer() &&
                   testDistinctObjectsRemainSeparateLayers()
               ? 0
               : 1;
}
