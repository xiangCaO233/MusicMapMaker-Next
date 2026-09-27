#pragma once

extern "C" {
#include <clay.h>
}
#include <string>

namespace MMM::UI
{

/// @brief 将 std::string 借用为 Clay_String。
/// @param s 生命周期必须覆盖 Clay 使用期的源字符串。
/// @return 不拥有字符内存的 Clay 字符串视图。
inline Clay_String ToClayString(const std::string& s)
{
    return { .isStaticallyAllocated = false,
             .length                = (int32_t)s.length(),
             .chars                 = s.c_str() };
}

/// @brief Clay 包装层支持的布局项语义类别。
enum class CLayItemType { Element, Spring };

/// @brief 单个 Clay 元素的稳定 ID 与尺寸配置。
struct CLayElementConfig {
    /// @brief 参与 Clay ID 生成的元素名称。
    std::string id;
    /// @brief 元素在主轴与交叉轴上的 Clay 尺寸策略。
    Clay_Sizing sizing;
};

/// @brief 管理 Clay 全局上下文、窗口独立上下文和 ImGui 文本测量桥接。
/// @details 单例拥有基础 Arena；每个窗口上下文拥有独立 Arena，调用方必须在
/// 不再绘制该窗口后通过 destroyWindowContext 成对释放。
class CLayWrapperCore
{
public:
    /// @brief 获取进程内唯一 Clay 包装器。
    /// @return 与进程生命周期一致的单例引用。
    static CLayWrapperCore& instance();
    /// @brief Clay Arena 地址稳定，禁止移动与复制包装器。
    CLayWrapperCore(CLayWrapperCore&&)                 = delete;
    CLayWrapperCore(const CLayWrapperCore&)            = delete;
    CLayWrapperCore& operator=(CLayWrapperCore&&)      = delete;
    CLayWrapperCore& operator=(const CLayWrapperCore&) = delete;
    /// @brief 释放单例持有的基础 Arena 内存。
    ~CLayWrapperCore();

    /// @brief 一个窗口专用的 Clay 上下文及其后备 Arena。
    struct WindowContext {
        /// @brief Arena 内构造的非拥有 Clay 上下文指针。
        Clay_Context* context;
        /// @brief 窗口上下文实际拥有的 Clay 内存竞技场。
        Clay_Arena arena;
    };

    /// @brief 为一个窗口分配并初始化独立 Clay 上下文。
    /// @return 调用方负责销毁的上下文与 Arena 句柄。
    WindowContext createWindowContext();
    /// @brief 释放窗口上下文并清空其句柄。
    /// @param ctx 由 createWindowContext 返回且尚未销毁的上下文。
    void destroyWindowContext(WindowContext& ctx);

    /// @brief 切换 Clay 当前活动上下文。
    /// @param ctx 目标上下文，nullptr 表示清除当前上下文。
    void makeCurrent(Clay_Context* ctx) { Clay_SetCurrentContext(ctx); }

    /// @brief 为当前 Clay 上下文安装 ImGui 字体测量回调。
    void setupClayTextMeasurement();

private:
    /// @brief 初始化单例基础 Arena 与 Clay 默认上下文。
    CLayWrapperCore();
    /// @brief 基础 Clay Arena 的最小内存容量。
    uint64_t clayMemorySize;
    /// @brief 单例默认上下文使用的后备 Arena。
    Clay_Arena clayArena;
};

}  // namespace MMM::UI
