#include "graphic/imguivk/VKTextureAtlas.h"
#include "log/colorful-log.h"
#include <stb_image.h>

#define STB_RECT_PACK_IMPLEMENTATION
#include <stb_rect_pack.h>

#include <algorithm>
#include <stdexcept>

namespace MMM::Graphic
{

VKTextureAtlas::VKTextureAtlas(vk::PhysicalDevice& physicalDevice,
                               vk::Device& device, vk::CommandPool commandPool,
                               vk::Queue queue)
    : m_device(device)
    , m_physDevice(physicalDevice)
    , m_pool(commandPool)
    , m_queue(queue)
{
}

VKTextureAtlas::~VKTextureAtlas() {}

void VKTextureAtlas::addTexture(uint32_t id, const std::string& filePath)
{
    int      texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(
        filePath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

    if ( !pixels ) {
        XERROR("Failed to load texture for atlas: {}", filePath);
        return;
    }

    TextureData data;
    data.id = id;
    data.w  = static_cast<uint32_t>(texWidth);
    data.h  = static_cast<uint32_t>(texHeight);
    data.pixels.assign(pixels, pixels + (texWidth * texHeight * 4));

    stbi_image_free(pixels);
    m_pendingTextures.push_back(std::move(data));

    XDEBUG("Texture added to atlas pending list: {} ({}x{})",
           filePath,
           data.w,
           data.h);
}

void VKTextureAtlas::addTexture(uint32_t id, const unsigned char* pixels,
                                uint32_t w, uint32_t h)
{
    TextureData data;
    data.id = id;
    data.w  = w;
    data.h  = h;
    data.pixels.assign(pixels, pixels + (w * h * 4));
    m_pendingTextures.push_back(std::move(data));
}

void VKTextureAtlas::build(uint32_t atlasSize)
{
    if ( m_pendingTextures.empty() ) {
        // 创建一个纯白 1x1 纹理作为后备，确保 getDescriptorSet() 不返回空
        unsigned char white[] = { 255, 255, 255, 255 };
        m_atlasTexture        = std::make_unique<VKTexture>(
            white, 1, 1, m_physDevice, m_device, m_pool, m_queue);
        return;
    }

    // 1. 准备矩形打包数据 (添加间距避免采样溢出)
    const int               padding = 2;
    std::vector<stbrp_rect> rects(m_pendingTextures.size());
    for ( size_t i = 0; i < m_pendingTextures.size(); ++i ) {
        rects[i].id = static_cast<int>(i);
        rects[i].w =
            static_cast<stbrp_coord>(m_pendingTextures[i].w + padding * 2);
        rects[i].h =
            static_cast<stbrp_coord>(m_pendingTextures[i].h + padding * 2);
    }

    // 2. 打包矩形
    stbrp_context           context;
    std::vector<stbrp_node> nodes(atlasSize);
    stbrp_init_target(&context,
                      atlasSize,
                      atlasSize,
                      nodes.data(),
                      static_cast<int>(nodes.size()));
    stbrp_pack_rects(&context, rects.data(), static_cast<int>(rects.size()));

    // 3. 创建合图像素缓冲区
    std::vector<unsigned char> atlasPixels(atlasSize * atlasSize * 4, 0);

    for ( size_t i = 0; i < rects.size(); ++i ) {
        if ( rects[i].was_packed ) {
            const auto& src = m_pendingTextures[rects[i].id];
            // 考虑间距后的实际放置位置
            int startX = rects[i].x + padding;
            int startY = rects[i].y + padding;

            // 将小纹理像素拷贝到大图对应位置
            for ( uint32_t row = 0; row < src.h; ++row ) {
                size_t srcOffset = row * src.w * 4;
                size_t dstOffset = ((startY + row) * atlasSize + startX) * 4;
                std::copy(src.pixels.begin() + srcOffset,
                          src.pixels.begin() + srcOffset + (src.w * 4),
                          atlasPixels.begin() + dstOffset);
            }

            // 记录 UV 坐标 (指向原始像素范围，不包含 padding)
            float u =
                static_cast<float>(startX) / static_cast<float>(atlasSize);
            float v =
                static_cast<float>(startY) / static_cast<float>(atlasSize);
            float rw =
                static_cast<float>(src.w) / static_cast<float>(atlasSize);
            float rh =
                static_cast<float>(src.h) / static_cast<float>(atlasSize);
            m_uvRects[src.id] = glm::vec4(u, v, rw, rh);

            XDEBUG("Atlas packed ID {}: [{}, {}] size [{}x{}]",
                   src.id,
                   rects[i].x,
                   rects[i].y,
                   src.w,
                   src.h);
        } else {
            XWARN("Failed to pack texture ID {} into atlas!",
                  m_pendingTextures[rects[i].id].id);
        }
    }

    // 4. 创建并上传纹理
    m_atlasTexture = std::make_unique<VKTexture>(atlasPixels.data(),
                                                 atlasSize,
                                                 atlasSize,
                                                 m_physDevice,
                                                 m_device,
                                                 m_pool,
                                                 m_queue);

    // 清理缓存数据
    m_pendingTextures.clear();
    XDEBUG(
        "Texture Atlas built successfully. Size: {}x{}", atlasSize, atlasSize);
}

glm::vec4 VKTextureAtlas::getUV(uint32_t id) const
{
    auto it = m_uvRects.find(id);
    if ( it != m_uvRects.end() ) {
        return it->second;
    }
    // 默认返回全图 UV
    return glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
}

ImTextureID VKTextureAtlas::getImTextureID()
{
    if ( m_atlasTexture ) {
        return m_atlasTexture->getImTextureID();
    }
    return 0;
}

vk::DescriptorSet VKTextureAtlas::getDescriptorSet() const
{
    if ( m_atlasTexture ) {
        return m_atlasTexture->getDescriptorSet();
    }
    return nullptr;
}

vk::DescriptorSet VKTextureAtlas::getNativeDescriptorSet(
    vk::DescriptorPool pool, vk::DescriptorSetLayout layout)
{
    if ( m_atlasTexture ) {
        return m_atlasTexture->getNativeDescriptorSet(pool, layout);
    }
    return nullptr;
}

}  // namespace MMM::Graphic
