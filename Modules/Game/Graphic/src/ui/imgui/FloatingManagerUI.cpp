#include "ui/imgui/FloatingManagerUI.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "log/colorful-log.h"
#include "ui/ISubView.h"
#include <utility>

namespace MMM::Graphic::UI
{

FloatingManagerUI::FloatingManagerUI(const std::string& name)
    : ITextureLoader(name)
{
    // 订阅切换事件
    m_subId = MMM::Event::EventBus::instance()
                  .subscribe<MMM::Event::UISubViewToggleEvent>(
                      [this](const MMM::Event::UISubViewToggleEvent& e) {
                          XINFO(
                              "FloatingManagerUI get event, targetName:{}, "
                              "targetSubViewId:{}",
                              e.targetFloatManagerName,
                              e.subViewId);
                          // 核心逻辑：只处理发给“我”的指令
                          if ( e.targetFloatManagerName == this->m_name ) {
                              this->toggleSubView(e.subViewId);
                          }
                      });
}

FloatingManagerUI::~FloatingManagerUI()
{
    MMM::Event::EventBus::instance()
        .unsubscribe<MMM::Event::UISubViewToggleEvent>(m_subId);
}

///@brief 注册子视图到这个管理器
void FloatingManagerUI::registerSubView(const std::string&        subViewId,
                                        std::unique_ptr<ISubView> subView)
{
    m_subViews[subViewId] = std::move(subView);
    toggleSubView(subViewId);
}

///@brief 核心切换逻辑
void FloatingManagerUI::toggleSubView(const std::string& subViewId)
{
    if ( m_currentSubViewId == subViewId && m_isVisible ) {
        m_isVisible = false;  // 已激活再点击 -> 隐藏
    } else {
        m_currentSubViewId = subViewId;
        m_isVisible        = true;  // 切换或显示
    }
}

void FloatingManagerUI::update()
{
    if ( !m_isVisible ) return;

    LayoutContext lctx{ m_layoutCtx, m_currentSubViewId };
    if ( m_subViews.contains(m_currentSubViewId) ) {
        m_subViews[m_currentSubViewId]->onUpdate(lctx);
    }
}

/// @brief 是否需要重载
bool FloatingManagerUI::needReload()
{
    // 仅加载一次
    return std::exchange(m_needReload, false);
}

/// @brief 重载纹理
void FloatingManagerUI::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                       vk::Device&         logicalDevice,
                                       vk::CommandPool&    cmdPool,
                                       vk::Queue&          queue)
{
}

}  // namespace MMM::Graphic::UI
