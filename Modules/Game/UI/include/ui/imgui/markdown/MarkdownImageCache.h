#pragma once

#include "ui/ITextureLoader.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include <cstddef>
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
/// @param animatedEdge 动画帧最长边；放大预览可请求高清图集。
/// @warning 低频后台任务：执行网络和临时文件 I/O；禁止在 UI 热路径调用。
MarkdownImagePixels loadUpdateImage(const std::string& url,
                                    unsigned           animatedEdge = 320U);
/// @brief 解码有界的图片字节；GIF 按请求分辨率采样为有界图集。
/// @param sourceFrameBudget 源帧展开预算；较大的值仅供打包的可信教程资源使用。
/// @warning 仅后台任务或测试调用；内存超限时才尝试逐帧视频解码。
MarkdownImagePixels decodeUpdateImage(
    std::span<const unsigned char> bytes, unsigned animatedEdge = 320U,
    std::size_t sourceFrameBudget = 128U * 1024U * 1024U);

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
    /// @brief 排入单张远程图片或打包的教程 GIF，不读取文件。
    void prepareImage(std::string_view destination);
    /// @brief 为更新日志的放大弹窗单独排入高清图集。
    /// @warning 低频 UI 操作：只排队；网络与解码由后台任务完成。
    void preparePreviewImage(std::string_view destination);
    /// @brief 放大弹窗关闭后安排安全释放高清纹理，归还正文缓存预算。
    void releasePreviewImage();
    /// @brief 结束教学时安排释放当前教程 GIF 的纹理和缓存条目。
    void releaseWalkthroughImage();
    /// @brief 已上传的 GIF 从当前教学阶段开始重新计时，不重建纹理。
    void restartImage(std::string_view destination);
    /// @brief 仅查询纹理与动画帧，禁止热路径 I/O 或所有权复制。
    MarkdownImage findImage(std::string_view destination) const override;
    /// @brief 优先显示已就绪的高清帧，准备期间沿用原缩略图。
    MarkdownImage findPreviewImage(std::string_view destination) const;
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
