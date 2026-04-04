#pragma once

#include "graphic/imguivk/VKTextureAtlas.h"
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
class Basic2DCanvas : public UI::IRenderableView
{
public:
    Basic2DCanvas(const std::string& name, uint32_t w, uint32_t h);
    Basic2DCanvas(Basic2DCanvas&&)                 = delete;
    Basic2DCanvas(const Basic2DCanvas&)            = delete;
    Basic2DCanvas& operator=(Basic2DCanvas&&)      = delete;
    Basic2DCanvas& operator=(const Basic2DCanvas&) = delete;

    ~Basic2DCanvas() override = default;

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
    void onRecordDrawCmds(vk::CommandBuffer& cmdBuf,
                          vk::PipelineLayout pipelineLayout,
                          vk::DescriptorSet  defaultDescriptor) override;

private:
    /// @brief 画布名称
    std::string m_canvasName;

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
};

}  // namespace MMM::Canvas
