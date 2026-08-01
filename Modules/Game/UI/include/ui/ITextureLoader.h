#pragma once

#include "ui/IUIView.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{
class VKTexture;
}

namespace MMM::UI
{

class ITextureLoader : virtual public IUIView
{
public:
    ITextureLoader(const std::string& name) : IUIView(name) {};
    ITextureLoader(ITextureLoader&&)                 = default;
    ITextureLoader(const ITextureLoader&)            = default;
    ITextureLoader& operator=(ITextureLoader&&)      = delete;
    ITextureLoader& operator=(const ITextureLoader&) = delete;

    virtual ~ITextureLoader() override = default;

    /// @brief 获取视图具体类型,替代 dynamic_cast
    ViewType getViewType() const override { return ViewType::TextureLoader; }

    /// @brief 安全转换为自身
    ITextureLoader* asTextureLoader() override { return this; }

    void* getActualInstance() override { return this; }

    /// @brief 是否需要重载
    virtual bool needReload() = 0;

    /// @brief 重载纹理
    virtual void reloadTextures(vk::PhysicalDevice& physicalDevice,
                                vk::Device&         logicalDevice,
                                vk::CommandPool& cmdPool, vk::Queue& queue) = 0;

protected:
    /**
     * @brief 公用接口：从文件路径加载纹理（自动识别 SVG 或位图）
     * @param path 文件路径
     * @param targetSize 如果是 SVG，栅格化的目标尺寸
     * @param overrideColor 可选：如果提供，SVG
     * 的所有非透明像素将被替换为此颜色 (RGB 范围 0.0~1.0)
     */
    std::unique_ptr<Graphic::VKTexture> loadTextureResource(
        const std::filesystem::path& path, uint32_t targetSize,
        vk::PhysicalDevice& physicalDevice, vk::Device& logicalDevice,
        vk::CommandPool& commandPool, vk::Queue& queue,
        std::optional<std::array<float, 4>> overrideColor = std::nullopt);
};

}  // namespace MMM::UI
