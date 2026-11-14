#include "graphic/mobile/MobileKeyboard.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
namespace MMM::Graphic
{
namespace
{
/// @brief 高低各 32 位分别保存参考高度与遮挡，避免撕裂两个独立尺寸。
/// @warning 写入者为 Activity 线程，读取者为 SDL UI
/// 线程；仅传值且没有附属数据， relaxed 足够保证完整配对，不需要每帧跨 JNI
/// 或等待平台线程。
std::atomic<std::uint64_t> keyboardGeometry{};
}  // namespace
/// @brief 错误尺寸与关闭状态统一发布无遮挡，不保留上次方向的高度。
/// @param bottom 相对内容区域底边的实际重叠像素，不含系统状态栏。
/// @param height 同一次平台回调测得的内容高度，必须与 bottom 成对。
/// @details 接受旋转过程中的暂时无效值，发布端统一规范化避免读取端猜测。
void publishMobileKeyboardOcclusion(int bottom, int height)
{
    // 屏幕旋转中允许暂时没有内容尺寸；收敛为零而不是除零或沿用旧键盘。
    const auto h = static_cast<std::uint32_t>(std::max(0, height));
    const auto b =
        static_cast<std::uint32_t>(std::clamp(bottom, 0, std::max(0, height)));
    // 该值没有附属对象；不需要以 acquire/release 发布 Java 或窗口生命周期。
    keyboardGeometry.store((std::uint64_t{ h } << 32) | b,
                           std::memory_order_relaxed);
}
/// @brief 比例在 ImGui 逻辑尺寸中应用，不把物理像素直接当作 UI 像素。
/// @warning 高频单次原子读取不与任何可变会话或资源生命周期耦合。
/// @return [0,1] 内的遮挡比例；未初始化或高度为零时返回零。
/// @details 同一比例可映射到不同 DPI 的逻辑工作区，不缓存屏幕方向。
float mobileKeyboardBottomFraction()
{
    // 一个整数携带同一次事件的全部几何，动画中不会混用新高度和旧遮挡。
    const auto packed = keyboardGeometry.load(std::memory_order_relaxed);
    // 发布端已经夹取比例，读取只拆包，不触发系统调用或分配临时资源。
    const auto h = static_cast<std::uint32_t>(packed >> 32);
    return h ? static_cast<float>(static_cast<std::uint32_t>(packed)) / h
             : 0.0f;
}
}  // namespace MMM::Graphic
