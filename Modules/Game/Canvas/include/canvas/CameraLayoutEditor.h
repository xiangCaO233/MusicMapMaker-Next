#pragma once

#include "ui/interaction/EditorInteraction.h"

#include "ui/layout/CLayWrapperCore.h"
#include <array>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <imgui.h>
#include <string>

namespace MMM::Config
{
struct VisualConfig;
}
namespace MMM::Canvas
{
/// @brief 布局期间的相机操作视图；不拥有 GPU 资源，也不修改谱面物件。
/// @details 随布局模式进入创建、退出销毁；预览只借用当前画布描述符。
class CameraLayoutEditor : public UI::Interaction::IInteractionGuard
{
public:
    /// @brief 默认构造不分配 Clay 内存，首次显示参数窗口时才创建。
    CameraLayoutEditor();
    /// @brief 布局退出时释放参数窗口专用的 Clay 内存。
    ~CameraLayoutEditor();
    /// @brief 禁止复制窗口上下文所有权，避免重复释放。
    CameraLayoutEditor(const CameraLayoutEditor&) = delete;
    /// @brief 禁止覆盖现有窗口上下文所有权。
    CameraLayoutEditor& operator=(const CameraLayoutEditor&) = delete;
    /// @brief 相机轴拖动或观察导航尚未结束时阻止切换操作方案。
    /// @warning 每帧只读本地事务标志，不查询 GPU 或逻辑会话。
    bool hasActivePointerGesture() const override
    {
        return m_axis >= 0 || m_orbiting;
    }
    /// @brief 查询本帧是否绘制摄像机操作空间。
    bool cameraMode() const { return m_cameraMode; }
    /// @brief 在调用方工具窗口中仅绘制元素与相机模式切换。
    /// @warning 每帧只访问 UI 线程配置，不获取会话锁或执行磁盘 IO。
    void drawControls(Config::VisualConfig& visual);
    /// @brief 绘制三维操作空间及实际视角预览。
    /// @return 当前模式或控制面板已占用画布输入时返回 true。
    /// @warning 每帧固定规模绘制；仅用户操作修改内存，不执行磁盘 IO。
    bool update(const std::string& id, ImVec2 origin, ImVec2 size,
                ImTextureID texture, Config::VisualConfig& visual);

private:
    /// @brief 参数窗口独享的 Clay 上下文，只在相机模式存在。
    UI::CLayWrapperCore::WindowContext m_parameterLayout{};
    /// @brief 在独立窗口中用 Clay 排列工具和精确数值。
    /// @warning 每帧固定控件；上下文仅在首次显示时分配。
    void drawParameterWindow(const std::string&    id,
                             Config::VisualConfig& visual);
    /// @brief 在参数窗口分配的两行区域绘制纯鼠标相机选项。
    /// @warning 每帧固定三个控件；活动手势期间禁止切换，不访问会话或磁盘。
    void drawPointerOptions();
    /// @brief 无 GPU 回归仅控制模式切换，实际绘制仍调用生产入口。
    friend struct CameraLayoutEditorTestAccess;
    /// @brief 摄像机模式与元素模式互斥；默认进入正视元素布局。
    bool m_cameraMode{ false };
    /// @brief 当前变换工具，false 为平移，true 为旋转。
    bool m_rotate{ false };
    /// @brief 正在拖动的世界轴，-1 表示没有事务。
    int m_axis{ -1 };
    /// @brief 鼠标中键导航只由操作视图按下边沿获取。
    bool m_orbiting{ false };
    /// @brief 纯鼠标中键按下时锁存导航意图，活动手势不随其它窗口改变。
    bool m_navigationPan{ false };
    /// @brief 纯鼠标轴或旋转环按下时锁存精细倍率，经典仍动态读取 Shift。
    bool m_fineAdjustment{ false };
    /// @brief 事务开始位置，Esc 恢复此值。
    std::array<float, 3> m_startPosition{};
    /// @brief 事务开始姿态，Esc 恢复此值。
    std::array<float, 3> m_startRotation{};
    /// @brief 事务累计屏幕增量，切换约束轴时重新解释整个手势。
    glm::vec2 m_dragDelta{};
    /// @brief 旋转环累计弧度，允许鼠标绕环跨越半周而不中断。
    float m_rotationDelta{ 0.0F };
    /// @brief 操作空间独立观察角，绝不写入目标相机。
    glm::vec2 m_orbit{ 25.0F, -35.0F };
    /// @brief 操作空间观察距离，与目标相机焦距无关。
    float m_distance{ 4.0F };
    /// @brief 中键平移改变操作空间观察中心。
    glm::vec3 m_center{ 0.0F, 0.0F, 0.4F };
    /// @brief 绘制固定网格、相机视锥及变换轴并处理手势。
    /// @warning 每帧固定辅助几何数量，不遍历谱面或等待逻辑线程。
    void drawScene(const std::string& id, ImVec2 origin, ImVec2 size,
                   Config::VisualConfig& visual);
};
}  // namespace MMM::Canvas
