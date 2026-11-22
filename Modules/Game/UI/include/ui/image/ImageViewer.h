#pragma once

#include "ui/ITextureLoader.h"
#include "ui/image/ImageRegion.h"

#include <imgui.h>

#include <filesystem>
#include <memory>
#include <string_view>

namespace MMM::UI
{
class UIManager;
struct BeatmapScreenshotImage;

/// @brief 文件查看页的稳定身份，供各平台停靠及关闭交互识别。
inline constexpr char FILE_IMAGE_TAB_ID[] = "FileImageViewer";

/// @brief 判断项目文件能否通过查看器打开，比较大小写不敏感的扩展名。
/// @warning 菜单和文件行可见时调用，仅比较字符串，不打开文件。
[[nodiscard]] bool isImageViewerExtension(std::string_view extension);

/// @brief 项目图片与导出预览共用的图片查看器。
/// @details 窗口状态留在 UI 线程；磁盘解码及可见区采样在后台完成。
/// 文件一旦关闭即释放原始像素，纹理由 UIManager 的资源阶段回收。
class ImageViewer final : public ITextureLoader
{
public:
    /// @brief 创建空查看器，视图 ID 区分工程文件与导出预览的生命周期。
    explicit ImageViewer(const char* viewId);

    /// @brief 获取并按需注册供项目资源管理器复用的图片标签页。
    /// @warning 低频打开入口；不能在每帧无条件注册。
    [[nodiscard]] static ImageViewer& fileTab(UIManager& manager);

    /// @brief 获取导出预览所用的查看器，独立于已打开的文件标签页。
    [[nodiscard]] static ImageViewer& exportPopup(UIManager& manager);

    /// @brief 关闭并回收工作线程，UIManager 负责 GPU 资源的销毁时序。
    ~ImageViewer() override;

    /// @brief 返回实际视图地址，供禁用 RTTI 的 UIManager 获取实例。
    void* getActualInstance() override { return this; }

    /// @brief 打开本地图片并聚焦中央标签页；再次点击同一文件时保留浏览位置。
    void openFile(std::filesystem::path path);

    /// @brief 在导出模态内打开大图，沿用已准备的成品批次与概览。
    /// @param image 可在后台独立取样的正式导出批次。
    /// @param imageSize 完整成品范围。
    /// @param thumbnail 导出窗口持有的缩略纹理，直到关闭此子模态才可释放。
    /// @param thumbnailSize 缩略纹理的实际像素尺寸。
    void openScreenshot(std::shared_ptr<const BeatmapScreenshotImage> image,
                        ImageRegion imageSize, ImTextureID thumbnail,
                        ImVec2 thumbnailSize);

    /// @brief 从导出窗口绘制子模态；ImGui 要求从同一个父窗口打开和绘制。
    /// @warning 每帧只推进已打开的查看器。
    void drawScreenshotPopup(float dpiScale);

    /// @brief 父窗口关闭或选项失效时撤销子模态使用的所有输入。
    void closeScreenshot();

    /// @brief 绘制中央停靠区中的文件标签页。
    /// @warning UI 热路径，后台未完成时立即返回或绘制加载状态。
    void update(UIManager* manager) override;

    /// @brief 检查是否有待上传图像或关闭后待回收的纹理。
    /// @warning 每帧只检查任务发布状态。
    bool needReload() override;

    /// @brief 在 UIManager 资源阶段上传可见区或清理过期纹理。
    /// @warning 仅上传或清理时进队列同步；绘制与拖拽不直接等待 GPU。
    void reloadTextures(vk::PhysicalDevice& physical, vk::Device& device,
                        vk::CommandPool& pool, vk::Queue& queue) override;

private:
    /// @brief 按来源 ID 查找或注册一个查看器，资源回收仍由 UIManager 负责。
    [[nodiscard]] static ImageViewer& get(UIManager&  manager,
                                          const char* viewId);

    struct Impl;
    /// @brief 独占后台任务和图形资源，避免头文件暴露编解码器。
    std::unique_ptr<Impl> m_impl;
};
}  // namespace MMM::UI
