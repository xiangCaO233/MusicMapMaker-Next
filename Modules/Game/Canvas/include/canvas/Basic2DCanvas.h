#pragma once

#include "event/core/EventBus.h"
#include "graphic/imguivk/VKTextureAtlas.h"
#include "logic/BeatmapSyncBuffer.h"
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
class Basic2DCanvas : public UI::IRenderableView
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
    void update(UI::UIManager* sourceManager) override;

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
    void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

    void onRecordGlowCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

private:
    /// @brief 画布名称
    std::string m_canvasName;

    /// @brief 逻辑视口 ID (对应 BeatmapSession 中的 Camera)
    std::string m_cameraId;

    /// @brief 同步缓冲区
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

private:
    void updateBackgroundTexture();
};

}  // namespace MMM::Canvas
