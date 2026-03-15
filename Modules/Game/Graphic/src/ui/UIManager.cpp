#include "ui/UIManager.h"
#include "log/colorful-log.h"

namespace MMM::Graphic::UI
{

/// @brief 注册视图，转交所有权
void UIManager::registerView(const std::string&       name,
                             std::unique_ptr<IUIView> view)
{
    m_uiSequence.push_back(name);
    m_uiviews[name] = std::move(view);
    XINFO("Registered [{}] UIView", name);
}

void UIManager::printUis()
{
    int count{ 1 };
    for ( const auto& [name, view] : m_uiviews ) {
        XINFO("ui [{}] ,name:[{}]", count, name);
        ++count;
    }
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
