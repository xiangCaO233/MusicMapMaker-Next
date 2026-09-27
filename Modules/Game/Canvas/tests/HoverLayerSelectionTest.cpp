#include "canvas/HoverLayerSelection.h"

#include <cstdint>
#include <vector>

namespace
{

/// @brief 验证同一自动采样的主体与偏移句柄只形成一个悬浮层。
/// @return 顶层偏移句柄被保留且主体未重复追加时返回 true。
///
/// 此用例固定实体与对象种类，仅改变命中部位，从而隔离自动采样专属的
/// 实体级合并规则。
bool testSameSampleHitboxesShareOneLayer()
{
    // 空列表保证第一次追加一定建立该自动采样的顶层候选。
    std::vector<MMM::Canvas::HoverLayerCandidate> candidates;
    const auto entity = static_cast<entt::entity>(7);
    // 使用非空实体值，确保测试不会触发任何空实体的上游过滤约定。
    // part=9 代表先命中的上层句柄，后到的 part=8 不应替换它。
    if ( !MMM::Canvas::appendHoverLayerCandidate(
             candidates,
             // 首项建立 AudioSample 实体 7 的去重身份。
             { entity,
               MMM::Logic::ChartObjectKind::AudioSample,
               std::uint8_t{ 9 },
               -1 }) ||
         MMM::Canvas::appendHoverLayerCandidate(
             candidates,
             // 同身份仅 part 不同，预期返回 false 且不追加。
             { entity,
               MMM::Logic::ChartObjectKind::AudioSample,
               std::uint8_t{ 8 },
               -1 }) ) {
        // 第一次追加失败或第二次重复追加成功都表示去重契约被破坏。
        return false;
    }
    // 不仅数量应为一，还要确认保留的是首个顶层部位。
    return candidates.size() == 1 && candidates.front().part == 9;
}

/// @brief 验证不同注册表、不同实体和玩家物件部位仍保持独立悬浮层。
/// @return 相同数值 ID 的玩家物件、其另一部位和另一自动采样均可追加时返回
/// true。
///
/// 用例刻意复用数值实体 ID，确认 ChartObjectKind 是跨注册表身份的一部分。
bool testDistinctObjectsRemainSeparateLayers()
{
    // 同数值实体 ID 可以存在于不同 ECS 注册表，不能仅按 entity 去重。
    std::vector<MMM::Canvas::HoverLayerCandidate> candidates;
    const auto entity = static_cast<entt::entity>(11);
    // 自动采样、玩家物件两个部位和另一实体应依次形成四个候选层。
    // 最终数量同时验证所有四次 append 都真实写入列表。
    return MMM::Canvas::appendHoverLayerCandidate(
               candidates,
               // 第一项位于 AudioSample 注册表。
               { entity,
                 MMM::Logic::ChartObjectKind::AudioSample,
                 std::uint8_t{ 1 },
                 -1 }) &&
           MMM::Canvas::appendHoverLayerCandidate(
               candidates,
               // 同号实体位于 PlayerNote 注册表，应视为另一对象。
               { entity,
                 MMM::Logic::ChartObjectKind::PlayerNote,
                 std::uint8_t{ 1 },
                 -1 }) &&
           MMM::Canvas::appendHoverLayerCandidate(
               candidates,
               // 玩家物件的另一部位不能套用自动采样去重规则。
               { entity,
                 MMM::Logic::ChartObjectKind::PlayerNote,
                 std::uint8_t{ 2 },
                 -1 }) &&
           MMM::Canvas::appendHoverLayerCandidate(
               candidates,
               // 不同 AudioSample 实体仍需成为独立候选。
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
    // 两项测试分别覆盖需要合并与必须保留的相反规则。
    // 测试只验证候选列表规则，不依赖真实 ECS 注册表。
    return testSameSampleHitboxesShareOneLayer() &&
                   testDistinctObjectsRemainSeparateLayers()
               ? 0
               : 1;
}
