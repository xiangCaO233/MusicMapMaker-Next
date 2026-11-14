#pragma once
#include <cstdint>
struct ImVec2;
namespace MMM::UI
{
/// @brief 只读资源行的轻点／静止长按分类，不修改合成鼠标输入。
/// @details 保存固定大小值状态，不持有行引用、窗口指针或资源所有权。
class TouchReadPress
{
public:
    /// @brief 所有结果均只交付一次，滑动、多指与取消不产生点击。
    enum class Result { None, Tap, Hold };
    /// @brief 为可见稳定目标采样；隐藏或几何改变必须以 eligible 拒绝旧目标。
    /// @warning 每帧固定标量运算，不分配、不等待或访问文件。
    Result update(std::uint32_t id, bool originHit, bool eligible);
    /// @brief 隐藏、切项目或切操作方案时吞掉当前代际。
    void cancel();
    /// @brief 使用归一化原始 Down 判断矩形，不能用滞后的合成鼠标落点。
    static bool originInside(ImVec2 min, ImVec2 max);

private:
    /// @brief 输入代际与稳定控件身份独立于列表排序位置。
    std::uint64_t m_epoch{};
    std::uint32_t m_target{};
    /// @brief UI 单调时钟只用于非阻塞长按判定。
    double m_started{};
    /// @brief 起点只能在首次观察代际时认领，缺帧后不能续接。
    int m_captureFrame{ -1 };
    int m_frame{};
    /// @brief 取消与交付都是单向门闩，新的 Down 才能恢复。
    bool m_blocked{}, m_sent{};
};
}  // namespace MMM::UI
