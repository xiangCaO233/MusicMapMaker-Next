#pragma once

#include "graphic/imguivk/VKTextureAtlas.h"
#include "logic/BeatmapSyncBuffer.h"
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
class PreviewCanvas : public UI::IRenderableView
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
    void update(UI::UIManager* sourceManager) override;

    ///@brief 是否需要重新记录命令 (根据快照更新状态)
    bool isDirty() const override;

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
    void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

private:
    /// @brief 画布名称 (对应翻译和 ID)
    std::string m_canvasName;

    /// @brief 逻辑视口 ID (固定为 "Preview")
    const std::string m_cameraId{ "Preview" };

    /// @brief 同步缓冲区
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
};

}  // namespace MMM::Canvas
