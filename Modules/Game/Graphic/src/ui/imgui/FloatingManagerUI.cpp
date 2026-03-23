#include "ui/imgui/FloatingManagerUI.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include <utility>

namespace MMM::Graphic::UI
{

FlotingManagerUI::FlotingManagerUI(const std::string& name)
    : ITextureLoader(name)
{
}

FlotingManagerUI::~FlotingManagerUI() {}

/// @brief 设置窗口标题
void FlotingManagerUI::set_window_title(const std::string& name)
{
    m_name = name;
}

void FlotingManagerUI::update()
{
    LayoutContext lctx{ m_layoutCtx, TR(m_name.c_str()) };
}

/// @brief 是否需要重载
bool FlotingManagerUI::needReload()
{
    // 仅加载一次
    return std::exchange(m_needReload, false);
}

/// @brief 重载纹理
void FlotingManagerUI::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                      vk::Device&         logicalDevice,
                                      vk::CommandPool&    cmdPool,
                                      vk::Queue&          queue)
{
}

}  // namespace MMM::Graphic::UI
