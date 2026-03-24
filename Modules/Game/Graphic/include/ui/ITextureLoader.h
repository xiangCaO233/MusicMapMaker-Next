#pragma once

#include "graphic/imguivk/VKTexture.h"
#include "ui/IUIView.h"
#include "vulkan/vulkan.hpp"
#include <filesystem>
#include <lunasvg.h>

namespace MMM::Graphic::UI
{

class ITextureLoader : public IUIView
{
public:
    ITextureLoader(const std::string& name) : IUIView(name) {};
    ITextureLoader(ITextureLoader&&)                 = default;
    ITextureLoader(const ITextureLoader&)            = default;
    ITextureLoader& operator=(ITextureLoader&&)      = delete;
    ITextureLoader& operator=(const ITextureLoader&) = delete;

    virtual ~ITextureLoader() override = default;

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
     */
    std::unique_ptr<VKTexture> loadTextureResource(
        const std::filesystem::path& path, uint32_t targetSize,
        vk::PhysicalDevice& pd, vk::Device& ld, vk::CommandPool& cp,
        vk::Queue& q)
    {
        if ( !std::filesystem::exists(path) ) {
            XWARN("Texture path not found: {}", path.string());
            return nullptr;
        }

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        // 1. 处理 SVG
        if ( ext == ".svg" ) {
            auto doc = lunasvg::Document::loadFromFile(path.string());
            if ( !doc ) {
                XWARN("lunasvg failed to load: {}", path.string());
                return nullptr;
            }

            auto bitmap = doc->renderToBitmap(targetSize, targetSize);
            bitmap.convertToRGBA();

            return std::make_unique<VKTexture>(
                bitmap.data(), targetSize, targetSize, pd, ld, cp, q);
        }

        // 2. 处理常规位图 (PNG, JPG 等)
        // 假设 VKTexture 有构造函数直接接受路径并使用 stb_image 加载
        return std::make_unique<VKTexture>(path.string(), pd, ld, cp, q);
    }


private:
};

}  // namespace MMM::Graphic::UI
