#pragma once

#include "imgui.h"
#include "ui/IParallelUiPreparable.h"
#include <string>

namespace vk
{
class PhysicalDevice;
class Device;
class CommandPool;
class Queue;
}  // namespace vk

namespace MMM::UI
{
class UIManager;
class LayoutContext;
/// @brief 所有侧边栏内容共享的轻量绘制与资源准备接口。
/// @details 子视图由上层容器持有，名称用于布局和查找；默认实现不拥有 GPU
/// 资源，也不参与并行数据准备。
class ISubView
{
public:
    /// @brief 创建具有稳定名称的子视图。
    /// @param subViewName 上层容器注册和查找该视图时使用的名称。
    ISubView(const std::string& subViewName) : m_subViewName(subViewName) {}

    /// @brief 通过接口析构派生子视图。
    virtual ~ISubView() = default;

    /// @brief 执行子视图内部的 Clay 或 ImGui 绘制逻辑。
    /// @param layoutContext 当前帧布局上下文。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI 热路径：面板可见时每帧调用。
    virtual void onUpdate(LayoutContext& layoutContext,
                          UIManager*     sourceManager) = 0;

    /// @brief 在查询布局尺寸前同步 UIManager 中的项目生命周期快照。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI 热路径：每帧可能调用多次；默认不执行任何操作，实现中禁止
    /// 文件系统扫描或高开销所有权复制。
    virtual void syncProjectUiState(UIManager* sourceManager)
    {
        (void)sourceManager;
    }

    /// @brief 安全转换为可并行准备 UI 数据的接口。
    /// @return 默认子视图不提供并行准备接口。
    virtual IParallelUiPreparable* asParallelUiPreparable() { return nullptr; }

    /// @brief 查询子视图是否有等待 GPU 上传的纹理资源。
    /// @return 默认子视图不加载纹理。
    /// @warning UI 资源准备热路径：只能读取内存脏位。
    virtual bool needsTextureReload() const { return false; }

    /// @brief 在所属纹理视图的资源准备阶段上传子视图纹理。
    /// @warning GPU 资源准备低频路径：仅在 needsTextureReload 返回 true
    /// 后调用。
    virtual void reloadTextures(vk::PhysicalDevice& physicalDevice,
                                vk::Device&         logicalDevice,
                                vk::CommandPool& commandPool, vk::Queue& queue)
    {
        (void)physicalDevice;
        (void)logicalDevice;
        (void)commandPool;
        (void)queue;
    }

    /// @brief 获取子视图名称。
    /// @return 与对象生命周期一致的只读名称引用。
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
    /// @brief 子视图稳定名称，构造后不再改变。
    const std::string m_subViewName;
};
}  // namespace MMM::UI
