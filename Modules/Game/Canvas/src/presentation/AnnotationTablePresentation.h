#pragma once
#include <cstddef>
#include <memory>
#include <optional>

namespace MMM::Canvas
{
class AnnotationTableData;
/// @brief 独立批注表的平台呈现边界，不依赖时间线或主画布生命周期。
/// @details 平台对象仅持有 UI 编辑副本；谱面修改仍经过正式批注命令。
// 平台实现不修改窗口数据缓存，返回值只决定是否跳过桌面列表。
class AnnotationTablePresentation
{
public:
    /// @brief 析构前窗口已停止更新，不持有会话或设备所有权。
    virtual ~AnnotationTablePresentation() = default;
    /// @brief 紧凑布局由平台实现决定；桌面保持原有导出工具行。
    virtual bool touchLayout() const { return false; }
    /// @brief 绘制前为当前平台设置窗口尺寸约束；桌面沿用原保存布局。
    /// @warning 每帧只提交标量约束，不读取设备、会话或磁盘。
    virtual void prepareWindow() const {}
    /// @brief 返回真时本帧列表与详情已由平台呈现，桌面继续原八列表格。
    /// @warning 每帧借用窗口自持快照，不访问会话锁或文件系统。
    virtual bool render(const AnnotationTableData&, std::optional<std::size_t>&)
    {
        return false;
    }
    /// @brief 窗口关闭时丢弃未提交副本，重新打开不得写入旧谱面。
    virtual void reset() {}
};
/// @brief 构造期由 CMake 选择平台，不在公共窗口头传播移动状态。
std::unique_ptr<AnnotationTablePresentation>
createAnnotationTablePresentation();
/// @brief CPU 回归可直接创建同一移动生产呈现，不替换桌面工厂。
std::unique_ptr<AnnotationTablePresentation>
createMobileAnnotationTablePresentation();
}  // namespace MMM::Canvas
