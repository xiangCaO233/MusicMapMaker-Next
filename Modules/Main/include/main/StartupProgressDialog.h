#pragma once

#include "graphic/imguivk/IGraphicUserHook.h"
#include "network/AssetSyncService.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace MMM::Main
{
class StartupLogoShader;

/// @brief 使用最小 ImGui 环境绘制的启动期资源同步界面。
class StartupProgressDialog final : public Graphic::IGraphicUserHook
{
public:
    /// @brief 创建初始处于资源检查状态的启动界面。
    StartupProgressDialog();
    /// @brief 在 Vulkan 启动上下文之前释放 Logo 管线。
    ~StartupProgressDialog();

    StartupProgressDialog(const StartupProgressDialog&)            = delete;
    StartupProgressDialog& operator=(const StartupProgressDialog&) = delete;

    /// @brief 为一次新的资源同步尝试重置界面状态。
    /// @warning 启动低频路径：只能在上一轮同步线程结束后由主线程调用。
    void beginSync();

    /// @brief 查询首轮启动动效是否仍在播放。
    /// @warning 启动渲染热路径：只读单调时钟，不等待动画或同步任务。
    [[nodiscard]] bool isIntroPlaying() const;

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

    /// @brief 取走一次手动选择本地资源包的请求。
    bool consumeImportRequest();

    /// @brief 发布当前文件选择器的状态，由启动主线程写入。
    /// @param pending 系统授权或缓存复制尚未完成。
    /// @param failed 系统选择失败，界面仍可继续当前同步。
    void setImportSelectionState(bool pending, bool failed = false);

    /// @brief 消耗用户本帧发出的继续请求。
    /// @return 用户确认提示并点击继续时返回 true，并清除请求。
    bool consumeContinueRequest();

    /// @brief 判断用户是否请求退出启动流程。
    /// @return 用户点击退出时返回 true。
    [[nodiscard]] bool isExitRequested() const;

    /// @brief 启动界面首次绘制前初始化 Logo 管线。
    /// @warning 每帧检查一次指针；创建资源仅发生在首次调用。
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

    /// @brief 渲染线程累计的动效时间，重试继续沿用已展开的界面。
    float m_introElapsed{ 0.0f };

    /// @brief 首次提交静态 Logo 终点帧后才能启动资源同步。
    bool m_introPresented{ false };

    /// @brief 只有资源任务开始后才显示进度和线路等控件。
    bool m_syncStarted{ false };

    /// @brief 只在启动窗口生命周期内持有的单色管线。
    std::unique_ptr<StartupLogoShader> m_logoShader;

    /// @brief 有界阶段日志，重复进度包不追加相同的消息。
    std::deque<std::string> m_logs;

    /// @brief 详情已经展开时才提交日志滚动区。
    bool m_detailsExpanded{ false };

    /// @brief 当前阶段说明，只在进度变化时格式化。
    std::string m_progressLabel;

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

    /// @brief 用户点击手动导入，本帧后在 ImGui 之外打开选取器。
    bool m_importRequested{ false };

    /// @brief 有一个系统选取器或平台缓存复制正在进行。
    bool m_importPending{ false };

    /// @brief 选择阶段发生平台错误，允许重新打开选择器。
    bool m_importFailed{ false };

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
