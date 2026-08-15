#include "canvas/HoverLayerSelection.h"

#include <cmath>
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

/// @brief 使用小容差比较悬浮提示框坐标。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个坐标足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 验证极小物件的悬浮提示框扩大到最低可见尺寸且中心不漂移。
/// @return 提示框保持中心并达到 32 像素时返回 true。
bool testSmallObjectHoverHintUsesMinimumExtent()
{
    const MMM::Canvas::HoverLayerCandidate candidate{
        .x      = 100.0F,
        .y      = 40.0F,
        .width  = 8.0F,
        .height = 12.0F,
    };
    const auto bounds = MMM::Canvas::calculateHoverHintBounds(candidate);
    return near(bounds.left, 88.0F) && near(bounds.top, 30.0F) &&
           near(bounds.right, 120.0F) && near(bounds.bottom, 62.0F);
}

/// @brief 验证普通物件的悬浮提示框只追加固定留白。
/// @return 四周各扩展 5 像素时返回 true。
bool testLargeObjectHoverHintUsesPadding()
{
    const MMM::Canvas::HoverLayerCandidate candidate{
        .x      = 10.0F,
        .y      = 20.0F,
        .width  = 50.0F,
        .height = 30.0F,
    };
    const auto bounds = MMM::Canvas::calculateHoverHintBounds(candidate);
    return near(bounds.left, 5.0F) && near(bounds.top, 15.0F) &&
           near(bounds.right, 65.0F) && near(bounds.bottom, 55.0F);
}

}  // namespace

/// @brief 覆盖主画布悬浮层按物件去重规则。
/// @return 全部断言通过时返回 0。
int main()
{
    return testSameSampleHitboxesShareOneLayer() &&
                   testDistinctObjectsRemainSeparateLayers() &&
                   testSmallObjectHoverHintUsesMinimumExtent() &&
                   testLargeObjectHoverHintUsesPadding()
               ? 0
               : 1;
}
