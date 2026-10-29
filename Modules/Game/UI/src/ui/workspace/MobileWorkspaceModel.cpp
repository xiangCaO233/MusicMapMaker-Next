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
/// @details 旋转不重置目标状态，业务视图仍保留当前选择。
/// @warning UI 线程本地赋值，不等待平台窗口或交换链重建。
void MobileWorkspaceModel::resize(float width, float height, float scale)
{
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

/// @brief 系统返回与应用边缘返回共用相同的层级规则。
/// @details 抽屉优先收起，已有确认优先取消，其余情况请求退出确认。
/// 这里只记录意图，不终止线程或修改打开的项目。
void MobileWorkspaceModel::requestBack()
{
    // 返回优先结束当前层级，不把收起抽屉解释为退出编辑器。
    if ( m_drawer != MobileDrawer::None ) {
        m_targetOpen     = false;
        m_swallowRelease = true;
    } else if ( m_exitConfirmation ) {
        m_exitConfirmation = false;
    } else {
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
    return m_drawer != MobileDrawer::None || m_dragging || m_backDragging ||
           m_swallowRelease || m_exitConfirmation;
}

/// @brief 每次采样只推进一次手势，归属以最初触摸起点为准。
/// @param popupBlocked 高层业务模态已拥有输入，抽屉不得抢占。
/// @details 拖动直接跟随触点，只有松手后的剩余距离执行动画。
/// @warning 每帧固定分支与数学运算，不创建任务或执行同步等待。
void MobileWorkspaceModel::update(MobilePointer pointer, float seconds,
                                  bool popupBlocked)
{
    // 释放当帧仍保持阻挡，下一次采样才允许背景接收新的手势。
    if ( !pointer.m_down && !pointer.m_released ) m_swallowRelease = false;
    if ( popupBlocked || m_exitConfirmation ) return;
    // 模态期间不建立新手势，恢复后必须从下一次按下重新取得所有权。
    // 点击归属在按下时确定，动画中的抽屉仍是一个完整输入层。
    // 即使只露出一条边，外部按下也必须关闭而不是落入背景。
    // 抽屉内部的选择操作不在本模型解释，由各业务视图消费。
    // 中途进入把手不能接管已有的画布手势，只有按下边沿建立所有权。
    if ( pointer.m_pressed ) {
        if ( m_drawer != MobileDrawer::None &&
             !drawerRect().contains(pointer.m_x, pointer.m_y) ) {
            m_targetOpen     = false;
            m_swallowRelease = true;
        } else if ( !m_swallowRelease ) {
            m_startX = pointer.m_x;
            m_startY = pointer.m_y;
            // 系统采用三键导航时仍允许真正的边缘返回手势。
            m_backDragging  = pointer.m_x < 12.0f * m_scale ||
                              pointer.m_x > m_width - 12.0f * m_scale;
            const auto side = leftHandle().contains(pointer.m_x, pointer.m_y)
                                  ? MobileDrawer::Left
                              : rightHandle().contains(pointer.m_x, pointer.m_y)
                                  ? MobileDrawer::Right
                                  : MobileDrawer::None;
            if ( !m_backDragging && m_drawer == MobileDrawer::None &&
                 side != MobileDrawer::None ) {
                m_drawer        = side;
                m_dragging      = true;
                m_startFraction = m_fraction;
            }
        }
    }
    // 位移由固定起点计算，不累计事件差分，不受事件合并和帧率变化影响。
    const float horizontal = pointer.m_x - m_startX;
    // 同时限制水平距离和垂直偏移，普通上下浏览不能解释为返回。
    if ( m_backDragging && pointer.m_released ) {
        m_backDragging   = false;
        m_swallowRelease = true;
        if ( std::abs(horizontal) > 64.0f * m_scale &&
             std::abs(pointer.m_y - m_startY) < 48.0f * m_scale )
            requestBack();
    }
    // 手指仍按下时直接更新展开量，不能用完成动画或网络确认延迟反馈。
    if ( m_dragging ) {
        const float direction = m_drawer == MobileDrawer::Left ? 1.0f : -1.0f;
        m_fraction =
            std::clamp(m_startFraction + direction * horizontal / drawerWidth(),
                       0.0f,
                       1.0f);
        // 轻点完整打开，拖动则按露出比例选择目标；业务选中内容不受影响。
        if ( pointer.m_released || !pointer.m_down ) {
            m_dragging = false;
            // 无释放沿代表触点取消或窗口失焦，不能被当成一次轻点。
            // 保留已拖出的比例阈值，但不会凭空完成点击打开动作。
            m_targetOpen =
                (pointer.m_released && std::abs(horizontal) < 8.0f * m_scale) ||
                m_fraction >= 0.25f;
            m_swallowRelease = true;
        }
    } else {
        // 这里只推进非阻塞动画，连续手势分支直接采用手指位置。
        const float target = m_targetOpen ? 1.0f : 0.0f;
        // 指数收敛不依赖固定帧数，慢帧直接前进到同一时间位置。
        // 只在松手后使用缓动，不能降低连续拖动的本地响应速度。
        m_fraction += (target - m_fraction) *
                      (1.0f - std::exp(-18.0f * std::max(0.0f, seconds)));
        if ( !m_targetOpen && m_fraction < 0.001f ) {
            // 动画归零后才释放抽屉身份，避免残留内容无输入保护。
            // 释放保护独立存在，因此同一松手帧仍不会触发背景编辑。
            m_fraction = 0.0f;
            m_drawer   = MobileDrawer::None;
        }
    }
}
}  // namespace MMM::UI
