#pragma once

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "ui/ITextureLoader.h"
#include "ui/IUIView.h"
#include "ui/brush/Brush.h"

#include <algorithm>
#include <cmath>

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

    /// @brief 获取视图具体类型,替代 dynamic_cast
    ViewType getViewType() const override { return ViewType::RenderableView; }

    /// @brief 安全转换为自身
    ITextureLoader*  asTextureLoader() override { return this; }
    IRenderableView* asRenderableView() override { return this; }

    void* getActualInstance() override { return this; }

    ///@brief 获取笔刷
    const Brush& getBrush() const { return m_brush; }

    ///@brief 是否可渲染
    bool renderable() override { return true; }

    ///@brief 是否需要重新记录命令 (比如数据变了)
    virtual bool isDirty() const = 0;

    /// @brief 当前帧是否需要录制离屏渲染命令。
    /// @return 需要录制时返回 true。
    /// @warning 渲染热路径：UIManager 每帧命令录制前读取；默认复用
    /// isDirty()，派生类可用可见性状态进一步裁剪。
    virtual bool shouldRecordOffscreen() const { return isDirty(); }

    class RenderContext
    {
    public:
        /// @brief 创建渲染窗口，并可为右侧辅助栏预留独立内容宽度。
        /// @param view 目标可渲染视图。
        /// @param window_title ImGui 窗口标题和 ID。
        /// @param width 首次显示使用的窗口宽度。
        /// @param height 首次显示使用的窗口高度。
        /// @param p_open 可选窗口开启状态。
        /// @param rightReservedWidth 右侧辅助栏期望宽度，单位为逻辑像素。
        /// @param minRenderableWidth 预留辅助栏后必须保留的最小画布宽度。
        /// @warning UI 热路径：每帧窗口绘制调用，只执行常量级布局计算、样式
        /// 栈操作和目标尺寸同步。
        RenderContext(IRenderableView* view, const char* window_title,
                      int width, int height, bool* p_open = nullptr,
                      float rightReservedWidth = 0.0f,
                      float minRenderableWidth = 1.0f)
            : m_view(view), m_width(width), m_height(height)
        {
            // 1. 在 Begin 之前设置窗口大小
            ImGui::SetNextWindowSize(ImVec2((float)m_width, (float)m_height),
                                     ImGuiCond_FirstUseEver);

            // 在 Begin 之前设置圆角
            float dpiScale =
                Config::AppConfig::instance().getWindowContentScale();
            float windowRound = std::floor(8.0f * dpiScale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);

            // 应用窗口标题字体
            auto&   skinMgr   = Config::SkinManager::instance();
            ImFont* titleFont = skinMgr.getFont("title");
            if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

            const bool wasOpenBeforeBegin = p_open != nullptr && *p_open;
            ImGui::Begin(window_title, p_open);
            FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, p_open);

            // Begin 后立即弹出，确保后续内容使用默认字体
            if ( titleFont ) ImGui::PopFont();


            // 1. 获取 ImGui 窗口分配给内容的实际大小
            ImVec2      size = ImGui::GetContentRegionAvail();
            const float safeMinRenderableWidth =
                std::max(1.0f, minRenderableWidth);
            m_reservedRightWidth =
                std::clamp(rightReservedWidth,
                           0.0f,
                           std::max(0.0f, size.x - safeMinRenderableWidth));
            m_renderSize =
                ImVec2(std::max(0.0f, size.x - m_reservedRightWidth), size.y);
            if ( size.x > 0 && size.y > 0 ) {
                // 核心修复：通知渲染器目标尺寸及其缩放倍率
                view->setTargetSize(static_cast<uint32_t>(m_renderSize.x),
                                    static_cast<uint32_t>(m_renderSize.y),
                                    dpiScale);
            }

            // 1. 必须清空上一帧的顶点
            view->m_brush.clear();
        }

        /// @brief 绘制当前帧离屏纹理并结束对应 ImGui 窗口。
        /// @warning UI 热路径：每帧只提交一个 Image 或加载提示并恢复样式栈。
        ~RenderContext()
        {
            const ImVec2 size = m_renderSize;
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
            ImGui::PopStyleVar();
        };

        /// @brief 获取扣除右侧辅助栏后的画布逻辑尺寸。
        /// @return 当前帧用于纹理和交互映射的画布尺寸。
        /// @warning UI 热路径：只返回构造时缓存的尺寸。
        ImVec2 getRenderSize() const { return m_renderSize; }

        /// @brief 获取当前帧实际预留的右侧辅助栏宽度。
        /// @return 经过窄窗口限制后的逻辑像素宽度。
        /// @warning UI 热路径：只返回构造时缓存的宽度。
        float getReservedRightWidth() const { return m_reservedRightWidth; }

    private:
        IRenderableView* m_view;
        int              m_width;
        int              m_height;
        /// @brief 当前帧扣除辅助栏后的画布逻辑尺寸。
        ImVec2 m_renderSize{ 0.0f, 0.0f };
        /// @brief 当前帧实际预留的右侧辅助栏宽度。
        float m_reservedRightWidth{ 0.0f };
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
     * @warning 热路径：每帧离屏命令录制时执行；只遍历 Brush
     * 已缓存命令，禁止资源加载和阻塞等待。
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
