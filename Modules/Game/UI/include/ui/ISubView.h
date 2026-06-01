#pragma once

#include "imgui.h"
#include "ui/IParallelUiPreparable.h"
#include <string>
namespace MMM::UI
{
class UIManager;
class LayoutContext;
// 所有的侧边栏内容需要实现的简单接口
class ISubView
{
public:
    ISubView(const std::string& subViewName) : m_subViewName(subViewName) {}

    virtual ~ISubView() = default;

    /// @brief 内部绘制逻辑 (Clay/ImGui)
    virtual void onUpdate(LayoutContext& layoutContext,
                          UIManager*     sourceManager) = 0;

    /// @brief 安全转换为可并行准备 UI 数据的接口。
    /// @return 默认子视图不提供并行准备接口。
    virtual IParallelUiPreparable* asParallelUiPreparable() { return nullptr; }

    /// @brief 获取子视图名称
    inline const std::string& getSubViewName() const { return m_subViewName; }

    /// @brief 获取子视图中不可再换行元素所需的最小内容尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 子视图内容区域的最小尺寸；返回 0 表示不额外限制。
    virtual ImVec2 getMinContentSize(float dpiScale) const
    {
        (void)dpiScale;
        return ImVec2(0.0f, 0.0f);
    }

protected:
    /// @brief 子视图名称
    const std::string m_subViewName;
};
}  // namespace MMM::UI
