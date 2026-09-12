#include "graphic/imguivk/VKTextureAtlas.h"
#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"
#include <stb_image.h>

#define STB_RECT_PACK_IMPLEMENTATION
#include <stb_rect_pack.h>

#include <algorithm>

namespace MMM::Graphic
{

/// @brief 创建尚未包含像素的纹理图集构建器。
///
/// Vulkan 句柄均按值保存但不取得 device、command pool 或 queue 的所有权；这些
/// 上下文资源必须覆盖图集及其最终 VKTexture 的整个生命周期。
///
/// @param physicalDevice 用于选择图像内存类型的物理设备。
/// @param device 创建图像和描述符资源的逻辑设备。
/// @param commandPool 上传纹理使用的一次性命令池。
/// @param queue 执行纹理上传命令的图形队列。
VKTextureAtlas::VKTextureAtlas(vk::PhysicalDevice& physicalDevice,
                               vk::Device& device, vk::CommandPool commandPool,
                               vk::Queue queue)
    : m_device(device)
    , m_physDevice(physicalDevice)
    , m_pool(commandPool)
    , m_queue(queue)
{
}

/// @brief 销毁图集；最终 GPU 纹理由 unique_ptr 自动按成员析构顺序释放。
VKTextureAtlas::~VKTextureAtlas() {}

/// @brief 解码磁盘图片并把 RGBA8 像素复制到待打包队列。
///
/// stb_image 返回的临时内存在本函数内释放，队列只保存自有像素副本，因此调用
/// 返回后文件可以移动或删除。解码失败只跳过当前条目，不影响此前已排队纹理。
///
/// @param id 调用方用于查询 UV 的稳定纹理 ID。
/// @param filePath 待解码图片路径。
/// @warning 资源加载低频路径：包含文件 I/O、图片解码和内存分配，禁止从渲染
/// 热路径调用。
void VKTextureAtlas::addTexture(uint32_t                     id,
                                const std::filesystem::path& filePath)
{
    // stb_image 接受窄字符路径；u8string 的字节序列在各平台保持 UTF-8，不经本地
    // 代码页转换，便于加载含非 ASCII 名称的皮肤资源。
    auto        u8Path = filePath.u8string();
    std::string utf8Path(reinterpret_cast<const char*>(u8Path.c_str()),
                         u8Path.size());

    // 强制输出四通道，令后续行跨度和 VKTexture 上传格式保持固定 RGBA8。
    int      texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(
        utf8Path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

    if ( !pixels ) {
        XERROR("Failed to load texture for atlas: {}", utf8Path);
        return;
    }

    // 在释放 stb_image 缓冲前复制像素，待构建队列不借用解码器内存。
    TextureData data;
    data.id = id;
    data.w  = static_cast<uint32_t>(texWidth);
    data.h  = static_cast<uint32_t>(texHeight);
    data.pixels.assign(pixels, pixels + (texWidth * texHeight * 4));

    // 先完成自有副本再释放第三方分配；队列中的 TextureData 可独立移动。
    stbi_image_free(pixels);
    m_pendingTextures.push_back(std::move(data));

    XDEBUG("Texture added to atlas pending list: {} ({}x{})",
           utf8Path,
           data.w,
           data.h);
}

/// @brief 把调用方提供的连续 RGBA8 像素复制到待打包队列。
///
/// 输入大小契约为 w * h * 4 字节，函数不会保留 pixels 指针。调用方负责确保
/// 指针非空且缓冲足够；本重载用于已经在内存中的内置纹理。
///
/// @param id 调用方用于查询 UV 的稳定纹理 ID。
/// @param pixels 连续 RGBA8 像素首地址。
/// @param w 纹理宽度（像素）。
/// @param h 纹理高度（像素）。
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

/// @brief 打包全部待处理纹理，生成 UV 映射并上传一张 GPU 图集。
///
/// 每个输入矩形四周预留固定透明间距，记录的 UV 仅覆盖原图内容。无法放入给定
/// 尺寸的条目会记录警告且不产生 UV；没有输入时仍创建白色 1x1 后备纹理，保证
/// 下游始终能够绑定有效 descriptor。
///
/// @param atlasSize 正方形图集的边长（像素）。
/// @warning 资源构建低频路径：会进行矩形打包、分配完整像素缓冲并同步上传 GPU，
/// 禁止从逐帧绘制或音频线程调用。
void VKTextureAtlas::build(uint32_t atlasSize)
{
    if ( m_pendingTextures.empty() ) {
        // 空图集仍需要可采样资源；白色纹理可让只使用顶点颜色的绘制保持原色。
        unsigned char white[] = { 255, 255, 255, 255 };
        m_atlasTexture        = std::make_unique<VKTexture>(
            white, 1, 1, m_physDevice, m_device, m_pool, m_queue);
        return;
    }

    // padding 属于占位矩形但不属于 UV 范围，可降低线性过滤采到相邻纹理的风险。
    // 当前实现不复制边缘像素，间距保持初始化后的全透明值。
    const int               padding = 2;
    std::vector<stbrp_rect> rects(m_pendingTextures.size());
    for ( size_t i = 0; i < m_pendingTextures.size(); ++i ) {
        rects[i].id = static_cast<int>(i);
        rects[i].w =
            static_cast<stbrp_coord>(m_pendingTextures[i].w + padding * 2);
        rects[i].h =
            static_cast<stbrp_coord>(m_pendingTextures[i].h + padding * 2);
    }

    // stb_rect_pack 使用每一水平扫描线的节点工作区；节点数取图集宽度即可覆盖
    // 默认启发式需要的状态，rect.id 则保留回到待处理纹理的稳定索引。
    stbrp_context           context;
    std::vector<stbrp_node> nodes(atlasSize);
    stbrp_init_target(&context,
                      atlasSize,
                      atlasSize,
                      nodes.data(),
                      static_cast<int>(nodes.size()));
    stbrp_pack_rects(&context, rects.data(), static_cast<int>(rects.size()));

    // 先分配完整透明 RGBA8 图像，未占用区域和纹理间距自然保持透明。
    std::vector<unsigned char> atlasPixels(atlasSize * atlasSize * 4, 0);

    for ( size_t i = 0; i < rects.size(); ++i ) {
        if ( rects[i].was_packed ) {
            // packer 可能重排 rect 数组，但 id 始终指向原始待处理条目。
            const auto& src = m_pendingTextures[rects[i].id];
            // packer 坐标指向含 padding 的外框，实际像素从内缩位置开始。
            int startX = rects[i].x + padding;
            int startY = rects[i].y + padding;

            // 逐行复制保持源纹理行连续，也避免对每个像素重复计算目标偏移。
            for ( uint32_t row = 0; row < src.h; ++row ) {
                size_t srcOffset = row * src.w * 4;
                size_t dstOffset = ((startY + row) * atlasSize + startX) * 4;
                std::copy(src.pixels.begin() + srcOffset,
                          src.pixels.begin() + srcOffset + (src.w * 4),
                          atlasPixels.begin() + dstOffset);
            }

            // UV 使用归一化左上角和宽高，不包含 padding；渲染调用方据此组合
            // 实际四角，未打包条目不会污染 m_uvRects。
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
            // 单个矩形失败不阻止其余成功条目生成图集，调用方查询失败 ID
            // 时会取得 全图后备 UV，并可结合此警告定位图集尺寸不足。
            XWARN("Failed to pack texture ID {} into atlas!",
                  m_pendingTextures[rects[i].id].id);
        }
    }

    // VKTexture 构造负责 staging 与图像布局转换；CPU 像素只需存活到构造返回。
    m_atlasTexture = std::make_unique<VKTexture>(atlasPixels.data(),
                                                 atlasSize,
                                                 atlasSize,
                                                 m_physDevice,
                                                 m_device,
                                                 m_pool,
                                                 m_queue);

    // 构建成功后释放全部 CPU 原图副本；UV 表和 GPU 纹理继续服务绘制查询。
    m_pendingTextures.clear();
    XDEBUG(
        "Texture Atlas built successfully. Size: {}x{}", atlasSize, atlasSize);
}

/// @brief 查询指定纹理在图集中的归一化矩形。
/// @param id 添加纹理时使用的 ID。
/// @return 命中时返回左上角与宽高；未命中时返回覆盖全图的后备矩形。
glm::vec4 VKTextureAtlas::getUV(uint32_t id) const
{
    auto it = m_uvRects.find(id);
    if ( it != m_uvRects.end() ) {
        return it->second;
    }
    // 全图 UV 同时适用于空图集创建的白色后备纹理。
    return glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
}

/// @brief 获取供 ImGui 绘图命令绑定的图集纹理标识。
/// @return 图集已构建时返回其 ImTextureID，否则返回空标识。
ImTextureID VKTextureAtlas::getImTextureID()
{
    if ( m_atlasTexture ) {
        return m_atlasTexture->getImTextureID();
    }
    return 0;
}

/// @brief 获取 VKTexture 为 ImGui 后端维护的描述符集。
/// @return 图集已构建时返回描述符集，否则返回空句柄。
vk::DescriptorSet VKTextureAtlas::getDescriptorSet() const
{
    if ( m_atlasTexture ) {
        return m_atlasTexture->getDescriptorSet();
    }
    return nullptr;
}

/// @brief 按本项目原生管线布局获取或创建图集描述符集。
/// @param pool 分配 descriptor set 的池。
/// @param layout 与原生管线绑定契约一致的 descriptor set layout。
/// @return 图集已构建时返回原生描述符集，否则返回空句柄。
vk::DescriptorSet VKTextureAtlas::getNativeDescriptorSet(
    vk::DescriptorPool pool, vk::DescriptorSetLayout layout)
{
    if ( m_atlasTexture ) {
        return m_atlasTexture->getNativeDescriptorSet(pool, layout);
    }
    return nullptr;
}

}  // namespace MMM::Graphic
