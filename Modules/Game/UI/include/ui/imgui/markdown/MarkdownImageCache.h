#pragma once

#include "ui/ITextureLoader.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::UI
{
/// @brief 后台生成的有界图片图集，不包含 GPU 所有权。
struct MarkdownImagePixels {
    std::vector<unsigned char> pixels;                          ///< RGBA 图集。
    unsigned                   width{}, height{};               ///< 图集尺寸。
    unsigned frameWidth{}, frameHeight{}, columns{}, frames{};  ///< 帧布局。
    double   duration{};  ///< 动画循环时长，静态图为零。
};
/// @brief 解析更新站点相对图片地址；只允许 HTTP(S)，不接受本地文件。
std::string resolveUpdateImageUrl(std::string_view destination);
/// @brief 下载并逐帧解码更新图片，生成尺寸受限的图集。
/// @warning 低频后台任务：执行网络和临时文件 I/O；禁止在 UI 热路径调用。
MarkdownImagePixels loadUpdateImage(const std::string& url);
/// @brief 解码有界的图片字节；GIF 逐帧生成缩略图，不展开全尺寸动画。
/// @warning 仅后台任务或测试调用，GIF 由 FFmpeg 从任务专属临时文件逐帧读取。
MarkdownImagePixels decodeUpdateImage(std::span<const unsigned char> bytes);

/// @brief 更新日志专用图片缓存，生命周期由 UIManager 管理。
class MarkdownImageCache final : public ITextureLoader, public IMarkdownImages
{
public:
    /// @brief 创建空缓存，不立即启动网络任务。
    MarkdownImageCache();
    /// @brief 释放缓存；后台任务不捕获本对象，UIManager 保证 GPU 已停止使用。
    ~MarkdownImageCache() override;
    /// @brief 为 UIManager 提供完整对象地址，不依赖多重继承的基址布局。
    void* getActualInstance() override { return this; }
    /// @brief 新文档到达时扫描图片并加入去重队列，不执行网络操作。
    void prepareDocument(std::string_view markdown);
    /// @brief 仅查询纹理与动画帧，禁止热路径 I/O 或所有权复制。
    MarkdownImage findImage(std::string_view destination) const override;
    /// @brief 缓存没有独立窗口，纹理生命周期由资源准备阶段推进。
    void update(UIManager*) override {}
    /// @brief 非阻塞轮询单个后台任务，避免并行解码占满内存。
    bool needReload() override;
    /// @brief 在低频资源准备阶段上传已解码的图集。
    /// @warning 仅在新图片到达时分配并上传纹理，沿用 VKTexture 必需上传同步。
    void reloadTextures(vk::PhysicalDevice&, vk::Device&, vk::CommandPool&,
                        vk::Queue&) override;

private:
    struct Impl;                   ///< 私有任务、去重索引和纹理所有权。
    std::unique_ptr<Impl> m_impl;  ///< 稳定的缓存状态。
};
}  // namespace MMM::UI
