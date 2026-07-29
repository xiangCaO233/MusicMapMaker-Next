#pragma once

#include "canvas/BackgroundVideoPlayer.h"
#include "canvas/CanvasSnapshotPrepare.h"
#include "common/AsciiFontData.h"
#include "common/UnicodeFontData.h"
#include "event/core/EventBus.h"
#include "graphic/imguivk/VKTextureAtlas.h"
#include "logic/BeatmapSyncBuffer.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/IRenderableView.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::Canvas
{
class Basic2DCanvasInteraction;
}

namespace MMM::Event
{
struct GLFWDropEvent;
}

namespace MMM::Logic
{
struct RenderSnapshot;
}

namespace MMM::Canvas
{
class Basic2DCanvas : public UI::IRenderableView,
                      public UI::IParallelUiPreparable
{
public:
    Basic2DCanvas(const std::string& name, uint32_t w, uint32_t h,
                  std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer,
                  const std::string&                        cameraId = "");
    Basic2DCanvas(Basic2DCanvas&&)                 = delete;
    Basic2DCanvas(const Basic2DCanvas&)            = delete;
    Basic2DCanvas& operator=(Basic2DCanvas&&)      = delete;
    Basic2DCanvas& operator=(const Basic2DCanvas&) = delete;

    ~Basic2DCanvas() override;

    // 接口实现
    /// @brief 更新画布 ImGui 窗口和交互状态。
    /// @warning 热路径：主渲染线程每帧执行；禁止文件系统访问、完整 ECS
    /// 遍历、完整排序和共享指针所有权复制。
    void update(UI::UIManager* sourceManager) override;

    /// @brief 获取窗口是否打开，用于拦截未保存的关闭
    bool isOpen() const override;

    /// @brief 请求关闭画布，复用未保存确认弹窗拦截 dirty 状态
    void requestClose();

    /// @brief 消费用户取消关闭操作的标记
    bool consumeCloseCancelled();

    /// @brief 请求下一次显示时停靠到主编辑区
    void requestDockToCenter();

    /// @brief 请求下一次更新时将画布窗口聚焦到前台。
    void requestFocus();

    /// @brief 获取画布当前所在的 ImGui Dock 节点。
    /// @return 当前窗口停靠节点 ID；未停靠时返回 0。
    ImGuiID getDockId() const;

    /// @brief 安全转换为 UI 并行准备接口。
    /// @return 当前画布的并行准备接口。
    UI::IParallelUiPreparable* asParallelUiPreparable() override
    {
        return this;
    }

    /// @brief 判断当前帧是否需要准备画布快照。
    /// @param snapshot 当前帧 UI 快照。
    /// @return 需要准备时返回 true。
    /// @warning UI 热路径：每帧主线程调用，只检查同步缓冲区和窗口状态。
    bool needsParallelUiPrepare(
        const UI::UiFrameSnapshot& snapshot) const override;

    /// @brief 声明画布快照准备可在线程池执行。
    /// @return 始终返回 false。
    /// @warning UI 热路径：每帧只返回固定能力标记。
    bool requiresMainThreadUiPrepare() const override { return false; }

    /// @brief 在线程池中拉取并准备画布渲染快照。
    /// @param snapshot 当前帧 UI 快照。
    /// @warning 后台线程路径：只消费 BeatmapSyncBuffer 并处理动态顶点偏移。
    void prepareUiFrameData(const UI::UiFrameSnapshot& snapshot) override;

    /// @brief 将准备好的画布快照切换到主线程可见状态。
    void swapPreparedUiFrameData() override;

    ///@brief 是否需要重新记录命令 (比如数据变了)
    bool isDirty() const override;

    /// @brief 当前帧是否需要录制主画布离屏渲染命令。
    /// @return 主画布窗口可见时返回 true。
    /// @warning 渲染热路径：每帧命令录制前调用，只读取 UI
    /// 线程维护的可见状态。
    bool shouldRecordOffscreen() const override;

    // --- 改变尺寸后的回调 ---
    void resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                    uint32_t h) const override;

    /**
     * @brief 获取 Shader 源码接口 (在固定时刻需要创建)
     */
    std::vector<std::string> getShaderSources(
        const std::string& shader_name) override;

    /**
     * @brief 获取 Shader 名称(需要按唯一名称名称储存和销毁)
     */
    std::string getShaderName(const std::string& shader_module_name) override;

    /// @brief 是否需要重载
    bool needReload() override;

    /// @brief 重载纹理
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

protected:
    const std::vector<Graphic::Vertex::VKBasicVertex>&
                                 getVertices() const override;
    const std::vector<uint32_t>& getIndices() const override;

    /// @brief 在离屏 RenderPass 开始前上传最新视频帧。
    /// @warning 渲染命令录制热路径：仅在存在新解码帧时向当前映射
    /// staging 槽复制固定尺寸 RGBA 数据并录制传输命令。
    void onRecordResourceUploads(vk::CommandBuffer& cmdBuf,
                                 uint32_t           frameIndex) override;

    /// @warning
    /// 热路径：每帧离屏命令录制时执行；只允许遍历当前快照命令并绑定已存在的
    /// descriptor。
    void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

    /// @warning 热路径：启用发光时每帧离屏命令录制执行；只允许遍历 glow
    /// 命令列表。
    void onRecordGlowCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

    /// @brief 记录主画布最终覆盖层离屏绘制命令。
    /// @warning 热路径：最终覆盖层每帧命令录制时执行；只遍历 overlay 命令列表。
    void onRecordOverlayCmds(vk::CommandBuffer&      cmdBuf,
                             vk::PipelineLayout      pipelineLayout,
                             vk::DescriptorSetLayout setLayout,
                             vk::DescriptorSet       defaultDescriptor,
                             uint32_t                frameIndex) override;

    /// @brief 判断当前快照是否包含发光绘制命令。
    /// @return 当前快照存在发光命令时返回 true。
    /// @warning 渲染热路径：每帧离屏命令录制前执行，只能读取快照命令数量。
    bool hasGlowDrawCmds() const override;

    /// @brief 判断当前快照是否包含最终覆盖层绘制命令。
    /// @return 存在覆盖层命令时返回 true。
    /// @warning 渲染热路径：每帧离屏命令录制时执行，只读取命令数量。
    bool hasOverlayDrawCmds() const override;

    /// @brief 清空缓存的 shader 源码。
    /// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
    void invalidateShaderSourceCache() override;

private:
    /// @brief 画布名称
    std::string m_canvasName;

    /// @brief 逻辑视口 ID (对应 BeatmapSession 中的 Camera)
    std::string m_cameraId;

    /// @brief 下一帧是否调用 ImGui::SetNextWindowFocus。
    bool m_shouldFocusNextFrame{ false };

    /// @brief 同步缓冲区
    /// @warning 热路径/共享指针：画布仅持有所有权确保缓冲区生命周期，update
    /// 中不得复制该 shared_ptr。
    std::shared_ptr<Logic::BeatmapSyncBuffer> m_syncBuffer;

    /// @brief 当前正在使用的渲染快照
    Logic::RenderSnapshot* m_currentSnapshot{ nullptr };

    /// @brief 缓存spv源码，避免重复读盘
    std::unordered_map<std::string, std::vector<std::string>>
        m_shaderSourceCache;

    ///@brief 是否需要重载
    bool m_needReload{ true };

    /// @brief 当前离屏画布图集中已加载的多档 ASCII 字体度量。
    Common::AsciiFontAtlasMetrics m_asciiFontAtlasMetrics;
    /// @brief 当前离屏画布图集中按项目资源名加载的 Unicode 字体度量。
    Common::UnicodeFontMetrics m_unicodeFontMetrics;
    /// @brief 最近一次加载图集时使用的软件 ASCII 字体偏好。
    std::string m_loadedAsciiFontPreference;
    /// @brief 最近一次加载图集时使用的软件 CJK 字体偏好。
    std::string m_loadedCjkFontPreference;

    ///@brief 全局图集
    std::unique_ptr<Graphic::VKTextureAtlas> m_textureAtlas{ nullptr };

    ///@brief 图集 UV 缓存，用于同步给逻辑线程
    std::unordered_map<uint32_t, glm::vec4> m_atlasUVs;

    // --- 动态加载使用的 Vulkan 设备 ---
    vk::PhysicalDevice m_physicalDevice{ nullptr };
    vk::Device         m_logicalDevice{ nullptr };
    vk::CommandPool    m_cmdPool{ nullptr };
    vk::Queue          m_queue{ nullptr };

    std::unique_ptr<Graphic::VKTexture> m_bgTexture{ nullptr };
    std::string                         m_loadedBgPath{ "" };

    /// @brief 已加载背景资源是否按视频处理。
    bool m_loadedBackgroundIsVideo{ false };

    /// @brief 专用后台背景视频解码器。
    std::unique_ptr<BackgroundVideoPlayer> m_backgroundVideoPlayer;

    /// @brief 当前是否已有可显示的视频帧内容。
    bool m_videoFrameAvailable{ false };

    /// @brief 当前背景视频路径是否指向可读取文件。
    bool m_videoSourceAvailable{ false };

    /// @brief 当前视频帧是否已到谱面显示时间。
    bool m_videoFrameVisible{ false };

    /// @brief 当前 UI 帧的谱面时钟是否允许显示视频。
    bool m_videoShouldBeVisibleThisFrame{ false };

    /// @brief 当前资源是否已提交过视频帧请求。
    bool m_hasRequestedVideoFrame{ false };

    /// @brief 最近一次提交给解码线程的视频时间。
    double m_lastRequestedVideoTime{ 0.0 };

    /// @brief 最近一次提交视频帧请求的 steady_clock 时间（秒）。
    double m_lastVideoFrameRequestSysTime{ 0.0 };

    /// @brief 当前暂停 Seek 代际已经补发的有限重试次数。
    std::uint32_t m_pendingVideoSeekRetryCount{ 0 };

    /// @brief 是否正在等待当前资源/Seek 代际的首帧。
    bool m_videoDiscontinuityPending{ false };

    /// @brief 当前待完成资源/Seek 代际的目标时间。
    double m_videoDiscontinuityTargetTime{ 0.0 };

    /// @brief 当前帧是否已到达视频末尾。
    bool m_videoReachedEnd{ false };

    /// @brief 当前 GPU 纹理内容是否已有对应视频时间戳。
    bool m_hasUploadedVideoTimestamp{ false };

    /// @brief 当前 GPU 纹理内容对应的 VFR 帧时间戳。
    double m_uploadedVideoTimestamp{ 0.0 };

    /// @brief 允许恢复显示的最小资源/Seek 代际。
    std::uint64_t m_requiredVideoRequestGeneration{ 0 };

    /// @brief 等待下一次命令录制上传的 RGBA 视频帧。
    /// @warning UI 主线程写入后，只由同帧后续渲染录制阶段读取；
    /// 两阶段由 VKRenderer 帧调度串行，不得从解码线程直接访问。
    std::vector<unsigned char> m_pendingVideoPixels;

    /// @brief 待上传视频帧的本地修订号。
    std::uint64_t m_pendingVideoUploadRevision{ 0 };

    /// @brief 待上传帧是否携带有效时间与代际状态。
    bool m_pendingVideoStateValid{ false };

    /// @brief 待上传帧的 VFR 时间戳。
    double m_pendingVideoTimestamp{ 0.0 };

    /// @brief 待上传帧所属资源/Seek 代际。
    std::uint64_t m_pendingVideoRequestGeneration{ 0 };

    /// @brief 待上传帧是否已到达视频末尾。
    bool m_pendingVideoReachedEnd{ false };

    /// @brief 已记录到 GPU 命令缓冲的视频帧修订号。
    std::uint64_t m_recordedVideoUploadRevision{ 0 };

    std::unique_ptr<Basic2DCanvasInteraction> m_interaction;

    /// @brief 上一次应用到动态顶点上的 Y 偏移量
    float m_lastAppliedYOffset{ 0.0f };

    /// @brief 上一次应用偏移的快照指针 (用于检测快照是否更新)
    Logic::RenderSnapshot* m_lastOffsetSnapshot{ nullptr };

    /// @brief 后台准备出的画布快照消费结果。
    PreparedCanvasSnapshot m_preparedSnapshot;

    /// @brief 是否有后台准备结果等待主线程切换。
    bool m_hasPreparedSnapshot{ false };

    /// @brief 当前主画布窗口是否真实可见。
    bool m_isCanvasVisible{ true };

    /// @brief 是否显示保存确认弹窗
    bool m_showSaveConfirm{ false };

    /// @brief 上一次关闭请求是否被用户取消
    bool m_closeCancelled{ false };

    /// @brief 用户是否已经确认关闭并允许跳过 dirty 拦截
    bool m_closeConfirmed{ false };

    /// @brief 是否需要在下一次 Begin 前停靠到主编辑区
    bool m_shouldDockToCenter{ false };

    /// @brief 当前画布窗口最近一次更新时所在的 ImGui Dock 节点。
    ImGuiID m_lastDockId{ 0 };

private:
    /// @brief 当快照背景路径或类型变化时加载或清理背景资源。
    /// @warning 低频阻塞路径：可能访问文件系统、创建 Vulkan 纹理并等待
    /// GPU；只能在背景路径或类型变化时调用，严禁每帧执行。
    void updateBackgroundTexture();

    /// @brief 根据谱面原始播放时钟请求并消费最新视频帧。
    /// @warning UI 热路径：每帧只进行常数时间计算和非阻塞取帧；
    /// 只有分辨率或资源变化时允许低频重建 Vulkan 纹理。
    void updateBackgroundVideoFrame();

    /// @brief 在对应像素已进入 GPU 纹理后提交视频帧状态。
    /// @param timestamp 已上传帧的 VFR 时间戳。
    /// @param requestGeneration 该帧所属资源/Seek 代际。
    /// @param reachedEnd 该帧是否为已知视频末尾帧。
    /// @warning UI 阶段或离屏录制阶段调用；两阶段由渲染器串行，
    /// 不得从解码线程调用。
    void commitUploadedVideoFrame(double        timestamp,
                                  std::uint64_t requestGeneration,
                                  bool          reachedEnd);

    /// @brief 判断已关闭窗口是否应保留并重置为 Logo 占位画布。
    /// @return 唯一真实谱面正在关闭时返回 true。
    /// @warning UI 热路径辅助：UIManager 每帧关闭检查时可能调用；只读取
    /// SessionRegistry 的常量规模状态。
    bool shouldKeepOpenForLastSessionReset() const;
};

}  // namespace MMM::Canvas
