#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MMM::UI
{
class IUIView;

/// @brief UI 线程同步画布生命周期所需的轻量工作区条目。
struct CanvasWorkspaceEntry {
    /// @brief 画布与逻辑会话共享的稳定标识。
    std::string cameraId;

    /// @brief 是否为无谱面的欢迎占位会话。
    bool isLogoPlaceholder{ false };

    /// @brief 是否应恢复项目工作区保存的 Dock 状态。
    bool restoreDockFromWorkspace{ false };
};

/// @brief 由组合根实现的画布工作区端口，隔离 UI 与 Logic/Canvas 具体类型。
class ICanvasWorkspaceService
{
public:
    virtual ~ICanvasWorkspaceService() = default;

    /// @brief 填充当前画布会话的 UI 只读快照。
    /// @param entries 接收条目的复用容器；实现应保留其已有容量。
    /// @warning UI 热路径：每帧调用一次；实现只能复制轻量条目，不得复制 Session
    /// 所有权、遍历 ECS 或无条件创建临时容器。
    virtual void fillEntries(std::vector<CanvasWorkspaceEntry>& entries) = 0;

    /// @brief 获取当前活动画布会话索引。
    [[nodiscard]] virtual std::int32_t getActiveEntryIndex() const = 0;

    /// @brief 判断项目是否正在执行挂起的切换流程。
    [[nodiscard]] virtual bool hasPendingProjectSwitch() const = 0;

    /// @brief 保存当前项目。
    virtual void saveProject() = 0;

    /// @brief 创建欢迎占位会话。
    virtual void createLogoPlaceholderSession(
        const std::string& displayName) = 0;

    /// @brief 关闭指定索引的画布会话。
    virtual void closeSession(std::int32_t index, bool updateWorkspace) = 0;

    /// @brief 获取当前画布会话数量。
    [[nodiscard]] virtual std::int32_t getEntryCount() const = 0;

    /// @brief 消费待处理的画布聚焦索引。
    [[nodiscard]] virtual std::int32_t consumePendingFocusIndex() = 0;

    /// @brief 重新排队指定画布会话的聚焦请求。
    virtual void requestEntryFocus(std::int32_t index) = 0;

    /// @brief 创建主编辑画布并把所有权交给 UIManager。
    /// @warning 低频会话创建路径：实现会复制同步缓冲区 shared_ptr 以保证画布
    /// 生命周期内的跨线程快照存活；不得在稳定的每帧路径重复调用。
    [[nodiscard]] virtual std::unique_ptr<IUIView> createMainCanvas(
        const CanvasWorkspaceEntry& entry, std::uint32_t width,
        std::uint32_t height) = 0;

    /// @brief 创建应用启动时的预览画布。
    [[nodiscard]] virtual std::unique_ptr<IUIView> createPreviewCanvas(
        const std::string& name, std::uint32_t width, std::uint32_t height) = 0;

    /// @brief 创建应用启动时的时间线画布。
    [[nodiscard]] virtual std::unique_ptr<IUIView> createTimelineCanvas(
        const std::string& name, std::uint32_t width, std::uint32_t height) = 0;
};

}  // namespace MMM::UI
