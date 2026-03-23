#pragma once

#include "ui/IUIView.h"
#include "vulkan/vulkan.hpp"

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

private:
};

}  // namespace MMM::Graphic::UI
