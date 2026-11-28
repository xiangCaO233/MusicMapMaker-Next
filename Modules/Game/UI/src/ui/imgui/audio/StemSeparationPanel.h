#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace MMM::UI
{

/// @brief 主音轨控制器的声部分离区域，由控制器独占生命周期。
class StemSeparationPanel
{
public:
    /// @brief 通知本面板尚未完成的后台任务取消。
    virtual ~StemSeparationPanel() = default;

    /// @brief 从控制器绑定的项目主轨显示模型目录、分离和混音。
    /// @param root 已发布项目的目录。
    /// @param relativePath 当前项目主轨的资源路径。
    /// @param dpiScale 控制器的内容缩放。
    /// @warning UI 热路径。仅在源变化、显式操作或任务就绪时处理资源。
    virtual void render(const std::filesystem::path& root,
                        const std::string& relativePath, float dpiScale) = 0;
};

/// @brief 创建主音轨控制器的私有声部分离区域。
std::unique_ptr<StemSeparationPanel> createStemSeparationPanel();

}  // namespace MMM::UI
