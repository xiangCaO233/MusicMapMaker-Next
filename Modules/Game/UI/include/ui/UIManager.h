#pragma once

#include "IUIView.h"
#include "graphic/imguivk/IGraphicUserHook.h"
#include "ui/layout/CLayWrapperCore.h"
#include <memory>
#include <unordered_map>
#include <vector>

namespace MMM::UI
{
class IRenderableView;
class ITextureLoader;
class UIManager : public MMM::Graphic::IGraphicUserHook
{
public:
    UIManager()
    {
        // 初始化CLay
        CLayWrapperCore::instance().setupClayTextMeasurement();
    }
    UIManager(UIManager&&)                 = delete;
    UIManager(const UIManager&)            = delete;
    UIManager& operator=(UIManager&&)      = delete;
    UIManager& operator=(const UIManager&) = delete;
    ~UIManager()                           = default;

    /// @brief 注册视图，转交所有权
    void registerView(const std::string& name, std::unique_ptr<IUIView> view);

    /// @brief 清理所有ui
    void clearAllViews();

    /// @brief 泛型获取裸指针 外部不负责销毁
    template<typename T> T* getView(const std::string& name)
    {
        auto it = m_uiviews.find(name);
        if ( it != m_uiviews.end() ) {
            return dynamic_cast<T*>(it->second.get());
        }
        return nullptr;
    }


    /// @brief 分派所有imgui事件
    void DispatchGlobalUIEvents();

    /// @brief 准备资源
    void onPrepareResources(vk::PhysicalDevice&   physicalDevice,
                            vk::Device&           logicalDevice,
                            Graphic::VKSwapchain& swapchain,
                            vk::CommandPool&      cmdPool,
                            vk::Queue&            queue) override;

    /// @brief 更新ui
    void onUpdateUI() override;

    /// @brief 录制所有离屏渲染指令
    void onRecordOffscreen(vk::CommandBuffer& cmd, uint32_t frameIndex) override;

private:
    /// @brief 所有ui接口
    std::unordered_map<std::string, std::unique_ptr<IUIView>> m_uiviews;

    /// @brief ui接口注册顺序
    std::vector<std::string> m_uiSequence;

    /// @brief 可再渲染ui接口注册顺序
    std::vector<std::string> m_renderableUiSequence;

    /// @brief 纹理加载器接口注册顺序
    std::vector<std::string> m_textureLoaderSequence;
};
}  // namespace MMM::UI
