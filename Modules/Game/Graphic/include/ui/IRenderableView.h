#pragma once

#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "ui/brush/Brush.h"
#include "vulkan/vulkan.hpp"

namespace MMM::Graphic
{
namespace UI
{
class Brush;
class IRenderableView : public VKOffScreenRenderer
{
public:
    IRenderableView()
    {
        // 默认创建一个离屏渲染器
        m_offscreen = std::make_unique<Graphic::VKOffScreenRenderer>();
    }
    IRenderableView(IRenderableView&&)                 = delete;
    IRenderableView(const IRenderableView&)            = delete;
    IRenderableView& operator=(IRenderableView&&)      = delete;
    IRenderableView& operator=(const IRenderableView&) = delete;
    ~IRenderableView()                                 = default;

    ///@brief 获取离屏渲染器接口
    VKOffScreenRenderer& getOffscreenRenderer() { return *m_offscreen; }

    ///@brief 供 UI 层获取离屏渲染的结果纹理 (用于 ImGui::Image)
    vk::DescriptorSet getTextureDescriptor()
    {
        return m_offscreen->getDescriptorSet();
    }

    ///@brief 获取笔刷
    const Brush& getBrush() const { return m_brush; }

    ///@brief 是否需要重新记录命令 (比如数据变了)
    virtual bool isDirty() const = 0;

protected:
    Brush m_brush;

private:
    std::unique_ptr<Graphic::VKOffScreenRenderer> m_offscreen;
};
}  // namespace UI
}  // namespace MMM::Graphic
