#pragma once

#include <string>
namespace MMM::Graphic::UI
{
class LayoutContext;
// 所有的侧边栏内容需要实现的简单接口
class ISubView
{
public:
    ISubView(const std::string& subViewName) : m_subViewName(subViewName) {}

    virtual ~ISubView() = default;

    /// @brief 内部绘制逻辑 (Clay/ImGui)
    virtual void onUpdate(LayoutContext& layoutContext) = 0;

    /// @brief 获取子视图名称
    inline const std::string& getSubViewName() const { return m_subViewName; }

protected:
    /// @brief 子视图名称
    const std::string& m_subViewName;
};
}  // namespace MMM::Graphic::UI
