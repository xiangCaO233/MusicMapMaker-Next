#pragma once

namespace MMM::UI
{
/// @brief 不依赖 ImGui 的移动工作区矩形，供布局和手势测试共用。
struct MobileRect {
    /// @brief 工作区相对位置与尺寸，逻辑像素与 ImGui 坐标保持一致。
    float m_x{}, m_y{}, m_width{}, m_height{};
    /// @brief 边界命中采用半开区间，避免相邻窗口同时取得一次触摸。
    bool contains(float x, float y) const;
};

/// @brief 抽屉只能独占一侧，关闭时保留业务视图的选择状态。
enum class MobileDrawer { None, Left, Right };

/// @brief 单次指针采样，坐标相对于主视口工作区。
struct MobilePointer {
    /// @brief 触点在可用工作区中的当前位置，不是屏幕全局坐标。
    float m_x{}, m_y{};
    /// @brief 按下、保持和释放分别采样，取消触点不伪造释放边沿。
    bool m_pressed{}, m_down{}, m_released{};
};

/// @brief 移动外壳的纯状态机，不持有会话、文件系统或渲染器。
class MobileWorkspaceModel
{
public:
    /// @brief 重新计算可用区域；旋转时只修改几何，不销毁业务视图。
    void resize(float width, float height, float scale);
    /// @brief 先确定触摸归属，再允许画布处理本帧原始输入。
    /// @warning 每帧固定规模运算，不分配或同步等待。
    void update(MobilePointer pointer, float deltaSeconds, bool popupBlocked);
    /// @brief 返回先收起抽屉，空闲时再进入专用退出确认。
    void requestBack();
    /// @brief 显式关闭当前抽屉，保持业务选择并消费关闭动作的释放。
    void closeDrawer();
    /// @brief 取消确认只返回编辑，确认后由组合根提交正常退出事件。
    void finishExitConfirmation(bool accepted);
    /// @brief 消费一次确认后的退出请求，不直接终止进程。
    bool takeExitRequest();
    /// @brief 当前抽屉侧；滑动动画结束前仍保留原来的侧。
    MobileDrawer drawer() const { return m_drawer; }
    /// @brief 连续拖动时直接跟随手指，松手后才进行剩余距离动画。
    float fraction() const { return m_fraction; }
    /// @brief 含关闭手势释放保护，防止同一次触摸落到下面的画布。
    bool blocksCanvasInput() const;
    /// @brief 退出确认窗口与普通抽屉互斥。
    bool exitConfirmation() const { return m_exitConfirmation; }
    /// @brief 边缘把手的触摸区大于可见握柄，满足手指命中尺寸。
    MobileRect leftHandle() const;
    /// @brief 与左把手镜像的右侧触摸区域，保留相同尺寸与系统边缘间隔。
    MobileRect rightHandle() const;
    /// @brief 展开的两级抽屉共同占用的区域。
    MobileRect drawerRect() const;
    /// @brief 展开抽屉外沿的独立把手，与该侧收起把手共享纵向位置。
    MobileRect drawerGrip() const;
    /// @brief 根据手持屏幕宽度限制抽屉宽度，始终留出遮罩点击区域。
    float drawerWidth() const;

private:
    /// @brief 手势起点确定唯一所有者，中途不能从业务内容切换到抽屉。
    enum class Gesture { None, Handle, Reposition, Drawer, Back };
    /// @brief 从起点保存展开状态，取消、旋转和业务模态打断时恢复目标。
    void beginGesture(Gesture gesture, MobilePointer pointer);
    /// @brief 结束未提交手势，不把失焦或触点取消当作一次点击。
    void cancelGesture();
    /// @brief 仅在非拖动帧推进收起或展开动画，不阻塞 UI 线程。
    void advanceAnimation(float seconds);
    /// @brief 可用尺寸与物理内容缩放，只由 UI 线程写入。
    float m_width{ 1 }, m_height{ 1 }, m_scale{ 1 };
    /// @brief 当前侧及归一化展开量，不与文件管理器选中状态绑定。
    MobileDrawer m_drawer{ MobileDrawer::None };
    /// @brief 当前展开量、手势初始展开量及固定触摸起点。
    float m_fraction{}, m_startFraction{}, m_startX{}, m_startY{};
    /// @brief 一次手势从开始到释放始终由同一控制器处理。
    bool m_targetOpen{}, m_swallowRelease{};
    /// @brief 当前手势及起点状态；水平锁定前不移动内容或提交返回。
    Gesture m_gesture{ Gesture::None };
    bool    m_startOpen{}, m_horizontalLocked{}, m_backFromLeft{};
    /// @brief 左右把手独立保存归一化中心，旋转后仍落在可触及范围内。
    float m_leftHandleCenter{ .5f }, m_rightHandleCenter{ .5f },
        m_startHandleCenter{};
    /// @brief 把手起点所属一侧，拖动途中不能切换另一侧。
    MobileDrawer m_handleSide{ MobileDrawer::None };
    /// @brief 退出确认由用户明确决定，返回手势本身不关闭进程。
    bool m_exitConfirmation{}, m_exitRequested{};
};
}  // namespace MMM::UI
