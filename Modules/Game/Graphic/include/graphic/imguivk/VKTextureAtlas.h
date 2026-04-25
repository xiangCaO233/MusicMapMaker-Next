#pragma once

#include "graphic/imguivk/VKTexture.h"
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace MMM::Graphic
{

/**
 * @brief 纹理集类，负责将多个小纹理打进一个大纹理中，以减少 DrawCall
 * 和纹理绑定开销
 */
class VKTextureAtlas
{
public:
    VKTextureAtlas(vk::PhysicalDevice& physicalDevice, vk::Device& device,
                   vk::CommandPool commandPool, vk::Queue queue);
    ~VKTextureAtlas();

    /**
     * @brief 添加一个要打进图集的纹理资源
     * @param id 自定义ID (对应 Logic::TextureID)
     * @param filePath 纹理文件路径
     */
    void addTexture(uint32_t id, const std::string& filePath);

    /**
     * @brief 添加一个内存中的像素数据到图集
     */
    void addTexture(uint32_t id, const unsigned char* pixels, uint32_t w,
                    uint32_t h);

    /**
     * @brief 构建图集并上传到 GPU
     * @param atlasSize 图集的分辨率 (通常是 1024, 2048, 4096)
     */
    void build(uint32_t atlasSize = 2048);

    /**
     * @brief 获取指定 ID 纹理在图集中的 UV 矩形
     * @return glm::vec4 (u, v, width_ratio, height_ratio)
     */
    glm::vec4 getUV(uint32_t id) const;

    /**
     * @brief 获取 ImGui 可用的纹理 ID
     */
    ImTextureID getImTextureID();

    /**
     * @brief 获取 Vulkan 描述符集
     */
    vk::DescriptorSet getDescriptorSet() const;

    /**
     * @brief 获取适配本项目原生管线的描述符集 (CombinedImageSampler)
     */
    vk::DescriptorSet getNativeDescriptorSet(vk::DescriptorPool      pool,
                                             vk::DescriptorSetLayout layout);

private:
    struct TextureData {
        uint32_t                   id;
        std::vector<unsigned char> pixels;
        uint32_t                   w, h;
    };

    vk::Device         m_device;
    vk::PhysicalDevice m_physDevice;
    vk::CommandPool    m_pool;
    vk::Queue          m_queue;

    std::vector<TextureData>      m_pendingTextures;
    std::map<uint32_t, glm::vec4> m_uvRects;

    std::unique_ptr<VKTexture> m_atlasTexture;
};

}  // namespace MMM::Graphic
