#pragma once
#include "canvas/HoverLayerSelection.h"
#include <cstdint>
#include <glm/vec2.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MMM::UI
{
class UIManager;
}
namespace MMM::UI::Interaction
{
struct SurfaceFrame;
}
namespace MMM::Common::Render
{
struct RenderSnapshot;
}
namespace MMM::Canvas
{
class Basic2DCanvasInteraction;
/// @brief 平台菜单借用同一画布的轻量状态，引用只能活在当前更新调用栈。
/// @details 不拥有会话、谱面或 ImGui 窗口，不向后台线程传递。
/// 相机身份、坐标和谱面身份来自同一个控制器，不允许拼接另一画布的数据。
/// 菜单打开后位置冻结，但当前候选只用于打开瞬间锁存。
/// 容器只包含值语义候选，不借用 ECS 注册表或跨线程组件。
/// 由中立桥建立引用集合，平台派生类无法访问其余控制器私有状态。
struct CanvasMenuContext {
    const std::string& cameraId;      ///< 稳定相机身份隔离多个画布的弹层。
    double&            pasteTime;     ///< 菜单激活时冻结的吸附时间。
    glm::vec2&         menuPoint;     ///< 移除框选等操作使用的画布局部落点。
    const glm::vec2&   pointerPoint;  ///< 正常拾取路径发布的当前局部指针。
    std::string&       beatmapKey;  ///< 路径与实例共同防止跨谱面实体身份复用。
    std::uintptr_t&    beatmapInstance;  ///< 同路径重新加载也会改变该身份。
    std::vector<HoverLayerCandidate>& layers;  ///< 弹层打开时锁存的值语义候选。
    const std::vector<HoverLayerCandidate>&
        candidates;  ///< 当前帧拾取结果，只读借用。
    std::optional<HoverLayerCandidate>& preferred;  ///< 显式选择的重叠层身份。
};
/// @brief 平台独立选择输入与菜单呈现，通用画布不判断安卓或触屏布局。
/// @details 桌面与移动实现、工厂及私有头由 CMake 选择，运行时不散落宏分支。
/// 公共头只前置声明本类，调用方不需要编译任一平台菜单定义。
/// 实现对象随画布构造一次，更新路径只进行虚调用。
/// 平台类可以改变输入呈现和布局，但不能绕过正式编辑命令。
/// 重叠选择和批注编辑保持公共业务实现，防止两端行为分叉。
class CanvasInteractionPresentation
{
public:
    /// @brief 在画布更新停止后销毁，仅拥有本地轻量交互状态。
    virtual ~CanvasInteractionPresentation() = default;
    /// @brief 解释一次已通过外部方案检查的画布帧。
    /// @warning 每帧执行；禁止等待会话长锁、文件操作和逐帧分配。
    virtual void update(Basic2DCanvasInteraction&             canvas,
                        UI::UIManager*                        manager,
                        const Common::Render::RenderSnapshot* snapshot,
                        const UI::Interaction::SurfaceFrame& frame, float width,
                        float height) = 0;
    /// @brief 平台批注入口：移动可请求常驻阅读菜单，桌面仍使用右键。
    /// @warning 每帧只分类输入，不保存快照或发送编辑命令。
    virtual bool annotationMenuRequested(std::uint32_t, bool) { return false; }
    /// @brief 待定触点同样拥有输入，摄像机不能在意图未定时接管。
    /// @warning 每帧只读取平台本地标量，不访问会话或加锁。
    virtual bool hasGesture() const { return false; }
    /// @brief 平台呈现决定是否显示跟随鼠标的详细检视框。
    /// @details 桌面默认保留，触屏使用本地按住反馈，公共画布不判断平台。
    /// @warning 每帧只读取操作方案能力，不分配、不访问会话或加锁。
    virtual bool allowsHoverInspection() const { return true; }

protected:
    /// @details 以下桥只允许同步转发，不保存管理器、快照或编辑器的所有权。
    /// 静态桥维持私有访问边界，平台派生类不能直接修改其他输入状态。
    /// @brief 短期借用共同菜单状态，不泄露整个控制器的私有实现。
    static CanvasMenuContext context(Basic2DCanvasInteraction& canvas);
    /// @brief 平台只安排输入，实际编辑仍由既有控制器和逻辑命令执行。
    /// @param completedDrag 平台已确认的拖动释放，补足同帧移动抬手的终点更新。
    /// 默认关闭，桌面仍按物理拖动门槛与原有释放门闩判断。
    /// @param target 平台已锁存的可见把手目标；空值保留普通拾取。
    /// @param pointerOnly
    /// 同帧补交连续指针样本时跳过快捷键，避免一次按键重复执行。
    /// 只适用于已经起笔的输入回放，普通帧和桌面入口保持默认关闭。
    /// @param tapOnly 已完成分类的轻点不拖动物件，折线命中旧物件只选择。
    /// 起笔和释放都使用该意图；普通拖动及桌面入口保持默认关闭。
    /// @warning 每帧调用，保留原有连续命令去重和权限检查。
    static void routeInput(Basic2DCanvasInteraction&             canvas,
                           const Common::Render::RenderSnapshot* snapshot,
                           float width, float height, bool cancellable = false,
                           bool completedDrag = false, bool tapOnly = false,
                           bool                       pointerOnly = false,
                           const HoverLayerCandidate* target      = nullptr);
    /// @brief 输入被接管时回退尚未提交的编辑，不触及此前的撤销历史。
    /// @warning 仅在取消边沿调用；通过命令队列顺序执行，不等待逻辑线程。
    static void cancelInput(Basic2DCanvasInteraction&             canvas,
                            const Common::Render::RenderSnapshot& snapshot);
    /// @brief 两种平台都可复用重叠物件选择，目标归当前画布。
    static void renderLayers(Basic2DCanvasInteraction&             canvas,
                             const Common::Render::RenderSnapshot& snapshot);
    /// @brief 桌面扩展项沿用旧选择、属性和批注入口。
    static void renderExtras(Basic2DCanvasInteraction& canvas,
                             UI::UIManager*            manager);
    /// @brief 新建时间戳批注只准备原编辑器请求，不直接写入谱面。
    static void requestAnnotation(Basic2DCanvasInteraction& canvas,
                                  double                    time);
};
/// @brief 每个画布构造时只创建一次目标平台实现，调用方不包含平台私有头。
std::unique_ptr<CanvasInteractionPresentation>
createCanvasInteractionPresentation();
/// @brief 经典和纯鼠标方案共用桌面输入解释器，安卓接鼠标时同样可用。
/// @details 每个画布只构造一次，不在切换方案或逐帧更新时分配。
std::unique_ptr<CanvasInteractionPresentation>
createDesktopCanvasInteractionPresentation();
}  // namespace MMM::Canvas
