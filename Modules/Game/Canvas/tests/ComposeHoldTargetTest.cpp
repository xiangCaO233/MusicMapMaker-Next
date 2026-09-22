#include "canvas/ComposeHoldTarget.h"
#include <array>

/// @brief 验证长条始终邻近上一单键、异轨且首尾取自真实可见拍线。
/// @return 任一时间、轨道或不可用边界违反契约时返回非零。
/// @note 故意打乱拍线时间顺序，覆盖反向滚动和可见区间分段后的候选集合。
/// @note 时间采用非零偏移及三分拍，避免零起点四分拍掩盖重新量化的错误。
int main()
{
    using MMM::Canvas::chooseComposeHoldTarget;
    using Line = MMM::Common::Render::PlayerBeatLineSnapshot;
    // 四条线同时提供前驱、同拍和两个后继，断言能区分最近与任意后继。
    // 非整数秒直接从输入返回，测试比较精确值以发现额外量化或四舍五入。
    const std::array lines{
        Line{ 1.337 + 1.0 / 3.0, 200.0F },
        Line{ 1.337 - 1.0 / 3.0, 400.0F },
        Line{ 1.337, 300.0F },
        Line{ 1.337 + 2.0 / 3.0, 100.0F },
    };
    // 全部轨数及随机种子都应排除原轨；首尾保持在最近的两条真实拍线上。
    // 起点固定与单键同拍，不因随机种子改变附近时间的定义。
    for ( int count = 2; count <= 8; ++count ) {
        for ( int track = 0; track < count; ++track ) {
            // 覆盖原轨位于首、中、末位置，防止非零偏移取模后绕回原轨。
            for ( unsigned seed = 0; seed < 16; ++seed ) {
                const auto target = chooseComposeHoldTarget(
                    lines, 1.337, track, count, 20.0F, 0.0F, 500.0F, seed);
                if ( !target || target->track == track || target->track < 0 ||
                     target->track >= count ||
                     target->startTime != lines[2].time ||
                     target->endTime != lines[0].time )
                    return 1;
            }
        }
    }
    // 原拍位位于视口上部时，选前一拍作为头部、原拍位作为尾部。
    // 这要求时间保持递增，不能简单交换屏幕坐标后制造负时长。
    const auto backward =
        chooseComposeHoldTarget(lines, 1.337, 1, 4, 20.0F, 250.0F, 500.0F, 3);
    if ( !backward || backward->startTime != lines[1].time ||
         backward->endTime != lines[2].time )
        return 2;
    // 反向滚动下时间增长对应 Y 增长；选择仍遵循时间，路径自然向下。
    auto reversed = lines;
    // 只反转屏幕位置，保留时间和输入顺序，隔离坐标方向对算法的影响。
    for ( auto& line : reversed ) line.y = 500.0F - line.y;
    const auto reverse =
        chooseComposeHoldTarget(reversed, 1.337, 0, 4, 20.0F, 0.0F, 500.0F, 0);
    if ( !reverse || reverse->startTime != lines[2].time ||
         reverse->endTime != lines[0].time )
        return 3;
    // 单轨、缺失原拍位、端点被裁剪或没有足够拖动空间，都不能凭空造目标。
    // 特别是缺失原拍位时，不能把视野中无关位置当作单键附近。
    // 裁剪用例的中心正好在边界上，只有完整尺寸检查才会拒绝它。
    // 大尺寸用例仍有可见中心，但端点之间的间隔不足以展示拖动路径。
    if ( chooseComposeHoldTarget(lines, 1.337, 0, 1, 20, 0, 500, 0) ||
         chooseComposeHoldTarget(lines, 8.0, 0, 4, 20, 0, 500, 0) ||
         chooseComposeHoldTarget(lines, 1.337, 0, 4, 20, 300, 500, 0) ||
         chooseComposeHoldTarget(lines, 1.337, 0, 4, 150, 0, 500, 0) )
        return 4;
    // 高频场景只借用输入 span；不要求渲染设备、谱面实体或磁盘测试资源。
    // 通过生产 helper 验证几何约束，鼠标与命令链由画布集成测试另行覆盖。
    return 0;
}
