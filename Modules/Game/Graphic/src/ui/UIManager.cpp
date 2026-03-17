#include "ui/UIManager.h"
#include "log/colorful-log.h"
#include "ui/IRenderableView.h"
#include <vector>

namespace MMM::Graphic::UI
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
    m_uiviews[name] = std::move(view);
}

/// @brief 获取可再渲染视图
std::vector<IRenderableView*> UIManager::getRenderableViews()
{
    std::vector<IRenderableView*> renderableViews;
    for ( const auto& name : m_renderableUiSequence ) {
        renderableViews.push_back(
            static_cast<IRenderableView*>(m_uiviews[name].get()));
    }
    return renderableViews;
}

/// @brief 更新所有ui视图
void UIManager::updateAllUIs()
{
    // 按注册顺序更新ui
    for ( const auto& name : m_uiSequence ) {
        // 内部触发 ImGui 渲染和画笔收集
        m_uiviews[name]->update();
    }
}

}  // namespace MMM::Graphic::UI
