#include "ui/imgui/FileManagerUI.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include <utility>

namespace MMM::Graphic::UI
{

FileManagerUI::FileManagerUI(const std::string& name) : ITextureLoader(name) {}

FileManagerUI::~FileManagerUI() {}


void FileManagerUI::update()
{
    LayoutContext lctx{ m_layoutCtx, TR("title.FileManager") };
}

/// @brief 是否需要重载
bool FileManagerUI::needReload()
{
    // 仅加载一次
    return std::exchange(m_needReload, false);
}

/// @brief 重载纹理
void FileManagerUI::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                   vk::Device&         logicalDevice,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
}

}  // namespace MMM::Graphic::UI
