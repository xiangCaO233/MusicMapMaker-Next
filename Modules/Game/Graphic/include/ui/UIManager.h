#pragma once

#include "IUIView.h"
#include <memory>
#include <unordered_map>
#include <vector>

namespace MMM::Graphic::UI
{
class UIManager
{
public:
    UIManager()                            = default;
    UIManager(UIManager&&)                 = delete;
    UIManager(const UIManager&)            = delete;
    UIManager& operator=(UIManager&&)      = delete;
    UIManager& operator=(const UIManager&) = delete;
    ~UIManager()                           = default;

    /// @brief 注册视图，转交所有权
    void registerView(const std::string& name, std::unique_ptr<IUIView> view);

    /// @brief 泛型获取裸指针 外部不负责销毁
    template<typename T> T* getView(const std::string& name)
    {
        auto it = m_uiviews.find(name);
        if ( it != m_uiviews.end() ) {
            return dynamic_cast<T*>(it->second.get());
        }
        return nullptr;
    }

    void printUis();

    /// @brief 更新所有ui视图
    void updateAllUIs();

private:
    /// @brief 所有ui接口
    std::unordered_map<std::string, std::unique_ptr<IUIView>> m_uiviews;

    /// @brief ui接口注册顺序
    std::vector<std::string> m_uiSequence;
};
}  // namespace MMM::Graphic::UI
