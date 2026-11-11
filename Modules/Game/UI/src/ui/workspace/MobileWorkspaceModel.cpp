#include "ui/workspace/MobileWorkspaceModel.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace MMM::UI
{
/// @brief 恢复归一化位置，具体像素边界仍由当前屏幕尺寸计算。
/// @details 配置读取已验证有限性；边界夹紧防止把手落到设备可用区外。
void MobileWorkspaceModel::restoreHandleCenters(float left, float right)
{
    m_leftHandleCenter  = std::clamp(left, 0.0f, 1.0f);
    m_rightHandleCenter = std::clamp(right, 0.0f, 1.0f);
}

/// @brief 以左上角和尺寸判断触摸是否属于本矩形。
/// @return 右边和下边不归本矩形，避免相邻层竞争同一触点。
/// @warning 仅做标量比较，不引用窗口或会话对象。
bool MobileRect::contains(float x, float y) const
{
    return x >= m_x && y >= m_y && x < m_x + m_width && y < m_y + m_height;
}

/// @brief 接收可用区，几何参数使用与 ImGui 相同的工作区坐标。
/// @param width 工作区宽度，不包含系统已经排除的屏幕安全区。
/// @param height 工作区高度，决定把手垂直居中位置。
/// @param scale 统一内容倍率，手势阈值和按钮几何使用同一单位。
/// @param rightInset 右侧工具栏的占用宽度，来自组合根本帧几何。
/// @details 旋转不重置目标状态，业务视图仍保留当前选择。
/// @warning UI 线程本地赋值，不等待平台窗口或交换链重建。
void MobileWorkspaceModel::resize(float width, float height, float scale,
                                  float rightInset, float handleWidth,
                                  float handleHeight)
{
    // 旋转会改变滑动分母和坐标系，旧触点不能继续控制新的矩形。
    if ( m_width != width || m_height != height || m_scale != scale ||
         m_rightInset != rightInset || m_handleWidth != handleWidth ||
         m_handleHeight != handleHeight )
        cancelGesture();
    // 异常布局值不能产生负窗口或非有限手势阈值。
    m_width  = std::max(1.0f, width);
    m_height = std::max(1.0f, height);
    m_scale  = std::max(0.5f, scale);
    // 工具栏显隐只平移把手，不能把用户已拖动的纵向偏好重置到屏幕中点。
    // 夹取保留区防止极窄布局生成负横坐标；命中与绘制共用此几何。
    m_rightInset = std::clamp(rightInset, 0.0f, m_width);
    // 标签尺寸独立于屏幕像素倍率，文字放大后命中与绘制仍用同一矩形。
    m_handleWidth  = std::max(56.0f, handleWidth);
    m_handleHeight = std::max(48.0f, handleHeight);
}

/// @brief 限制展开区宽度，同时适应狭窄手机屏幕。
/// @details 百分比上限保证外侧总有可触摸的关闭遮罩。
/// 左右采用同一宽度语义，拖动距离对应相同展开比例。
float MobileWorkspaceModel::drawerWidth() const
{
    return std::min(440.0f * m_scale, m_width * 0.86f);
}

/// @brief 生成左把手触摸矩形，可见握柄由绘制器另行居中。
/// @details 高度不超出工作区；窄视觉不代表窄命中区域。
/// 系统返回起点与应用把手分开，避免同向手势互相抢占。
/// 中心初值为屏幕中点，但后续帧不能覆盖用户已移动的位置。
/// 几何按当前可用高度夹取，短屏和旋转不会把完整命中区放到屏幕外。
MobileRect MobileWorkspaceModel::leftHandle() const
{
    // 收起入口始终完整可见，旧配置的缩边偏好不再使其退化成细线。
    // 只为标签留出边缘距离，画布仍然使用完整工作区，不添加根窗口外边距。
    const float height = std::min(m_height, m_handleHeight * m_scale);
    return { 32.0f * m_scale,
             std::clamp(m_leftHandleCenter * m_height - height * .5f,
                        0.0f,
                        m_height - height),
             m_handleWidth * m_scale,
             height };
}

/// @brief 将同一把手规格镜像到右侧，不复制手势阈值常量。
/// @return 工作区相对坐标，尚未添加主视口的屏幕偏移。
/// @warning 镜像只涉及标量，不查找 UI 窗口。
MobileRect MobileWorkspaceModel::rightHandle() const
{
    auto rect = leftHandle();
    // 标签位于工具栏左侧，系统最外缘不作为抽屉命中区。
    // 工具栏显隐只改变横坐标，左右标签仍各自保留纵向偏好。
    rect.m_x =
        std::max(0.0f, m_width - m_rightInset - rect.m_width - 32.0f * m_scale);
    // 右把手只镜像边距；独立中心确保移动左把手不会连带移动右把手。
    rect.m_y = std::clamp(m_rightHandleCenter * m_height - rect.m_height * .5f,
                          0.0f,
                          m_height - rect.m_height);
    return rect;
}

/// @brief 根据展开量平移整个抽屉，而非压缩内容控件。
/// @details 内容宽度保持不变，拖动时文字不会逐帧重新折行。
/// @return 可以部分位于工作区外，由绘制器按视口裁剪。
MobileRect MobileWorkspaceModel::drawerRect() const
{
    const auto width = drawerWidth();
    return { m_drawer == MobileDrawer::Left ? width * (m_fraction - 1.0f)
                                            : m_width - width * m_fraction,
             0.0f,
             width,
             m_height };
}

/// @brief 将额外把手放到面板外沿，内容区不再预留顶部收起条。
/// @return 工作区坐标中的独立命中区，遮罩关闭判定必须先排除它。
/// @details 水平位置跟随抽屉动画，纵向位置与该侧收起把手共用。
/// 窄屏外侧留白不足 48dp 时向内夹取，仍保证完整命中区在屏幕内。
/// 纵向由同侧把手处理边界，横向只校正外沿命中区，避免重复偏移。
/// 额外把手不改变面板本身宽度，文件与音频列表不会因它出现而重新换行。
MobileRect MobileWorkspaceModel::drawerGrip() const
{
    const auto drawer = drawerRect();
    auto rect = m_drawer == MobileDrawer::Left ? leftHandle() : rightHandle();
    const float edge = m_drawer == MobileDrawer::Left
                           ? drawer.m_x + drawer.m_width
                           : drawer.m_x - rect.m_width;
    rect.m_x = std::clamp(edge, 0.0f, std::max(0.0f, m_width - rect.m_width));
    return rect;
}

/// @brief 记录起点和原目标，不累计事件差分，避免拖动随帧率变化。
/// @param gesture 当前按下边沿选定的手势所有者。
/// @param pointer 初始触点，后续位移始终相对于这一位置计算。
/// @warning 输入采样路径只写本地标量，不向后台发布中间状态。
void MobileWorkspaceModel::beginGesture(Gesture gesture, MobilePointer pointer)
{
    m_gesture = gesture;
    m_startX  = pointer.m_x;
    m_startY  = pointer.m_y;
    // 起点保持实际夹取后的中心，旋转后第一次拖动不会出现位置跳变。
    if ( gesture == Gesture::Handle ) {
        // 以把手中心加相对位移，按在把手边缘也不会瞬间跳到手指中心。
        // 只在按下沿采样，持续移动不会累计舍入误差或重复偏移。
        const auto handle =
            m_handleSide == MobileDrawer::Left ? leftHandle() : rightHandle();
        m_startHandleCenter = handle.m_y + handle.m_height * .5f;
    }
}

/// @brief 取消恢复原目标，不根据无效的最后坐标选择展开或收起。
/// @details 即使平台丢失释放沿，后续画布输入也能恢复，不遗留返回捕获。
/// 原目标只用于取消，正常释放仍根据本次实际位移选择最终状态。
/// @warning 仅 UI 线程调用，不执行平台指针捕获或同步等待。
void MobileWorkspaceModel::cancelGesture()
{
    if ( m_gesture == Gesture::None ) return;
    // 抽屉目标只在有效轻点释放时提交，中断不需要回滚半开的抽屉。
    m_gesture        = Gesture::None;
    m_swallowRelease = true;
}

/// @brief 按真实帧间隔推进剩余距离，连续拖动分支不使用此缓动。
/// @param seconds 当前帧间隔；负值按零处理，不允许动画时间倒退。
/// @details 关闭动画归零后才释放抽屉身份，防止残留内容没有输入保护。
/// 指数收敛避免依赖固定帧数，拖动帧只由实际触点决定位置。
/// @warning 每帧常量级数学计算，不使用固定时长等待来完成动画。
void MobileWorkspaceModel::advanceAnimation(float seconds)
{
    const float target = m_targetOpen ? 1.0f : 0.0f;
    m_fraction += (target - m_fraction) *
                  (1.0f - std::exp(-18.0f * std::max(0.0f, seconds)));
    if ( !m_targetOpen && m_fraction < 0.001f ) {
        m_fraction = 0.0f;
        m_drawer   = MobileDrawer::None;
    }
}

/// @brief 按钮、遮罩和返回共用收起入口，不触及业务视图的当前选择。
/// @details 取消仍在进行的拖动，避免下一次采样重新打开刚关闭的抽屉。
/// 动画尚未结束时仍保留原抽屉侧，绘制与输入层级一起完成收起。
/// @warning 低频激活路径只设置本地意图，不释放业务资源。
void MobileWorkspaceModel::closeDrawer()
{
    m_gesture        = Gesture::None;
    m_targetOpen     = false;
    m_swallowRelease = true;
}

/// @brief 系统返回事件遵循抽屉、确认窗口、退出确认的层级规则。
/// @details 抽屉优先收起，已有确认优先取消，其余情况请求退出确认。
/// 这里只记录意图，不终止线程或修改打开的项目。
void MobileWorkspaceModel::requestBack()
{
    // 返回优先结束当前层级，不把收起抽屉解释为退出编辑器。
    if ( m_drawer != MobileDrawer::None ) {
        // 当前层级优先于应用退出，同一次返回不能同时关闭抽屉并弹出确认。
        closeDrawer();
    } else if ( m_exitConfirmation ) {
        m_exitConfirmation = false;
        m_swallowRelease   = true;
    } else {
        // 系统返回可以打断应用手势；确认结束后不能继续处理旧触点。
        cancelGesture();
        m_exitConfirmation = true;
    }
}

/// @brief 发布确认按钮的结果，两种结果都结束当前确认状态。
/// @param accepted 只有明确的确认按钮可以传入 true。
/// @details 释放保护覆盖整个关闭手势，不能再次操作背景。
void MobileWorkspaceModel::finishExitConfirmation(bool accepted)
{
    m_exitConfirmation = false;
    m_exitRequested    = accepted;
    m_swallowRelease   = true;
}

/// @brief 组合根消费意图后再调用既有正常关闭通道。
/// @return 一次确认至多返回一次 true，不在普通帧重复提交关闭。
/// @warning 只访问 UI 线程私有值，不需要原子或互斥锁。
bool MobileWorkspaceModel::takeExitRequest()
{
    return std::exchange(m_exitRequested, false);
}

/// @brief 判断画布原始输入是否需要由移动外壳拦截。
/// @details 收起动画结束后也要覆盖关闭手势的完整释放。
/// 不按当前触点是否在抽屉内决定，避免拖出后突然绘制物件。
bool MobileWorkspaceModel::blocksCanvasInput() const
{
    return m_drawer != MobileDrawer::None || m_gesture != Gesture::None ||
           m_swallowRelease || m_exitConfirmation;
}

/// @brief 每次采样只推进一次手势，归属以最初触摸起点为准。
/// @param pointer 单帧采样，不同阶段不能重复提交同一个按下边沿。
/// @param seconds 仅用于松手后的剩余动画，不参与拖动位移计算。
/// @param popupBlocked 高层业务模态已拥有输入，抽屉不得抢占。
/// @details 拖动直接跟随触点，只有松手后的剩余距离执行动画。
/// @warning 每帧固定分支与数学运算，不创建任务或执行同步等待。
void MobileWorkspaceModel::update(MobilePointer pointer, float seconds,
                                  bool popupBlocked)
{
    // 释放当帧仍保持阻挡，下一次采样才允许背景接收新的手势。
    // 新的按下沿也是新手势，即使两次快速点击之间没有空帧也能重新取得所有权。
    if ( pointer.m_pressed || (!pointer.m_down && !pointer.m_released) )
        m_swallowRelease = false;
    // 业务模态出现或触点被取消时必须清除旧捕获，不能等一个永远不来的释放沿。
    if ( popupBlocked || m_exitConfirmation ) {
        cancelGesture();
        if ( pointer.m_down || pointer.m_released ) m_swallowRelease = true;
        advanceAnimation(seconds);
        return;
    }
    // 抽屉开合只由标签点击、遮罩和平台返回决定，不截获系统边缘滑动。
    // 一次触点的归属固定，画布或列表起手后经过标签不会突然被接管。
    if ( pointer.m_pressed && !m_swallowRelease ) {
        if ( m_drawer != MobileDrawer::None ) {
            if ( drawerGrip().contains(pointer.m_x, pointer.m_y) ) {
                m_handleSide = m_drawer;
                beginGesture(Gesture::Handle, pointer);
            } else if ( !drawerRect().contains(pointer.m_x, pointer.m_y) )
                closeDrawer();
        } else {
            const auto side = leftHandle().contains(pointer.m_x, pointer.m_y)
                                  ? MobileDrawer::Left
                              : rightHandle().contains(pointer.m_x, pointer.m_y)
                                  ? MobileDrawer::Right
                                  : MobileDrawer::None;
            if ( side != MobileDrawer::None ) {
                m_handleSide = side;
                beginGesture(Gesture::Handle, pointer);
            }
        }
    }
    // 平台取消不等于松手，不能让失焦或模态中断意外打开抽屉。
    if ( m_gesture != Gesture::None && !pointer.m_down && !pointer.m_released )
        cancelGesture();
    const float horizontal = pointer.m_x - m_startX;
    const float vertical   = pointer.m_y - m_startY;
    const float slop       = 8.0f * m_scale;
    if ( m_gesture == Gesture::Handle ) {
        // 方向只锁定一次；横向划过后即使手指划回原位也不能变成点击。
        // 上下移动后锁住标签归属，不再根据随后的横向漂移触发开关。
        if ( std::abs(vertical) > slop &&
             std::abs(vertical) >= std::abs(horizontal) )
            m_gesture = Gesture::Reposition;
        else if ( std::abs(horizontal) > slop )
            m_gesture = Gesture::Cancelled;
        else if ( pointer.m_released ) {
            // 仅完整轻点提交一次开关；展开后的标签同样点击收起。
            m_drawer     = m_handleSide;
            m_targetOpen = !m_targetOpen;
        }
    }
    if ( m_gesture == Gesture::Reposition ) {
        // 直接跟随手指更新位置，不等待动画、磁盘保存或后台任务。
        // 中心按实际标签高度夹紧，放大字号和旋转后也不会有半个标签出屏。
        const float half = std::min(m_height, m_handleHeight * m_scale) * .5f;
        const float center =
            std::clamp(m_startHandleCenter + vertical, half, m_height - half) /
            m_height;
        if ( m_handleSide == MobileDrawer::Left )
            m_leftHandleCenter = center;
        else
            m_rightHandleCenter = center;
    }
    if ( m_gesture != Gesture::None && pointer.m_released ) {
        // 取消横拖也吞掉释放，不让这一触点误落到背景编辑器。
        m_gesture        = Gesture::None;
        m_swallowRelease = true;
    }
    // 开合动画与标签位置互不依赖，移动标签不暂停或反转抽屉动画。
    advanceAnimation(seconds);
}
}  // namespace MMM::UI
