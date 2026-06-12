#pragma once

#include "canvas/CanvasSnapshotPrepare.h"
#include "graphic/imguivk/VKTextureAtlas.h"
#include "logic/BeatmapSyncBuffer.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/IRenderableView.h"
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

namespace MMM::Logic
{
struct RenderSnapshot;
}

namespace MMM::Canvas
{
/**
 * @brief 预览画布类，专门用于右侧预览窗口。
 * 独立于 Basic2DCanvas，具有精简的交互逻辑（仅点击跳转时间）。
 */
class PreviewCanvas : public UI::IRenderableView,
                      public UI::IParallelUiPreparable
{
public:
    PreviewCanvas(const std::string& name, uint32_t w, uint32_t h,
                  std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer);
    PreviewCanvas(PreviewCanvas&&)                 = delete;
    PreviewCanvas(const PreviewCanvas&)            = delete;
    PreviewCanvas& operator=(PreviewCanvas&&)      = delete;
    PreviewCanvas& operator=(const PreviewCanvas&) = delete;

    ~PreviewCanvas() override = default;

    // 接口实现
    /// @brief 更新预览画布 ImGui 窗口和鼠标交互。
    /// @warning 热路径：主渲染线程每帧执行；禁止文件系统访问、完整 ECS
    /// 遍历、完整排序和共享指针所有权复制。
    void update(UI::UIManager* sourceManager) override;

    ///@brief 是否需要重新记录命令 (根据快照更新状态)
    bool isDirty() const override;

    /// @brief 安全转换为 UI 并行准备接口。
    /// @return 当前预览画布的并行准备接口。
    UI::IParallelUiPreparable* asParallelUiPreparable() override
    {
        return this;
    }

    /// @brief 判断当前帧是否需要准备预览快照。
    /// @param snapshot 当前帧 UI 快照。
    /// @return 需要准备时返回 true。
    /// @warning UI 热路径：每帧主线程调用，只检查同步缓冲区和窗口状态。
    bool needsParallelUiPrepare(
        const UI::UiFrameSnapshot& snapshot) const override;

    /// @brief 在线程池中拉取并准备预览画布快照。
    /// @param snapshot 当前帧 UI 快照。
    /// @warning 后台线程路径：只消费 BeatmapSyncBuffer 并处理动态顶点偏移。
    void prepareUiFrameData(const UI::UiFrameSnapshot& snapshot) override;

    /// @brief 将准备好的预览快照切换到主线程可见状态。
    /// @warning UI 热路径：每帧只切换快照；无活跃谱面时清空旧帧。
    void swapPreparedUiFrameData() override;

    // --- 改变尺寸后的回调 ---
    void resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                    uint32_t h) const override;

    /**
     * @brief 获取 Shader 源码接口
     */
    std::vector<std::string> getShaderSources(
        const std::string& shader_name) override;

    /**
     * @brief 获取 Shader 名称
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
    /// @warning 热路径：每帧离屏命令录制时执行；只允许遍历当前快照命令并复用
    /// descriptor。
    void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

    /// @brief 记录预览画布最终覆盖层离屏绘制命令。
    /// @warning 热路径：最终覆盖层每帧命令录制时执行；只遍历 overlay 命令列表。
    void onRecordOverlayCmds(vk::CommandBuffer&      cmdBuf,
                             vk::PipelineLayout      pipelineLayout,
                             vk::DescriptorSetLayout setLayout,
                             vk::DescriptorSet       defaultDescriptor,
                             uint32_t                frameIndex) override;

    /// @brief 判断当前快照是否包含最终覆盖层绘制命令。
    /// @return 存在覆盖层命令时返回 true。
    /// @warning 渲染热路径：每帧离屏命令录制时执行，只读取命令数量。
    bool hasOverlayDrawCmds() const override;

    /// @brief 清空缓存的 shader 源码。
    /// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
    void invalidateShaderSourceCache() override;

private:
    /// @brief 上一次发送给逻辑线程的鼠标状态，用于过滤重复交互命令。
    struct LastMouseCommand {
        /// @brief 是否已经记录过一次鼠标命令。
        bool valid{ false };
        /// @brief 上一次发送的本地鼠标坐标。
        glm::vec2 pos{ 0.0f, 0.0f };
        /// @brief 上一次发送的视口宽度。
        float viewportWidth{ 0.0f };
        /// @brief 上一次发送的视口高度。
        float viewportHeight{ 0.0f };
        /// @brief 上一次发送的窗口悬浮状态。
        bool isHovering{ false };
        /// @brief 上一次发送的鼠标拖拽状态。
        bool isDragging{ false };
    };

    /// @brief 画布名称 (对应翻译和 ID)
    std::string m_canvasName;

    /// @brief 逻辑视口 ID (固定为 "Preview")
    const std::string m_cameraId{ "Preview" };

    /// @brief 同步缓冲区
    /// @warning 热路径/共享指针：画布仅持有所有权确保缓冲区生命周期，update
    /// 中不得复制该 shared_ptr。
    std::shared_ptr<Logic::BeatmapSyncBuffer> m_syncBuffer;

    /// @brief 当前正在使用的渲染快照
    Logic::RenderSnapshot* m_currentSnapshot{ nullptr };

    /// @brief 缓存spv源码
    std::unordered_map<std::string, std::vector<std::string>>
        m_shaderSourceCache;

    ///@brief 是否需要重载
    bool m_needReload{ true };

    ///@brief 全局图集 (预览区也需要绘制 Note)
    std::unique_ptr<Graphic::VKTextureAtlas> m_textureAtlas{ nullptr };

    ///@brief 图集 UV 缓存
    std::unordered_map<uint32_t, glm::vec4> m_atlasUVs;

    // --- Vulkan devices ---
    vk::PhysicalDevice m_physicalDevice{ nullptr };
    vk::Device         m_logicalDevice{ nullptr };
    vk::CommandPool    m_cmdPool{ nullptr };
    vk::Queue          m_queue{ nullptr };

    /// @brief 上一次应用到动态顶点上的 Y 偏移量
    float m_lastAppliedYOffset{ 0.0f };

    /// @brief 上一次应用偏移的快照指针
    Logic::RenderSnapshot* m_lastOffsetSnapshot{ nullptr };

    /// @brief 后台准备出的预览快照消费结果。
    PreparedCanvasSnapshot m_preparedSnapshot;

    /// @brief 是否有后台准备结果等待主线程切换。
    bool m_hasPreparedSnapshot{ false };

    /// @brief 上一次发送给逻辑线程的鼠标状态。
    LastMouseCommand m_lastMouseCommand;
};

}  // namespace MMM::Canvas
