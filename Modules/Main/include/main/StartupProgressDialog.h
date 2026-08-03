#pragma once

#include "graphic/imguivk/IGraphicUserHook.h"
#include "network/AssetSyncService.h"

#include <atomic>
#include <mutex>
#include <string>

namespace MMM::Main
{

/// @brief 使用最小 ImGui 环境绘制的启动期资源同步界面。
class StartupProgressDialog final : public Graphic::IGraphicUserHook
{
public:
    /// @brief 创建初始处于资源检查状态的启动界面。
    StartupProgressDialog() = default;

    StartupProgressDialog(const StartupProgressDialog&)            = delete;
    StartupProgressDialog& operator=(const StartupProgressDialog&) = delete;

    /// @brief 为一次新的资源同步尝试重置界面状态。
    /// @warning 启动低频路径：只能在上一轮同步线程结束后由主线程调用。
    void beginSync();

    /// @brief 发布后台资源同步进度。
    /// @param progress 最新资源同步进度快照。
    /// @warning 跨线程低频路径：后台线程写入互斥保护快照并发布原子脏位；
    /// 不得从这里调用 ImGui。
    void update(const Network::AssetSyncProgress& progress);

    /// @brief 在启动界面中展示可重试错误。
    /// @param title 错误标题。
    /// @param message 错误详情。
    /// @warning 启动低频路径：只能由渲染主线程调用。
    void showError(std::string title, std::string message);

    /// @brief 在启动界面中展示需要用户确认的提示。
    /// @param title 提示标题。
    /// @param message 提示详情。
    /// @warning 启动低频路径：只能由渲染主线程调用。
    void showWarning(std::string title, std::string message);

    /// @brief 消耗用户本帧发出的重试请求。
    /// @return 用户点击重试时返回 true，并清除请求。
    bool consumeRetryRequest();

    /// @brief 消耗用户本帧发出的继续请求。
    /// @return 用户确认提示并点击继续时返回 true，并清除请求。
    bool consumeContinueRequest();

    /// @brief 判断用户是否请求退出启动流程。
    /// @return 用户点击退出时返回 true。
    [[nodiscard]] bool isExitRequested() const;

    /// @brief 启动界面不需要准备额外 Vulkan 资源。
    /// @warning 启动渲染热路径：每帧调用，保持为空且不得加入阻塞操作。
    void onPrepareResources(vk::PhysicalDevice&, vk::Device&,
                            Graphic::VKSwapchain&, vk::CommandPool&,
                            vk::Queue&) override;

    /// @brief 绘制启动期资源检查、进度或错误界面。
    /// @warning 启动渲染热路径：每帧执行，只消费已发布快照并绘制 ImGui。
    void onUpdateUI() override;

    /// @brief 启动界面不录制离屏绘制命令。
    /// @warning 启动渲染热路径：保持为空。
    void onRecordOffscreen(vk::CommandBuffer&, uint32_t) override;

    /// @brief 获取启动界面的离屏任务数量。
    /// @return 始终为 0。
    /// @warning 启动渲染热路径：只返回稳定常量。
    [[nodiscard]] uint32_t getOffscreenRecordTaskCount() const override;

private:
    /// @brief 消费后台线程发布的最新进度快照。
    /// @warning 启动渲染热路径：仅当原子脏位为 true 时短暂获取互斥锁。
    void consumePendingProgress();

    /// @brief 计算进度条比例。
    /// @param progress 资源同步进度快照。
    /// @return 0 到 1 之间的进度比例。
    static float progressFraction(const Network::AssetSyncProgress& progress);

    /// @brief 生成启动界面进度说明。
    /// @param progress 资源同步进度快照。
    /// @return 面向用户的进度文本。
    static std::string progressText(const Network::AssetSyncProgress& progress);

    /// @brief 后台线程写入的最新进度快照。
    Network::AssetSyncProgress m_pendingProgress;

    /// @brief 渲染线程当前使用的稳定进度快照。
    Network::AssetSyncProgress m_visibleProgress;

    /// @brief 保护后台进度快照的互斥锁。
    /// @warning 仅资源同步回调写入和渲染线程消费脏快照时获取，不得持锁执行
    /// 网络、文件系统或 ImGui 操作。
    std::mutex m_progressMutex;

    /// @brief 是否存在尚未被渲染线程消费的新进度。
    /// @warning 跨线程原子脏位：后台同步线程 release 写入，渲染线程 acquire
    /// 消费；只传递快照可见性，不承载业务状态。
    std::atomic<bool> m_progressDirty{ false };

    /// @brief 当前是否展示错误状态。
    bool m_hasError{ false };

    /// @brief 当前是否展示需要用户确认的提示状态。
    bool m_hasWarning{ false };

    /// @brief 用户是否请求重新同步资源。
    bool m_retryRequested{ false };

    /// @brief 用户是否确认提示并继续启动。
    bool m_continueRequested{ false };

    /// @brief 用户是否请求退出程序。
    bool m_exitRequested{ false };

    /// @brief 当前错误标题。
    std::string m_errorTitle;

    /// @brief 当前错误详情。
    std::string m_errorMessage;

    /// @brief 当前提示标题。
    std::string m_warningTitle;

    /// @brief 当前提示详情。
    std::string m_warningMessage;
};

}  // namespace MMM::Main
