#pragma once

#include "canvas/CanvasSnapshotPrepare.h"
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

    /// @brief 在线程池中拉取并准备画布渲染快照。
    /// @param snapshot 当前帧 UI 快照。
    /// @warning 后台线程路径：只消费 BeatmapSyncBuffer 并处理动态顶点偏移。
    void prepareUiFrameData(const UI::UiFrameSnapshot& snapshot) override;

    /// @brief 将准备好的画布快照切换到主线程可见状态。
    void swapPreparedUiFrameData() override;

    ///@brief 是否需要重新记录命令 (比如数据变了)
    bool isDirty() const override;

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

    ///@brief 全局图集
    std::unique_ptr<Graphic::VKTextureAtlas> m_textureAtlas{ nullptr };

    ///@brief 图集 UV 缓存，用于同步给逻辑线程
    std::unordered_map<uint32_t, glm::vec4> m_atlasUVs;

    // --- Vulkan devices for dynamic loading ---
    vk::PhysicalDevice m_physicalDevice{ nullptr };
    vk::Device         m_logicalDevice{ nullptr };
    vk::CommandPool    m_cmdPool{ nullptr };
    vk::Queue          m_queue{ nullptr };

    std::unique_ptr<Graphic::VKTexture> m_bgTexture{ nullptr };
    std::string                         m_loadedBgPath{ "" };

    std::unique_ptr<Basic2DCanvasInteraction> m_interaction;

    /// @brief 上一次应用到动态顶点上的 Y 偏移量
    float m_lastAppliedYOffset{ 0.0f };

    /// @brief 上一次应用偏移的快照指针 (用于检测快照是否更新)
    Logic::RenderSnapshot* m_lastOffsetSnapshot{ nullptr };

    /// @brief 后台准备出的画布快照消费结果。
    PreparedCanvasSnapshot m_preparedSnapshot;

    /// @brief 是否有后台准备结果等待主线程切换。
    bool m_hasPreparedSnapshot{ false };

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
    /// @brief 当快照背景路径变化时加载或清理背景纹理。
    /// @warning 低频阻塞路径：可能访问文件系统、创建 Vulkan 纹理并等待
    /// GPU；只能在背景路径变化时调用，严禁每帧执行。
    void updateBackgroundTexture();

    /// @brief 判断已关闭窗口是否应保留并重置为 Logo 占位画布。
    /// @return 唯一真实谱面正在关闭时返回 true。
    /// @warning UI 热路径辅助：UIManager 每帧关闭检查时可能调用；只读取
    /// SessionRegistry 的常量规模状态。
    bool shouldKeepOpenForLastSessionReset() const;
};

}  // namespace MMM::Canvas
