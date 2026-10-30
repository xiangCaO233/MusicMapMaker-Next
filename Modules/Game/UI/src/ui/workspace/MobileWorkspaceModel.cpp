#include "ui/workspace/MobileWorkspaceModel.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace MMM::UI
{
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
/// @details 旋转不重置目标状态，业务视图仍保留当前选择。
/// @warning UI 线程本地赋值，不等待平台窗口或交换链重建。
void MobileWorkspaceModel::resize(float width, float height, float scale)
{
    // 旋转会改变滑动分母和坐标系，旧触点不能继续控制新的矩形。
    if ( m_width != width || m_height != height || m_scale != scale )
        cancelGesture();
    // 异常布局值不能产生负窗口或非有限手势阈值。
    m_width  = std::max(1.0f, width);
    m_height = std::max(1.0f, height);
    m_scale  = std::max(0.5f, scale);
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
MobileRect MobileWorkspaceModel::leftHandle() const
{
    // 远离系统返回手势起点；握柄窄，实际触摸区保持 48dp。
    return { 16.0f * m_scale,
             std::max(0.0f, (m_height - 72.0f * m_scale) * 0.5f),
             48.0f * m_scale,
             std::min(m_height, 72.0f * m_scale) };
}

/// @brief 将同一把手规格镜像到右侧，不复制手势阈值常量。
/// @return 工作区相对坐标，尚未添加主视口的屏幕偏移。
/// @warning 镜像只涉及标量，不查找 UI 窗口。
MobileRect MobileWorkspaceModel::rightHandle() const
{
    auto rect = leftHandle();
    rect.m_x  = m_width - rect.m_width - rect.m_x;
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

/// @brief 标题区为两级抽屉共用，不挤占分类按钮和业务列表的触摸区域。
/// @details 高度与关闭按钮的 48dp 命中区分开计算，保留上下留白。
float MobileWorkspaceModel::headerHeight() const
{
    return std::min(m_height, 72.0f * m_scale);
}

/// @brief 只有标题可拖动收起，文件列表的水平和纵向操作均留给业务内容。
/// @return 排除右侧按钮及留白的标题区域，使用工作区相对坐标。
/// @details 收起按钮包含图标、短标签和外边距，不能只扣除图标的宽度。
/// 排除区略宽于按钮，使手指按在文字边缘时也不会启动拖动。
MobileRect MobileWorkspaceModel::drawerGrip() const
{
    auto rect = drawerRect();
    // 标题与显式按钮互斥，不能一次点击既激活按钮又开始拖动。
    rect.m_width  = std::max(0.0f, rect.m_width - 144.0f * m_scale);
    rect.m_height = headerHeight();
    return rect;
}

/// @brief 记录起点和原目标，不累计事件差分，避免拖动随帧率变化。
/// @param gesture 当前按下边沿选定的手势所有者。
/// @param pointer 初始触点，后续位移始终相对于这一位置计算。
/// @warning 输入采样路径只写本地标量，不向后台发布中间状态。
void MobileWorkspaceModel::beginGesture(Gesture gesture, MobilePointer pointer)
{
    m_gesture          = gesture;
    m_startX           = pointer.m_x;
    m_startY           = pointer.m_y;
    m_startFraction    = m_fraction;
    m_startOpen        = m_targetOpen;
    m_horizontalLocked = false;
    m_backFromLeft     = pointer.m_x < m_width * .5f;
}

/// @brief 取消恢复原目标，不根据无效的最后坐标选择展开或收起。
/// @details 即使平台丢失释放沿，后续画布输入也能恢复，不遗留返回捕获。
/// 原目标只用于取消，正常释放仍根据本次实际位移选择最终状态。
/// @warning 仅 UI 线程调用，不执行平台指针捕获或同步等待。
void MobileWorkspaceModel::cancelGesture()
{
    if ( m_gesture == Gesture::None ) return;
    if ( m_gesture == Gesture::Drawer ) m_targetOpen = m_startOpen;
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

/// @brief 系统返回与应用边缘返回共用相同的层级规则。
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
    // 仅按下沿决定归属，内容区开始的操作中途经过标题也不能抢走。
    if ( pointer.m_pressed && !m_swallowRelease ) {
        const bool edge = pointer.m_x < 12.0f * m_scale ||
                          pointer.m_x > m_width - 12.0f * m_scale;
        if ( m_drawer != MobileDrawer::None ) {
            // 已展开时先判断遮罩，背景按下只收起，不建立第二个打开手势。
            if ( !drawerRect().contains(pointer.m_x, pointer.m_y) )
                closeDrawer();
            else if ( edge )
                beginGesture(Gesture::Back, pointer);
            else if ( drawerGrip().contains(pointer.m_x, pointer.m_y) )
                beginGesture(Gesture::Drawer, pointer);
        } else if ( edge ) {
            beginGesture(Gesture::Back, pointer);
        } else {
            const auto side = leftHandle().contains(pointer.m_x, pointer.m_y)
                                  ? MobileDrawer::Left
                              : rightHandle().contains(pointer.m_x, pointer.m_y)
                                  ? MobileDrawer::Right
                                  : MobileDrawer::None;
            if ( side != MobileDrawer::None ) {
                m_drawer = side;
                beginGesture(Gesture::Drawer, pointer);
            }
        }
    }
    // 无释放沿按取消处理，不能凭空把零位移解释为轻点。
    if ( m_gesture != Gesture::None && !pointer.m_down && !pointer.m_released )
        cancelGesture();
    if ( m_gesture == Gesture::None ) {
        // 普通业务触摸不进入手势计算，但已提交的收起动画仍继续推进。
        advanceAnimation(seconds);
        return;
    }
    // 水平方向锁定前忽略微抖；纵向浏览优先取消，避免把手被擦过时展开。
    const float horizontal = pointer.m_x - m_startX;
    const float vertical   = pointer.m_y - m_startY;
    const float slop       = 8.0f * m_scale;
    if ( !m_horizontalLocked ) {
        // 一旦确定纵向意图就放弃本次抽屉手势，后续水平漂移不能重新抢占。
        if ( std::abs(vertical) > slop &&
             std::abs(vertical) >= std::abs(horizontal) ) {
            cancelGesture();
            advanceAnimation(seconds);
            return;
        }
        m_horizontalLocked = std::abs(horizontal) > slop;
    }
    if ( m_gesture == Gesture::Drawer ) {
        // 直接由固定起点计算展开量，手指移动与本地显示处于同一帧。
        const float direction = m_drawer == MobileDrawer::Left ? 1.0f : -1.0f;
        if ( m_horizontalLocked )
            m_fraction = std::clamp(
                m_startFraction + direction * horizontal / drawerWidth(),
                0.0f,
                1.0f);
        if ( pointer.m_released ) {
            // 从收起开始轻点即打开；标题轻点不关闭，只有按钮或明确滑动才收起。
            // 展开与收起均需跨过四分之一宽度，轻微抖动回到原目标。
            m_targetOpen = m_horizontalLocked
                               ? m_fraction >= (m_startOpen ? .75f : .25f)
                               : true;
        }
    } else if ( pointer.m_released ) {
        // 返回只能从对应边缘向内滑，不能把右缘向右或左缘向左误判为退出。
        const float inward = horizontal * (m_backFromLeft ? 1.0f : -1.0f);
        if ( inward > 64.0f * m_scale && std::abs(vertical) < 48.0f * m_scale )
            requestBack();
    }
    if ( pointer.m_released ) {
        m_gesture        = Gesture::None;
        m_swallowRelease = true;
    }
}
}  // namespace MMM::UI
