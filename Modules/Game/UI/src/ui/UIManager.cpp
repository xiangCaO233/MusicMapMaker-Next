#include "ui/UIManager.h"
#include "event/core/EventBus.h"
#include "event/input/translators/ImGuiTranslator.h"
#include "event/input/translators/UniversalCodepoint.h"
#include "event/ui/iwindow/UIWindowKeyEvent.h"
#include "event/ui/iwindow/UIWindowMouseEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "ui/IRenderableView.h"
#include "ui/ITextureLoader.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include <vector>

namespace MMM::UI
{

/// @brief 注册视图，转交所有权
void UIManager::registerView(const std::string&       name,
                             std::unique_ptr<IUIView> view)
{
    m_uiSequence.push_back(name);

    if ( view->renderable() ) {
        m_renderableUiSequence.push_back(name);
        XINFO("Registered Renderable [{}] UIView", name);
    } else {
        XINFO("Registered General [{}] UIView", name);
    }

    auto textureLoader = view->asTextureLoader();
    if ( textureLoader ) {
        m_textureLoaderSequence.push_back(name);
    }

    m_uiviews[name] = std::move(view);
}

/// @brief 注销并销毁视图
void UIManager::unregisterView(const std::string& name)
{
    /// @brief 即将被注销的视图迭代器。
    auto viewIt = m_uiviews.find(name);
    if ( viewIt != m_uiviews.end() ) {
        waitForGpuBeforeDestroyView(*viewIt->second);
        m_uiviews.erase(viewIt);
    }
    std::erase(m_uiSequence, name);
    std::erase(m_renderableUiSequence, name);
    std::erase(m_textureLoaderSequence, name);
    XINFO("Unregistered [{}] UIView", name);
}

/// @brief 清理所有ui
void UIManager::clearAllViews()
{
    /// @brief 当前仍注册在 UIManager 内的视图条目。
    for ( auto& entry : m_uiviews ) {
        waitForGpuBeforeDestroyView(*entry.second);
    }
    m_uiviews.clear();
}

/// @brief 准备资源
void UIManager::onPrepareResources(vk::PhysicalDevice&   physicalDevice,
                                   vk::Device&           logicalDevice,
                                   Graphic::VKSwapchain& swapchain,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
    // 检查并重建所有离屏帧缓冲
    for ( const auto& name : m_renderableUiSequence ) {
        auto renderableView = m_uiviews[name]->asRenderableView();
        if ( renderableView && renderableView->needReCreateFrameBuffer() ) {
            renderableView->reCreateFrameBuffer(
                physicalDevice, logicalDevice, swapchain, cmdPool, queue);
        }
    }

    // 检查并重载所有纹理
    for ( const auto& name : m_textureLoaderSequence ) {
        auto textureLoader = m_uiviews[name]->asTextureLoader();
        if ( textureLoader && textureLoader->needReload() ) {
            textureLoader->reloadTextures(
                physicalDevice, logicalDevice, cmdPool, queue);
        }
    }
}

/// @brief 更新ui
void UIManager::onUpdateUI()
{
    // 清理已关闭的 IUIView
    std::vector<std::string> toRemove;
    for ( auto& [name, view] : m_uiviews ) {
        if ( !view->isOpen() ) {
            toRemove.push_back(name);
        }
    }

    for ( const auto& name : toRemove ) {
        /// @brief 当前待销毁视图的迭代器。
        auto viewIt = m_uiviews.find(name);
        if ( viewIt != m_uiviews.end() ) {
            waitForGpuBeforeDestroyView(*viewIt->second);
            m_uiviews.erase(viewIt);
        }
        std::erase(m_uiSequence, name);
        // 同时也从纹理加载器和可渲染序列中移除（如果存在）
        std::erase(m_renderableUiSequence, name);
        std::erase(m_textureLoaderSequence, name);
    }

    // 派发 ImGui 事件 (每帧仅 1 次)
    DispatchGlobalUIEvents();

    // 按注册顺序更新本帧开始前已存在的 UI。
    // update() 过程中可能注册新视图，使用索引和名称副本避免迭代器失效。
    const size_t initialViewCount = m_uiSequence.size();
    for ( size_t i = 0; i < initialViewCount && i < m_uiSequence.size(); ++i ) {
        const std::string name = m_uiSequence[i];
        auto              it   = m_uiviews.find(name);
        if ( it == m_uiviews.end() ) {
            continue;
        }

        // 内部触发 ImGui 渲染和画笔收集
        it->second->update(this);
    }
}

/// @brief 录制所有离屏渲染指令
void UIManager::onRecordOffscreen(vk::CommandBuffer& cmd, uint32_t frameIndex)
{
    for ( const auto& name : m_renderableUiSequence ) {
        auto renderableView = m_uiviews[name]->asRenderableView();
        if ( renderableView ) {
            renderableView->recordCmds(cmd, frameIndex);
        }
    }
}


/// @brief 分派所有imgui事件
void UIManager::DispatchGlobalUIEvents()
{
    ImGuiIO& io = ImGui::GetIO();

    // 1. 获取当前正在被操作（Focus/Hover）的 ImGui 窗口名称
    // 这取代了原来在 IUIView 中写死的 m_name
    std::string focusedWindowName = "";
    std::string hoveredWindowName = "";

    if ( ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow ) {
        focusedWindowName = focused->Name;
    }
    if ( ImGuiWindow* hovered = ImGui::GetCurrentContext()->HoveredWindow ) {
        hoveredWindowName = hovered->Name;
    }

    // ==========================================
    // 处理鼠标事件 (每帧仅 1 次)
    // ==========================================
    // 只有当鼠标真的在 ImGui 窗口上时，才分发带窗口名字的事件
    if ( io.WantCaptureMouse ) {
        // 提取鼠标移动
        if ( io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ) {
            Event::UIWindowMouseMoveEvent e;
            e.uiManager    = this;
            e.sourceUiName = hoveredWindowName;  // 鼠标事件归属悬停窗口
            e.pos          = { io.MousePos.x, io.MousePos.y };
            e.delta        = { io.MouseDelta.x, io.MouseDelta.y };
            Event::EventBus::instance().publish(e);
        }

        // 提取滚轮
        if ( io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ) {
            Event::UIWindowMouseScrollEvent e;
            e.uiManager    = this;
            e.sourceUiName = hoveredWindowName;
            e.pos          = { io.MousePos.x, io.MousePos.y };
            e.offset       = { io.MouseWheelH, io.MouseWheel };
            Event::EventBus::instance().publish(e);
        }

        // 提取点击
        for ( int i = 0; i < 5; ++i ) {
            bool pressed  = ImGui::IsMouseClicked(i);
            bool released = ImGui::IsMouseReleased(i);
            if ( pressed || released ) {
                Event::UIWindowMouseButtonEvent e;
                e.uiManager    = this;
                e.sourceUiName = hoveredWindowName;
                e.button       = Event::Translator::ImGui::GetMouseButton(i);
                e.action       = pressed ? Event::Input::Action::Press
                                         : Event::Input::Action::Release;
                e.mods         = Event::Translator::ImGui::GetMods();
                e.pos          = { io.MousePos.x, io.MousePos.y };
                Event::EventBus::instance().publish(e);
            }
        }
    }

    // ==========================================
    // 处理键盘事件 (每帧仅 1 次)
    // ==========================================
    if ( io.WantCaptureKeyboard ) {
        // 全局遍历一次枚举 (约 100 次循环，对 CPU 来说仅需几纳秒，完全不拉胯)
        for ( int i = ImGuiKey_NamedKey_BEGIN; i < ImGuiKey_NamedKey_END;
              ++i ) {
            ImGuiKey key = static_cast<ImGuiKey>(i);

            bool pressed  = ImGui::IsKeyPressed(key, true);
            bool released = ImGui::IsKeyReleased(key);

            if ( pressed || released ) {
                Event::UIWindowKeyPressEvent e;
                e.uiManager    = this;
                e.sourceUiName = focusedWindowName;  // 键盘事件归属焦点窗口

                e.key      = Event::Translator::ImGui::GetKey(key);
                e.action   = pressed ? Event::Input::Action::Press
                                     : Event::Input::Action::Release;
                e.mods     = Event::Translator::ImGui::GetMods();
                e.scancode = 0;

                if ( e.action != Event::Input::Action::Release ) {
                    e.codepoint =
                        Event::Translator::ResolveCodepoint(e.key, e.mods);
                } else {
                    e.codepoint = 0;
                }

                Event::EventBus::instance().publish(e);
            }
        }
    }
}

/// @brief 在销毁可能持有 Vulkan 资源的视图前等待 GPU 完成在途命令。
void UIManager::waitForGpuBeforeDestroyView(IUIView& view)
{
    /// @brief 目标视图是否持有可能被命令缓冲引用的 Vulkan 资源。
    bool ownsGpuResources = view.renderable() || view.asTextureLoader();
    if ( !ownsGpuResources ) {
        return;
    }

    /// @brief 当前 Vulkan 上下文查询结果。
    auto contextResult = Graphic::VKContext::get();
    if ( !contextResult ) {
        XWARN("UIManager: skip GPU idle wait before destroying [{}]: {}",
              view.m_name,
              contextResult.error());
        return;
    }

    (void)contextResult->get().getLogicalDevice().waitIdle();
}

}  // namespace MMM::UI
