#pragma once

#include "graphic/imguivk/VKTexture.h"
#include "ui/IUIView.h"
#include "vulkan/vulkan.hpp"
#include <filesystem>
#include <lunasvg.h>

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
        vk::PhysicalDevice& pd, vk::Device& ld, vk::CommandPool& cp,
        vk::Queue&                          q,
        std::optional<std::array<float, 4>> overrideColor = std::nullopt)
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

            // --- 核心：修改目标颜色 ---
            if ( overrideColor.has_value() ) {
                uint8_t* pixels = bitmap.data();
                auto&    col    = overrideColor.value();

                // 将浮点颜色转为 0-255 字节
                uint8_t targetR = static_cast<uint8_t>(
                    std::clamp(col[0] * 255.0f, 0.0f, 255.0f));
                uint8_t targetG = static_cast<uint8_t>(
                    std::clamp(col[1] * 255.0f, 0.0f, 255.0f));
                uint8_t targetB = static_cast<uint8_t>(
                    std::clamp(col[2] * 255.0f, 0.0f, 255.0f));
                // 如果你还想覆盖透明度，可以拿 col[3]

                for ( uint32_t i = 0; i < targetSize * targetSize; ++i ) {
                    // pixels[i*4 + 3] 是 Alpha 通道
                    // 我们只修改有颜色的部分（或者直接全部替换 RGB，保留 A）
                    pixels[i * 4 + 0] = targetR;
                    pixels[i * 4 + 1] = targetG;
                    pixels[i * 4 + 2] = targetB;
                    // 如果你想让图标整体变淡，可以 pixels[i * 4 + 3] *= col[3];
                }
            }

            return std::make_unique<Graphic::VKTexture>(
                bitmap.data(), targetSize, targetSize, pd, ld, cp, q);
        }

        // 2. 处理常规位图
        return std::make_unique<Graphic::VKTexture>(
            path.string(), pd, ld, cp, q);
    }


private:
};

}  // namespace MMM::UI
