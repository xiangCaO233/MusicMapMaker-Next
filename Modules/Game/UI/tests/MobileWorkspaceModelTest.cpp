#include "ui/workspace/MobileWorkspaceModel.h"
#include "log/colorful-log.h"
#include <cmath>
#include <initializer_list>

namespace
{
/// @brief 推进非阻塞动画，不引入墙钟等待或依赖图形驱动。
/// @param model 当前测试独占的模型，无外部订阅和图形资源。
/// @details 合成一秒时间足够收敛，不把动画实现的具体速度当作接口。
/// 测试关心最终输入层级，允许以后调整视觉缓动曲线。
void settle(MMM::UI::MobileWorkspaceModel& model)
{
    // 使用合成帧而非墙钟等待，断言不受测试机速度和 GPU 状态影响。
    for ( int frame = 0; frame < 60; ++frame )
        model.update({}, 1.0f / 60.0f, false);
}

/// @brief 两侧共用标签契约：点击开关、纵向定位、横向划过取消。
/// @param side 独立检查资源和工具入口，防止两侧归属混淆。
/// @details 不依赖绘制像素或墙钟时间，只通过公开几何发送完整触点。
/// @return 点击、定位、遮罩或内容归属有误时返回 false。
bool verifyDrawerGestures(MMM::UI::MobileDrawer side)
{
    using namespace MMM::UI;
    MobileWorkspaceModel model;
    model.resize(1280, 720, 1);
    const auto handle =
        side == MobileDrawer::Left ? model.leftHandle() : model.rightHandle();
    const float x = handle.m_x + handle.m_width * .5f;
    float       y = handle.m_y + handle.m_height * .5f;
    // 从模型矩形取中心而非复制边距常量，避免测试与生产各自使用不同命中区。
    // 纵向位移留在屏幕内部，使边界夹紧不会掩盖独立定位是否生效。
    // 同一个测试分别传入左右侧，另一入口应始终停留在原始中心。
    // 纵向移动立即生效，释放不能同时打开抽屉或连带移动另一侧。
    model.update({ x, y, true, true, false }, .016f, false);
    model.update({ x, y + 80, false, true, false }, .016f, false);
    model.update({ x, y + 80, false, false, true }, .016f, false);
    settle(model);
    const auto moved =
        side == MobileDrawer::Left ? model.leftHandle() : model.rightHandle();
    const auto peer =
        side == MobileDrawer::Left ? model.rightHandle() : model.leftHandle();
    if ( std::abs(moved.m_y - handle.m_y - 80) > .01f ||
         std::abs(peer.m_y - handle.m_y) > .01f ||
         model.drawer() != MobileDrawer::None )
        return false;
    y += 80;
    // 使用跨阈值的移动帧再回到原点，覆盖仅检查最终位移会漏掉的误触。
    // 中途不补第二次按下沿，因此不能让取消手势重新取得点击资格。
    // 释放后的空帧同时确认捕获已清除，后续轻点是完整的新手势。
    // 横拖即使又回到原点也只能取消，防止一次滑动在松手时伪装成轻点。
    model.update({ x, y, true, true, false }, .016f, false);
    model.update({ x + 100, y, false, true, false }, .016f, false);
    model.update({ x, y, false, false, true }, .016f, false);
    settle(model);
    if ( model.drawer() != MobileDrawer::None || model.exitConfirmation() )
        return false;
    // 两像素抖动模拟手指按压的不稳定坐标，不能要求用户绝对静止。
    // 在释放前先断言抽屉仍收起，区分按下激活与确认轻点两种语义。
    // 按下当帧就需要阻挡背景，否则松手检查通过也可能已经落下物件。
    // 小幅抖动仍是一次点击；按下先保护画布，释放后才提交打开。
    model.update({ x, y, true, true, false }, .016f, false);
    if ( !model.blocksCanvasInput() || model.drawer() != MobileDrawer::None )
        return false;
    model.update({ x + 2, y + 2, false, false, true }, .016f, false);
    settle(model);
    if ( model.drawer() != side || model.fraction() < .99f ) return false;
    // 内容坐标与外沿标签错开，确保文件列表或音轨控件保留输入所有权。
    // 终点的横向位移足以越过标签方向阈值，但不能因此重判起点归属。
    // 抽屉仍全开意味着背景滑动没有偷偷改变它的目标状态。
    // 内容起手后的水平拖动只属于业务内容，不得中途变成抽屉开关。
    const auto  drawer   = model.drawerRect();
    const float contentX = drawer.m_x + drawer.m_width * .5f;
    model.update({ contentX, 200, true, true, false }, .016f, false);
    model.update({ contentX + 180, 200, false, false, true }, .016f, false);
    settle(model);
    if ( model.drawer() != side ) return false;
    // 打开后入口跟随面板外沿，不能继续用收起状态的屏幕坐标测试。
    // 向上移动与前面的向下移动互补，避免实现仅正确处理正向位移。
    // 这里没有模拟系统滚动，验证的是标签自身位置与抽屉展开量相互独立。
    // 展开后的标签仍可上下移动，抽屉本身保持全开，不被拖出半开状态。
    const auto  grip = model.drawerGrip();
    const float gx   = grip.m_x + grip.m_width * .5f;
    float       gy   = grip.m_y + grip.m_height * .5f;
    model.update({ gx, gy, true, true, false }, .016f, false);
    model.update({ gx, gy - 40, false, true, false }, .016f, false);
    model.update({ gx, gy - 40, false, false, true }, .016f, false);
    settle(model);
    if ( model.drawer() != side || model.fraction() < .99f ) return false;
    gy -= 40;
    // 取消横拖后仍能立即点击，检测吞掉释放却永久阻挡下一触点的回归。
    // 点击收起的释放帧仍保护背景，动画结束后才恢复普通画布输入。
    // 左右方向一致的关闭契约，不依赖当前面板是资源页还是工具页。
    // 反向横拖不能再关闭；保持原标签位置，随后正常点击即可收起。
    model.update({ gx, gy, true, true, false }, .016f, false);
    model.update({ gx - 200, gy, false, false, true }, .016f, false);
    settle(model);
    if ( model.drawer() != side ) return false;
    model.update({ gx, gy, true, true, false }, .016f, false);
    model.update({ gx, gy, false, false, true }, .016f, false);
    if ( !model.blocksCanvasInput() ) return false;
    settle(model);
    return model.drawer() == MobileDrawer::None && !model.blocksCanvasInput();
}

/// @brief 模态、旋转、取消不能伪造点击；系统边缘不再由应用识别返回。
/// @details 退出确认只能来自平台通知，手指从最外缘滑入不能自行触发它。
/// @return 中断后遗留捕获或边缘误识别时返回 false。
bool verifyGestureCancellation()
{
    using namespace MMM::UI;
    MobileWorkspaceModel model;
    model.resize(1280, 720, 1);
    // 边缘坐标故意位于标签外，应用模型不应模拟系统返回识别器。
    // 两个方向都滑到屏幕中间，足够跨过旧版自定义返回的距离阈值。
    // 测试不要求平台真的返回；原生通知在后面的独立步骤模拟。
    // 两侧最外缘的触摸既不抢占画布，也不产生应用导航意图。
    for ( const float edge : { 2.0f, 1278.0f } ) {
        model.update({ edge, 200, true, true, false }, .016f, false);
        if ( model.blocksCanvasInput() ) return false;
        model.update({ 640, 200, false, false, true }, .016f, false);
        settle(model);
        if ( model.exitConfirmation() || model.drawer() != MobileDrawer::None )
            return false;
    }
    const auto  handle = model.leftHandle();
    const float x      = handle.m_x + handle.m_width * .5f;
    const float y      = handle.m_y + handle.m_height * .5f;
    // 触点取消可能来自系统导航抢占或失焦，均不会保证收到正常松手。
    // 空采样不是用户点击，不能用补交释放的方法使此测试通过。
    // 随后的空帧还检查没有旧的延迟开合意图残留。
    // 无释放沿意味着取消，不得自动补出一次点击打开。
    model.update({ x, y, true, true, false }, .016f, false);
    model.update({}, .016f, false);
    settle(model);
    if ( model.blocksCanvasInput() || model.drawer() != MobileDrawer::None )
        return false;
    // 业务弹窗在按住期间出现，必须从这一帧起优先拥有触点。
    // 模态解除后原释放仍被丢弃，不把不同层级的半次操作拼成点击。
    // 抽屉和背景都不应因模态中断而永久占住输入。
    // 平台模态接管后，原触点的后续释放无权再切换标签。
    model.update({ x, y, true, true, false }, .016f, false);
    model.update({ x, y, false, true, false }, .016f, true);
    model.update({ x, y, false, false, true }, .016f, false);
    settle(model);
    if ( model.blocksCanvasInput() || model.drawer() != MobileDrawer::None )
        return false;
    // 屏幕宽高交换后沿用旧坐标释放，专门检查旋转入口有没有清除捕获。
    // 不先发送取消帧，否则会掩盖 resize 本身未处理旧手势的问题。
    // 旋转后的空闲状态需要同时满足不展开、不阻挡两项约束。
    // 旋转改变命中几何，必须丢弃旧触点，而不是让其释放触发新位置。
    model.update({ x, y, true, true, false }, .016f, false);
    model.resize(720, 1280, 1);
    model.update({ x, y, false, false, true }, .016f, false);
    settle(model);
    if ( model.blocksCanvasInput() || model.drawer() != MobileDrawer::None )
        return false;
    // 平台通知和触摸坐标分开，避免重新引入两套返回手势的竞争。
    // 取消确认不能生成可消费的退出请求，也不能保留上一手势的释放保护。
    // 这些状态只由当前 UI 线程模型访问，不需要测试跨线程调度。
    // 平台返回仍保持原来的确认契约；这里只记录意图，不终止测试进程。
    model.requestBack();
    if ( !model.exitConfirmation() ) return false;
    model.finishExitConfirmation(false);
    settle(model);
    return !model.exitConfirmation() && !model.takeExitRequest();
}

/// @brief 验证工具栏显示、缩放和把手移动后，绘制与触摸入口始终分离。
/// @details 覆盖真机高密度屏、不同配置留白以及三个纵向停放位置。
/// 只断言几何不相交与实际手势结果，不在测试中复制把手布局公式。
/// @return 遮挡工具、恢复位置或移动后触点归属有误时返回 false。
bool verifyToolbarClearance()
{
    using namespace MMM::UI;
    // 尺寸使用同一倍率转换，包含真机 2.75 倍内容缩放的工作区比例。
    // 小于一与非整数倍率分别检查最小触摸尺寸和分数像素计算。
    // 不用屏幕物理像素替代内容倍率，二者混用会让高密度设备再次产生遮挡。
    for ( const float scale : { .75f, 1.0f, 2.75f } ) {
        // 工具栏包含按钮和双侧可配置留白，不能只避开固定的按钮宽度。
        // 零留白与较大留白均验证真实保留区域，而非某一皮肤下碰巧不相交。
        for ( const float padding : { 0.0f, 8.0f, 20.0f } ) {
            // 保留非中心位置，检测工具栏显隐是否无意重置已保存的把手偏好。
            for ( const float center : { .2f, .5f, .8f } ) {
                // 每个场景独占模型，避免上一场景的动画或释放保护掩盖新命中错误。
                MobileWorkspaceModel model;
                const float          width        = 980 * scale;
                const float          height       = 440 * scale;
                const float          toolbarWidth = (64 + 2 * padding) * scale;
                model.restoreHandleCenters(.5f, center);
                model.resize(width, height, scale);
                const auto original = model.rightHandle();
                const auto left     = model.leftHandle();
                // 先提供无工具栏的屏幕，再显示工具栏，覆盖实际开关引发的几何变化。
                // 不能一开始就只构造最终尺寸，否则无法发现旧触点区域残留。
                model.resize(width, height, scale, toolbarWidth);
                const auto moved = model.rightHandle();
                // 任意纵向停放位置都必须避开工具栏，不依赖默认中点碰巧有空白。
                // 把手原有触摸尺寸与左把手位置不能因右侧保留区而缩水或联动。
                if ( moved.m_x + moved.m_width > width - toolbarWidth ||
                     moved.m_width != original.m_width ||
                     moved.m_height != original.m_height ||
                     moved.m_y != original.m_y ||
                     model.leftHandle().m_x != left.m_x ||
                     model.leftHandle().m_y != left.m_y )
                    return false;
                // 原右缘位置现在属于工具栏，不能仍被隐藏的旧把手拦截。
                // 该点远离系统最外缘返回区，因此输入阻挡只能来自错误的把手归属。
                const float oldX = original.m_x + original.m_width * .5f;
                const float y    = moved.m_y + moved.m_height * .5f;
                model.update({ oldX, y, true, true, false }, .016f, false);
                if ( model.blocksCanvasInput() ) return false;
                model.update({ oldX, y, false, false, true }, .016f, false);
                settle(model);
                // 新矩形必须是真正的可操作入口，不能只移动绘制而遗留旧命中区。
                // 按下与释放分帧提交，避免零位移点击被触点取消流程替代。
                const float x = moved.m_x + moved.m_width * .5f;
                model.update({ x, y, true, true, false }, .016f, false);
                model.update({ x, y, false, false, true }, .016f, false);
                settle(model);
                if ( model.drawer() != MobileDrawer::Right ) return false;
                // 从真正打开过的抽屉返回，下一次拖动必须再次属于收起把手。
                // 用合成帧收敛动画，不能在测试中引入平台等待或固定墙钟延迟。
                model.closeDrawer();
                settle(model);
                // 上下拖动继续在工具栏左侧进行，且不会改变归属或打开抽屉。
                // 位移超过方向锁阈值，三个中心处又都不触及屏幕上下边界。
                // 因而必须完整移动给定距离，不能让边界夹取掩盖错误的中心换算。
                model.update({ x, y, true, true, false }, .016f, false);
                model.update(
                    { x, y + 20 * scale, false, true, false }, .016f, false);
                model.update(
                    { x, y + 20 * scale, false, false, true }, .016f, false);
                settle(model);
                if ( model.drawer() != MobileDrawer::None ||
                     std::abs(model.rightHandle().m_y - moved.m_y -
                              20 * scale) > .01f )
                    return false;
                // 隐藏工具栏后回到右缘，只恢复横向位置，不覆盖用户新纵向偏好。
                // 使用刚完成的拖动结果比较，不能只检查最初写入的配置值。
                const auto repositioned = model.rightHandle();
                model.resize(width, height, scale);
                const auto hidden = model.rightHandle();
                if ( hidden.m_x != original.m_x ||
                     hidden.m_y != repositioned.m_y )
                    return false;
            }
        }
    }
    return true;
}
}  // namespace

/// @brief 验证启动收起、把手归属、遮罩关闭以及返回确认的状态边界。
/// @details 只测试纯模型；真实窗口层级另外在 Android 模拟器验收。
/// 所有触点使用工作区坐标，不依赖模拟器窗口在桌面上的摆放。
/// 每个手势都区分按下和释放，防止测试跳过关键的背景阻挡阶段。
/// @return 失败断言数非零时返回失败，不使用异常作为断言通道。
int main()
{
    using namespace MMM::UI;
    // 保留首个失败之后的检查，单次运行可以展示不同状态边界的回归。
    int  failures = 0;
    auto check    = [&](bool valid, const char* message) {
        if ( !valid ) {
            ++failures;
            XERROR("Mobile workspace: {}", message);
        }
    };
    MobileWorkspaceModel model;
    // 独立于 ImGui 创建模型，输入归属的核心不需要平台窗口支持。
    model.resize(1280, 720, 1);
    check(model.drawer() == MobileDrawer::None && !model.blocksCanvasInput(),
          "startup is collapsed");
    // 把手提供完整手指命中面积，但避开操作系统最外侧返回区。
    const auto handle = model.leftHandle();
    check(handle.m_width >= 48 && handle.m_height >= 48 && handle.m_x >= 12,
          "touch target and back edge are separate");
    const float x = handle.m_x + handle.m_width * .5f;
    const float y = handle.m_y + handle.m_height * .5f;
    model.update({ x, y, true, true, false }, 1.0f / 60, false);
    // 在释放前就检查保护，防止第一帧已经往背景画布落下物件。
    check(model.blocksCanvasInput(), "handle down blocks canvas immediately");
    // 零位移释放代表轻点，与后续直接跟随手指的拖动语义分开。
    model.update({ x, y, false, false, true }, 1.0f / 60, false);
    settle(model);
    // 收敛后检查目标，而非依赖某个中间帧的浮点舍入结果。
    check(model.drawer() == MobileDrawer::Left && model.fraction() > .99f,
          "tap opens left drawer");
    // 左抽屉宽度小于视口，选取确定落在其外侧的坐标进行关闭。
    // 遮罩按下必须先关闭两级抽屉，该次按下与松开不能落到画布。
    model.update({ 1100, 300, true, true, false }, 1.0f / 60, false);
    check(model.blocksCanvasInput(), "outside down is consumed");
    model.update({ 1100, 300, false, false, true }, 1.0f / 60, false);
    // 松手帧需要继续阻挡，下一帧才可建立新的画布手势。
    check(model.blocksCanvasInput(), "outside release is consumed");
    // 收起动画没有隐藏的同步等待，继续喂入空帧即可回到初始状态。
    settle(model);
    check(model.drawer() == MobileDrawer::None && !model.blocksCanvasInput(),
          "closed drawer releases next gesture");
    // 原始画布区域不是抽屉触发区，普通绘制不能意外拉开抽屉。
    model.update({ 640, 300, true, true, false }, 1.0f / 60, false);
    // 起点位于画布中央，即使水平移动也不能被当成边缘手势。
    model.update({ 800, 300, false, false, true }, 1.0f / 60, false);
    check(model.drawer() == MobileDrawer::None,
          "canvas drag does not open drawer");
    model.update({}, 1.0f / 60, false);
    // 返回手势必须从外缘开始，不复用普通画布拖动的中间坐标。
    // 系统返回通知负责导航，模型不再自行监听与系统冲突的边缘滑动。
    model.requestBack();
    // 只检查出现确认，而不是用退出请求代替确认窗口状态。
    check(model.exitConfirmation() && !model.takeExitRequest(),
          "platform back asks instead of exiting");
    model.finishExitConfirmation(false);
    // 取消只结束确认状态，绝不能留下下一帧可消费的退出意图。
    settle(model);
    check(!model.exitConfirmation() && !model.takeExitRequest(),
          "cancel returns to editor");
    model.requestBack();
    // 直接返回请求模拟系统回调，边缘手势和平台入口必须共用结果。
    // 正常关闭通道只消费一次确认，避免重复发布原生关闭事件。
    model.finishExitConfirmation(true);
    check(model.takeExitRequest() && !model.takeExitRequest(),
          "confirmed exit is consumed once");
    settle(model);
    // 高层业务模态出现时，把手不能越过其输入所有权。
    model.update({ x, y, true, true, false }, 1.0f / 60, true);
    // 被业务模态遮住时，相同把手坐标也不能取得手势所有权。
    check(model.drawer() == MobileDrawer::None, "business modal has priority");
    model.resize(320, 720, 1);
    // 小屏展开宽度保留外侧遮罩，否则只能依赖系统返回才能关闭。
    check(model.drawerWidth() < 320 && model.drawerWidth() > 240,
          "portrait keeps outside dismissal area");
    // 触点丢失按取消处理，不能因为位移为零而误识别成点击。
    MobileWorkspaceModel cancelled;
    // 单独实例避免前一个测试的释放保护掩盖取消行为。
    cancelled.resize(1280, 720, 1);
    cancelled.update({ x, y, true, true, false }, 1.0f / 60, false);
    cancelled.update({}, 1.0f / 60, false);
    settle(cancelled);
    check(cancelled.drawer() == MobileDrawer::None,
          "cancelled touch does not tap open");
    // 与左抽屉相同的返回层级也适用于右侧开关抽屉。
    const auto right = cancelled.rightHandle();
    // 镜像坐标从模型获取，不在测试中复制右把手的布局公式。
    const float rx = right.m_x + right.m_width * .5f;
    cancelled.update({ rx, y, true, true, false }, 1.0f / 60, false);
    cancelled.update({ rx, y, false, false, true }, 1.0f / 60, false);
    settle(cancelled);
    check(cancelled.drawer() == MobileDrawer::Right,
          "right handle opens controls");
    cancelled.requestBack();
    // 先消费抽屉层级，不能第一次返回就跨过它询问退出。
    settle(cancelled);
    check(cancelled.drawer() == MobileDrawer::None &&
              !cancelled.exitConfirmation(),
          "back closes drawer before asking exit");
    // 测试进程正常返回，不通过终止进程或实际关闭 Activity 验证模型。
    // 真实退出按钮和 JNI 桥接留给模拟器交互验证。
    check(verifyDrawerGestures(MobileDrawer::Left),
          "left drawer gesture ownership");
    check(verifyDrawerGestures(MobileDrawer::Right),
          "right drawer gesture ownership");
    check(verifyGestureCancellation(),
          "cancelled and interrupted gestures release input");
    check(verifyToolbarClearance(),
          "toolbar and movable right handle keep separate touch regions");
    // 字号或英文文案增大标签后，纯模型必须保持与绘制一致的命中几何。
    MobileWorkspaceModel largeLabels;
    largeLabels.resize(1280, 720, 2, 100, 110, 90);
    check(
        largeLabels.leftHandle().m_width == 220 &&
            largeLabels.leftHandle().m_height == 180 &&
            largeLabels.rightHandle().m_x + largeLabels.rightHandle().m_width <=
                1180,
        "localized labels preserve full touch targets and toolbar clearance");
    // 模拟新进程读取独立客户端值，恢复后仍保持抽屉收起。
    MobileWorkspaceModel restored;
    // 使用全新模型，避免前一手势的释放保护或动画状态参与恢复结果。
    restored.restoreHandleCenters(.2f, .8f);
    restored.resize(1280, 720, 1);
    check(restored.drawer() == MobileDrawer::None &&
              restored.leftHandle().m_y < restored.rightHandle().m_y,
          "restart restores independent handles without opening drawer");
    // 旋转只重新映射像素位置，不把归一化中心重置到屏幕正中。
    restored.resize(720, 1280, 1);
    // 先恢复后提供屏幕尺寸也必须有效，模拟配置早于平台窗口准备完成。
    // 只比较布局状态，不将打开抽屉的运行中手势写入持久化。
    check(restored.leftHandleCenter() == .2f &&
              restored.rightHandleCenter() == .8f &&
              restored.rightHandle().m_y + restored.rightHandle().m_height <=
                  1280,
          "rotation preserves restored positions inside screen");
    return failures == 0 ? 0 : 1;
}
