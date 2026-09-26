#pragma once

#include "ui/ITextureLoader.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::UI
{

/// @brief 工具插件在管理界面中展示的加载快照。
struct ToolPluginInfo {
    /// @brief 发布后保持稳定的插件标识。
    std::string id;
    /// @brief 窗口及管理界面展示名称。
    std::string name;
    /// @brief 加载或运行失败时的诊断文本，成功时为空。
    std::string error;
    /// @brief 清单项是否已创建可打开的工具窗口。
    bool available{ true };
};

/// @brief 将 Lua 声明式控件渲染为可停靠的 ImGui 工具窗口。
/// @details Lua 只在加载与用户动作后构造控件树，普通帧由 C++ 绘制缓存。
class ToolPluginView final : public ITextureLoader
{
public:
    /// @brief 加载内置和用户工具插件。
    /// @warning 初始化低频路径：扫描文件并执行 Lua，不能从 UI 每帧调用。
    ToolPluginView();

    /// @brief 释放插件 Lua 状态与窗口缓存。
    ~ToolPluginView() override;

    /// @brief 重新扫描工具插件，替换旧状态。
    /// @warning 用户触发的低频路径；旧插件窗口及回调在返回后失效。
    void reload();

    /// @brief 打开具有指定稳定 ID 的插件窗口。
    /// @return 找到并打开窗口时为 true。
    bool openPlugin(std::string_view id);

    /// @brief 返回最近一次扫描的插件快照。
    /// @warning UI 热路径：只借用内存向量，不做文件系统访问。
    [[nodiscard]] const std::vector<ToolPluginInfo>& plugins() const;

    /// @brief 绘制所有已打开工具插件窗口。
    /// @warning UI 热路径：无操作时只提交缓存控件，不运行 Lua 或访问文件。
    void update(UIManager* sourceManager) override;

    /// @brief 关闭 RTTI 时返回完整对象地址供视图注册表使用。
    void* getActualInstance() override { return this; }

    /// @brief 仅检查用户新选封面后的纹理上传待办。
    /// @warning UI 热路径：只读取 UI 线程布尔状态。
    [[nodiscard]] bool needReload() override;

    /// @brief 在渲染器资源准备阶段上传新封面纹理。
    /// @warning 低频资源路径：只处理新封面，不在常态帧分配 GPU 资源。
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

private:
    /// @brief 隔离 Lua 实现、控件缓存和每个插件的资源所有权。
    struct Impl;
    /// @brief 实现生命周期与本视图一致，不跨线程共享。
    std::unique_ptr<Impl> m_impl;
};

}  // namespace MMM::UI
