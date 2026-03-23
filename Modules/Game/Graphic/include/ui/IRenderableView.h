#pragma once

#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "ui/ITextureLoader.h"
#include "ui/brush/Brush.h"

namespace MMM::Graphic
{
namespace UI
{
class Brush;
class IRenderableView : public ITextureLoader, public VKOffScreenRenderer
{
public:
    IRenderableView(const std::string& name) : ITextureLoader(name) {}
    IRenderableView(IRenderableView&&)                 = delete;
    IRenderableView(const IRenderableView&)            = delete;
    IRenderableView& operator=(IRenderableView&&)      = delete;
    IRenderableView& operator=(const IRenderableView&) = delete;
    virtual ~IRenderableView() override                = default;

    ///@brief 获取笔刷
    const Brush& getBrush() const { return m_brush; }

    ///@brief 是否可渲染
    bool renderable() override { return true; }

    ///@brief 是否需要重新记录命令 (比如数据变了)
    virtual bool isDirty() const = 0;

    class RenderContext
    {
    public:
        RenderContext(IRenderableView* view, const char* window_title,
                      int width, int height)
            : m_view(view), m_width(width), m_height(height)
        {
            // 1. 在 Begin 之前设置窗口大小
            // ImGuiCond_FirstUseEver: 只在第一次运行且没有 .ini
            // 配置文件记录时生效 ImGuiCond_Always:
            // 强制每一帧都固定这个大小（用户无法拖拽改变大小） ImGuiCond_Once:
            // 每轮启动只设置一次
            ImGui::SetNextWindowSize(ImVec2((float)m_width, (float)m_height),
                                     ImGuiCond_FirstUseEver);

            ImGui::Begin(window_title);


            // 1. 获取 ImGui 窗口分配给内容的实际大小
            ImVec2 size = ImGui::GetContentRegionAvail();
            if ( size.x > 0 && size.y > 0 ) {
                // 仅仅是发送请求，不会立刻改变渲染状态
                view->setTargetSize(static_cast<uint32_t>(size.x),
                                    static_cast<uint32_t>(size.y));
            }

            // 1. 必须清空上一帧的顶点
            view->m_brush.clear();
        }

        ~RenderContext()
        {
            ImVec2 size = ImGui::GetContentRegionAvail();
            if ( size.x > 0 && size.y > 0 ) {
                vk::DescriptorSet texID = m_view->getDescriptorSet();
                // 增加判空，防止在重构瞬间崩溃
                if ( texID != VK_NULL_HANDLE ) {
                    ImGui::Image((ImTextureID)(VkDescriptorSet)texID, size);
                } else {
                    ImGui::Text("%s", TR("Loading Surface...").data());
                }
                ImGui::End();
            }
        };

    private:
        IRenderableView* m_view;
        int              m_width;
        int              m_height;
    };

protected:
    Brush m_brush;
};
}  // namespace UI
}  // namespace MMM::Graphic
