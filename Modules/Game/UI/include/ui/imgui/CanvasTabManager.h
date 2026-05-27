#pragma once

#include "ui/IUIView.h"
#include <string>
#include <unordered_set>

namespace MMM::UI
{

/**
 * @brief 画布 Tab 管理器 (系统级视图)
 *
 * 负责在 UI 线程同步 EditorEngine 中的逻辑会话与 UIManager 中的渲染画布。
 * 动态处理新画布的创建、动态停靠、以及画布关闭事件。
 */
class CanvasTabManager : public IUIView
{
public:
    /// @brief 构造函数
    CanvasTabManager(const std::string& name = "CanvasTabManager");

    /// @brief 析构函数
    ~CanvasTabManager() override = default;

    /// @brief 每帧更新与同步
    void update(UIManager* sourceManager) override;

    /// @brief 获取实际类型指针
    void* getActualInstance() override { return this; }

private:
    /// @brief 已初始化画布的 cameraId 集合
    std::unordered_set<std::string> m_initializedCanvases;
};

}  // namespace MMM::UI
