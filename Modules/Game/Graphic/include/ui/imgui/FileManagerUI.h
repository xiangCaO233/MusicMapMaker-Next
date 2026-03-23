#pragma once

#include "ui/ITextureLoader.h"
#include <string>

namespace MMM::Graphic::UI
{

class FileManagerUI : public ITextureLoader
{
public:
    FileManagerUI(const std::string& name);
    FileManagerUI(FileManagerUI&&)                 = default;
    FileManagerUI(const FileManagerUI&)            = default;
    FileManagerUI& operator=(FileManagerUI&&)      = delete;
    FileManagerUI& operator=(const FileManagerUI&) = delete;
    ~FileManagerUI() override;

    void update() override;

    /// @brief 是否需要重载
    bool needReload() override;

    /// @brief 重载纹理
    virtual void reloadTextures(vk::PhysicalDevice& physicalDevice,
                                vk::Device&         logicalDevice,
                                vk::CommandPool&    cmdPool,
                                vk::Queue&          queue) override;

private:
    bool m_needReload{ true };
};

}  // namespace MMM::Graphic::UI
