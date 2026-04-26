#pragma once

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "ui/ITextureLoader.h"
#include "ui/IUIView.h"
#include "ui/brush/Brush.h"

namespace MMM::UI
{
class Brush;
class IRenderableView : public ITextureLoader,
                        public Graphic::VKOffScreenRenderer
{
public:
    IRenderableView(const std::string& name)
        : IUIView(name), ITextureLoader(name)
    {
    }

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
            ImGui::SetNextWindowSize(ImVec2((float)m_width, (float)m_height),
                                     ImGuiCond_FirstUseEver);

            // 应用窗口标题字体
            auto&   skinMgr   = Config::SkinManager::instance();
            ImFont* titleFont = skinMgr.getFont("title");
            if ( titleFont ) ImGui::PushFont(titleFont);

            ImGui::Begin(window_title);

            // Begin 后立即弹出，确保后续内容使用默认字体
            if ( titleFont ) ImGui::PopFont();


            // 1. 获取 ImGui 窗口分配给内容的实际大小
            ImVec2 size     = ImGui::GetContentRegionAvail();
            float  dpiScale = Config::AppConfig::instance().getWindowContentScale();
            if ( size.x > 0 && size.y > 0 ) {
                // 核心修复：通知渲染器目标尺寸及其缩放倍率
                view->setTargetSize(static_cast<uint32_t>(size.x),
                                    static_cast<uint32_t>(size.y),
                                    dpiScale);
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
            }
            ImGui::End();
        };

    private:
        IRenderableView* m_view;
        int              m_width;
        int              m_height;
    };

protected:
    Brush m_brush;

    // --- 获取数据供 Vulkan 使用 ---
    const std::vector<Graphic::Vertex::VKBasicVertex>&
    getVertices() const override
    {
        return m_brush.getVertices();
    }

    const std::vector<uint32_t>& getIndices() const override
    {
        return m_brush.getIndices();
    };

    /**
     * @brief 录制具体的绘制指令 (由 UI 层实现)
     */
    void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override
    {
        for ( const auto& cmd : m_brush.getCmds() ) {
            if ( cmd.texture != VK_NULL_HANDLE ) {
                cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                          pipelineLayout,
                                          0,
                                          1,
                                          &cmd.texture,
                                          0,
                                          nullptr);
            } else {
                cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                          pipelineLayout,
                                          0,
                                          1,
                                          &defaultDescriptor,
                                          0,
                                          nullptr);
            }
            cmdBuf.drawIndexed(
                cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
        }
    }
};
}  // namespace MMM::UI
