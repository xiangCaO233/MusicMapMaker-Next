#include "canvas/CameraLayoutEditor.h"
#include "canvas/CameraLayoutMath.h"
#include "config/VisualConfig.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/vec4.hpp>
#include <string>

namespace MMM::Canvas
{
/// @brief 在工具栏附着窗口内仅绘制布局模式切换。
/// @warning 每帧仅绘制固定控件；不新建窗口，不保存配置到磁盘。
void CameraLayoutEditor::drawControls(Config::VisualConfig&)
{
    // 元素模式只覆盖观察矩阵，不复位已修改的目标相机。
    // 当前手势已经写入的内存状态会保留。
    // 预览不再提交到 ImGui 后，正常窗口生命周期会将其隐藏。
    if ( UI::FeedbackButton("画布元素") && m_cameraMode ) {
        // 模式切换提交当前姿态并立即释放输入；预览窗口本帧不再提交。
        m_cameraMode = false;
        m_axis       = -1;
        m_orbiting   = false;
    }
    ImGui::SameLine();
    // 进入相机模式时重新取得独立手势状态。
    // 旧元素布局的鼠标所有权不能转为新的相机拖动。
    // 切换本身不移动目标相机。
    if ( UI::FeedbackButton("摄像机位置") && !m_cameraMode ) {
        m_cameraMode = true;
        m_axis       = -1;
        m_orbiting   = false;
    }
}

/// @brief 参数窗口不持有 GPU 资源，销毁仅释放自己的 Clay 内存。
CameraLayoutEditor::~CameraLayoutEditor()
{
    if ( m_parameterLayout.context )
        UI::CLayWrapperCore::instance().destroyWindowContext(m_parameterLayout);
}

/// @brief Clay 负责纵向分组与按钮等宽排列，ImGui 负责控件交互。
/// @param id 当前画布的稳定身份，用于隔离独立参数窗口。
/// @param visual 与三维操作空间共用的可编辑姿态配置。
/// @pre 调用方已经启用相机模式并完成旧配置迁移。
/// @details 本函数不改变当前布局模式；窗口只提供姿态和辅助导航控件。
/// 所有按钮与数值回调均在返回前同步执行，不向其他线程排队。
/// 配置持久化仍由退出布局的既有流程处理，不在数值拖动期间写盘。
/// 折叠时仍配对结束 ImGui 窗口，但不重建不可见的控件布局。
/// 每次完成布局后恢复调用方 Clay 上下文，允许多个画布依次绘制。
/// @warning 只在相机模式绘制；窗口关闭和恢复不访问磁盘或逻辑会话。
void CameraLayoutEditor::drawParameterWindow(const std::string&    id,
                                             Config::VisualConfig& visual)
{
    // 参数和预览由同一个模式分支驱动，不提供独立关闭按钮。
    // 稳定 ID 隔离多个谱面，窗口位置和大小允许用户自行调整。
    // 不把窗口开关存入视觉配置，否则重启后可能只有参数窗口仍然打开。
    // NoFocusOnAppearing 防止切入相机模式时抢走画布键盘焦点。
    // 折叠窗口只停止控件绘制，仍保留相机模式与预览。
    // 模式退出则由 update 统一释放上下文，不依赖窗口是否折叠。
    const auto title = std::string("摄像机参数###CameraLayoutParameters_") + id;
    // 初始尺寸跟随字体缩放，后续帧尊重用户手动调整的大小。
    // 最小宽度保证三列数值仍可输入，不以缩小字体来挤压控件。
    // 最小高度允许窗口比内容短，超出的行由普通窗口滚动访问。
    const float scale = std::max(1.0F, ImGui::GetFontSize() / 16.0F);
    ImGui::SetNextWindowSize({ 460 * scale, 340 * scale },
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({ 340 * scale, 200 * scale },
                                        { 10000, 10000 });
    if ( ImGui::Begin(title.c_str(),
                      nullptr,
                      ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing) ) {
        // 必须恢复调用方的 Clay 上下文，避免污染其它窗口尚未完成的布局。
        // 单独 Arena 只在进入模式时建立，稳定帧重复使用。
        // createWindowContext 会自动切换当前上下文，必须提前保存旧值。
        // 各谱面拥有不同 Arena，但所有布局计算都在 UI 线程串行执行。
        // 不把 Clay 的内部元素地址保留到下一帧或暴露给逻辑线程。
        // 没有文本测量节点，因此控件字体仍完全由当前 ImGui 样式决定。
        auto& core     = UI::CLayWrapperCore::instance();
        auto* previous = Clay_GetCurrentContext();
        if ( !m_parameterLayout.context )
            m_parameterLayout = core.createWindowContext();
        core.makeCurrent(m_parameterLayout.context);
        // 布局树描述本帧几何，不保存视觉配置的副本。
        // 这样坐标轴拖动与精确输入始终读写同一份姿态数据。
        // 子布局在栈上持有，父容器只借用它们的地址。
        // 所有回调必须在本作用域结束前执行完毕。
        auto root    = UI::CLayBox::VBox();
        auto tools   = UI::CLayBox::HBox();
        auto actions = UI::CLayBox::HBox();
        // 行高包含当前 FramePadding，保证按钮与输入框有相同交互高度。
        // Clay 的节点间距复用主题 ItemSpacing，不额外叠加 ImGui 空白行。
        // Grow 只分配横向剩余空间，纵向使用实际控件需要的固定高度。
        const float rowHeight = ImGui::GetFrameHeight();
        const auto gap = static_cast<uint16_t>(ImGui::GetStyle().ItemSpacing.y);
        root.setSpacing(gap);
        tools.setSpacing(gap);
        actions.setSpacing(gap);
        // 两组按钮使用均分宽度，窗口缩放时不依赖 SameLine 的隐式宽度。
        // 回调在本函数返回前执行，借用当前配置和编辑器即可。
        // 不使用 Clay 悬停标志触发操作，点击归属仍由 ImGui 判断。
        // FeedbackButton 保留统一的点击音效与悬浮反馈。
        // 切换工具只清除旧事务，不初始化新的鼠标拖动起点。
        tools.addElement(
            "move",
            UI::Sizing::Grow(),
            UI::Sizing::Fixed(rowHeight),
            [&](Clay_BoundingBox rect, bool) {
                if ( UI::FeedbackButton("移动 (G)",
                                        { rect.width, rect.height }) ) {
                    m_rotate = false;
                    m_axis   = -1;
                }
            });
        tools.addElement(
            "rotate",
            UI::Sizing::Grow(),
            UI::Sizing::Fixed(rowHeight),
            [&](Clay_BoundingBox rect, bool) {
                if ( UI::FeedbackButton("旋转 (R)",
                                        { rect.width, rect.height }) ) {
                    m_rotate = true;
                    m_axis   = -1;
                }
            });
        root.addLayout(
            "tools", tools, UI::Sizing::Grow(), UI::Sizing::Fixed(rowHeight));
        // 标签在输入行上方，数值框获得完整宽度；XYZ 由 DragFloat3 等宽分配。
        // 固定两行高度根据字体计算，不裁掉 DPI 放大后的文字。
        // 可见标签与隐藏 ID 分离，标签宽度不占用三列数值框空间。
        // 两组隐藏 ID 不同，键盘输入状态不会在位置与旋转之间复用。
        // 每组绑定固定长度数组，回调期间不改变其存储位置。
        root.addElement(
            "position",
            UI::Sizing::Grow(),
            UI::Sizing::Fixed(rowHeight +
                              ImGui::GetTextLineHeightWithSpacing()),
            [&](Clay_BoundingBox rect, bool) {
                ImGui::TextUnformatted("位置 XYZ");
                ImGui::SetNextItemWidth(rect.width);
                // Ctrl+点击支持精确输入，数值范围与三维拖动保持一致。
                if ( ImGui::DragFloat3("##CameraPosition",
                                       visual.canvasCameraPosition.data(),
                                       0.005F,
                                       -100,
                                       100,
                                       "%.4f",
                                       ImGuiSliderFlags_AlwaysClamp) )
                    m_axis = -1;
            });
        root.addElement(
            "rotation",
            UI::Sizing::Grow(),
            UI::Sizing::Fixed(rowHeight +
                              ImGui::GetTextLineHeightWithSpacing()),
            [&](Clay_BoundingBox rect, bool) {
                ImGui::TextUnformatted("旋转 XYZ（度）");
                ImGui::SetNextItemWidth(rect.width);
                // 欧拉角沿用已有定义；数值编辑接管输入后终止旧拖动事务。
                // 这里显示角度而不是弧度，矩阵转换仍集中在相机数学入口。
                // AlwaysClamp 同时约束拖动和键盘输入，避免两种操作范围不同。
                // 不从辅助观察相机反推数值，避免中键导航改写实际视角。
                if ( ImGui::DragFloat3("##CameraRotation",
                                       visual.canvasCameraRotation.data(),
                                       0.25F,
                                       -180,
                                       180,
                                       "%.3f",
                                       ImGuiSliderFlags_AlwaysClamp) )
                    m_axis = -1;
            });
        actions.addElement(
            "reset",
            UI::Sizing::Grow(),
            UI::Sizing::Fixed(rowHeight),
            [&](Clay_BoundingBox rect, bool) {
                // 复位只改变相机，不改变谱面或画布组件布局。
                // 保留自定义姿态开关，默认正视效果由标准位置和零旋转表达。
                // 取消旧事务防止下一帧从拖动起点覆盖本次复位结果。
                if ( UI::FeedbackButton("重置相机",
                                        { rect.width, rect.height }) ) {
                    visual.canvasCameraPosition = { 0, 0, 1 };
                    visual.canvasCameraRotation = { 0, 0, 0 };
                    m_axis                      = -1;
                }
            });
        actions.addElement(
            "locate",
            UI::Sizing::Grow(),
            UI::Sizing::Fixed(rowHeight),
            [&](Clay_BoundingBox rect, bool) {
                // 定位仅改变辅助观察中心，不改变真实相机姿态。
                // 用于找回已经移出操作空间视野的相机句柄。
                // 辅助观察距离不是焦距，不写入可持久化的 VisualConfig。
                if ( UI::FeedbackButton("定位相机",
                                        { rect.width, rect.height }) ) {
                    const auto& p = visual.canvasCameraPosition;
                    m_center      = { p[0], p[1], p[2] };
                    m_distance    = 3;
                }
            });
        root.addLayout("actions",
                       actions,
                       UI::Sizing::Grow(),
                       UI::Sizing::Fixed(rowHeight));
        // 长度是可见范围，不是变换缩放；变化后由逻辑线程补齐远端物件。
        // 拖动逐帧更新预览；Ctrl+点击允许输入精确数值，无需 Enter 才生效。
        // 单位始终是原窗口高度，不受相机位置、俯仰或滚动速度影响。
        // 只允许向远端延长，避免缩短到判定线之前产生不可编辑区域。
        // 用局部值接收输入，拒绝非法值后才改变权威视觉配置。
        // 静止或抵达边界时不产生变化，调用方不重复发布配置。
        // 切回元素布局不清空长度，退出调整后仍使用最后调整值。
        root.addElement(
            "length",
            UI::Sizing::Grow(),
            UI::Sizing::Fixed(rowHeight +
                              ImGui::GetTextLineHeightWithSpacing()),
            [&](Clay_BoundingBox rect, bool) {
                ImGui::TextUnformatted("画布长度（拖动调整，Ctrl+点击输入）");
                ImGui::SetNextItemWidth(rect.width);
                float length = visual.canvasCameraLength;
                // 拒绝非有限输入，保持投影与候选区间查询的有限边界。
                if ( ImGui::DragFloat("##CanvasLength",
                                      &length,
                                      0.02F,
                                      1.0F,
                                      32.0F,
                                      "%.3f",
                                      ImGuiSliderFlags_AlwaysClamp) &&
                     std::isfinite(length) )
                    visual.canvasCameraLength = std::clamp(length, 1.0F, 32.0F);
            });
        // 补偿逐次写入本地配置，由宿主捕获布局预设并发布最新逻辑快照。
        // 两个参数分别控制投影与局部几何，不借用已有长度或皮肤缩放值。
        // 默认值为零，旧版用户升级后不会被主动改变已有透视效果。
        // 滑块保留连续反馈，保存由宿主的变化边沿合并到后台配置队列。
        // 从内置布局开始调节时仍走自动复制流程，不修改代码定义的方案。
        // 每项说明使用独立行，避免窄窗口把标签挤到画布预览上。
        // 控件内部 ID 与显示说明分开，文字更新不会打断当前手势。
        // 高度仅增加平面可见厚度，不改变 Key 音效或物件的播放时间。
        // 两个补偿都不需要重新解码皮肤纹理或重建 Vulkan 离屏资源。
        // 运动改变共同投影；厚度只改变物件局部高度，不改变谱面时间。
        root.addElement(
            "compensation",
            UI::Sizing::Grow(),
            UI::Sizing::Fixed(rowHeight * 6),
            [&](Clay_BoundingBox rect, bool) {
                ImGui::TextUnformatted("3D 运动补偿");
                ImGui::SetNextItemWidth(rect.width);
                UI::FeedbackSliderFloat("##MotionCompensation",
                                        &visual.canvasMotionCompensation,
                                        0,
                                        100,
                                        "%.0f");
                ImGui::TextUnformatted("0 原始透视 · 100 纵向匀速");
                ImGui::TextUnformatted("3D 厚度补偿");
                ImGui::SetNextItemWidth(rect.width);
                UI::FeedbackSliderFloat("##ThicknessCompensation",
                                        &visual.canvasThicknessCompensation,
                                        0,
                                        300,
                                        "%.0f");
                ImGui::TextUnformatted(
                    "额外高度：0 原始 · 100 双倍 · 300 四倍");
            });
        // 提示参与同一纵向布局，窄窗口使用滚动而不挤压精确数值行。
        root.addElement(
            "help",
            UI::Sizing::Grow(),
            UI::Sizing::Fixed(ImGui::GetTextLineHeightWithSpacing() * 4),
            [](Clay_BoundingBox, bool) {
                ImGui::TextUnformatted("中键旋转 · Shift+中键平移 · 滚轮缩放");
                ImGui::TextUnformatted(
                    "G/R 切换工具 · 拖动轴/环调整 · Esc 取消");
                ImGui::TextUnformatted(
                    "Ctrl+点击数值精确输入；X 红 / Y 绿 / Z 蓝");
                ImGui::TextUnformatted(
                    "画布中心为原点，高度为 1；Z 垂直画布向外");
            });
        // Fit 高度让窗口小于内容时保留纵向滚动，不把行压缩为零。
        // 嵌套容器必须存活到 renderInCurrent 完成，不能提前析构。
        root.renderInCurrent(ImGui::GetCursorScreenPos(),
                             { ImGui::GetContentRegionAvail().x, 0 });
        // 恢复必须发生在参数窗口返回之前，后续预览或其它 UI 可以继续使用
        // 自己的上下文；参数窗口的 Arena 则保留给下次可见帧复用。
        core.makeCurrent(previous);
    }
    ImGui::End();
}

/// @brief 更新三维操作空间和实际视角预览，元素模式不覆盖画布。
/// @warning 每帧固定规模绘制；仅模式进入和退出创建、释放 Clay 上下文。
bool CameraLayoutEditor::update(const std::string& id, ImVec2 origin,
                                ImVec2 size, ImTextureID texture,
                                Config::VisualConfig& visual)
{
    if ( !m_cameraMode ) {
        // 与预览一样停止提交参数窗口，同时释放其专用布局内存。
        // 返回相机模式时重新创建，不保留失效的 Clay 元素状态。
        if ( m_parameterLayout.context )
            UI::CLayWrapperCore::instance().destroyWindowContext(
                m_parameterLayout);
        return false;
    }
    // 工具栏可能晚于画布更新；首次进入也须先迁移旧相机参数。
    enableCanvasCameraPose(visual);
    // 三维辅助几何写入父窗口，子窗口始终在上方，不被辅助背景覆盖。
    drawScene(origin, size, visual);
    drawParameterWindow(id, visual);
    const auto title = std::string("实际视角预览###CameraLayoutPreview_") + id;
    // 预览首次显示采用小窗口，后续允许自由调整尺寸。
    // 不创建独立纹理加载器或会话快照消费者。
    // 模式退出后也没有需要等待 GPU 的专属资源。
    ImGui::SetNextWindowSize({ 420, 360 }, ImGuiCond_FirstUseEver);
    // 预览窗口由模式生命周期控制，不设置独立关闭按钮或保存停靠状态。
    // 它只显示最终成像，操作仍由左侧三维视图完成。
    if ( ImGui::Begin(title.c_str(),
                      nullptr,
                      ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing) ) {
        // 预览使用主画布宽高比计算显示尺寸。
        // 改变预览窗口大小不会重新定义相机视野。
        // 不足的边缘空间留白，避免拉伸造成误判。
        const auto  available = ImGui::GetContentRegionAvail();
        const float scale =
            std::max(0.0F,
                     std::min(available.x / std::max(1.0F, size.x),
                              available.y / std::max(1.0F, size.y)));
        const ImVec2 previewSize{ size.x * scale, size.y * scale };
        // 保持主画布宽高比，预览不能通过拉伸制造与退出后的不同效果。
        if ( texture && scale > 0 ) ImGui::Image(texture, previewSize);
    }
    ImGui::End();
    return true;
}

/// @brief 绘制世界坐标轴和相机对象，平移沿投影轴，旋转沿世界轴累积。
/// @warning 固定网格和三组圆环；不分配场景对象，不写盘，不建立逻辑命令。
void CameraLayoutEditor::drawScene(ImVec2 origin, ImVec2 size,
                                   Config::VisualConfig& visual)
{
    // 所有鼠标位置与辅助图元使用逻辑像素。
    // 不混用原生窗口的物理坐标。
    // 因此 DPI 改变不会使绘制位置与命中区域分离。
    auto&           io = ImGui::GetIO();
    const glm::vec2 top{ origin.x, origin.y }, extent{ size.x, size.y };
    const glm::vec2 mouse{ io.MousePos.x, io.MousePos.y };
    // 父窗口悬停排除了控制子窗口及上方弹窗。
    // 活动输入框拥有优先权，不能隔着输入框操作相机。
    // 开始手势仍须位于真实画布矩形内。
    const bool hovered =
        ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(origin,
                                   { origin.x + size.x, origin.y + size.y }) &&
        !ImGui::IsAnyItemActive();
    // 中键只能在画布按下时获得所有权；悬停其它面板不能开始导航。
    if ( !ImGui::IsMouseDown(ImGuiMouseButton_Middle) ) m_orbiting = false;
    if ( hovered && m_axis < 0 &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Middle) )
        m_orbiting = true;
    // 导航只能由画布内的中键按下建立。
    // 取得所有权后允许拖出画布，松开立即结束。
    // 输入逐帧推进，不使用同步等待或固定延迟。
    if ( m_orbiting ) {
        if ( io.KeyShift ) {
            // 平移沿观察平面进行，距离越远单位像素覆盖的世界范围越大。
            const auto rotation = cameraEulerRotation(
                glm::radians(m_orbit.y), glm::radians(m_orbit.x), 0.0F);
            m_center +=
                glm::vec3(rotation *
                          glm::vec4(-io.MouseDelta.x, io.MouseDelta.y, 0, 0)) *
                (m_distance / std::max(1.0F, size.y));
        } else {
            // 水平角连续转动，俯仰限制在稳定观察区间。
            // 这两个值只影响操作空间的观察方向。
            // 它们绝不能覆盖正在编辑的相机姿态。
            m_orbit.y += io.MouseDelta.x * 0.3F;
            m_orbit.x =
                std::clamp(m_orbit.x + io.MouseDelta.y * 0.3F, -85.0F, 85.0F);
        }
    }
    // 正在变换时不改变辅助视图距离，保持投影尺度稳定。
    // 空闲滚轮只缩放操作空间，不改变谱面时间。
    // 距离下限避免近距离数值放大，上限控制精度损失。
    if ( hovered && m_axis < 0 )
        m_distance = std::clamp(
            m_distance * std::exp2(-io.MouseWheel / 8), 0.5F, 200.0F);
    const auto orbit = cameraEulerRotation(
        glm::radians(m_orbit.y), glm::radians(m_orbit.x), 0.0F);
    const glm::vec3 eye =
        m_center + glm::vec3(orbit * glm::vec4(0, -m_distance, m_distance, 0));
    // 辅助相机以世界 Z 为上方向观察 XY 平面。
    // 目标相机仍按自己的局部上方向成像。
    // 两者分开构造，避免预览受到辅助导航影响。
    const auto view = glm::lookAtRH(eye, m_center, glm::vec3(0, 0, 1));
    auto       projection =
        glm::perspectiveRH_ZO(glm::radians(45.0F),
                              std::max(1.0F, size.x) / std::max(1.0F, size.y),
                              0.01F,
                              1024.0F);
    // 深度范围使用 Vulkan 的零至一约定。
    // 屏幕 Y 翻转只发生在投影层。
    // 世界轴和姿态配置始终使用同一右手坐标系。
    projection[1][1] *= -1;
    const auto matrix = projection * view;
    auto*      draw   = ImGui::GetWindowDrawList();
    // 辅助绘制不能跨出所属画布边界。
    // 此裁剪与预览窗口的裁剪相互独立。
    // 末尾对称弹栈，防止影响后续普通编辑工具。
    draw->PushClipRect(origin, { origin.x + size.x, origin.y + size.y }, true);
    draw->AddRectFilled(origin,
                        { origin.x + size.x, origin.y + size.y },
                        IM_COL32(23, 26, 31, 255));
    // 辅助绘制与命中检测共享本帧矩阵。
    // 近平面后方的端点直接拒绝。
    // 失败的投影不能留下可点击但看不见的句柄。
    const auto project = [&](glm::vec3 p, glm::vec2& result) {
        return projectCameraHandle(matrix, p, top, extent, result);
    };
    // 线段两端都可投影时才生成屏幕图元。
    // 这样不会因背后端点透视除法而出现跨屏长线。
    // 辅助线不查询也不复制谱面顶点缓冲。
    const auto line =
        [&](glm::vec3 a, glm::vec3 b, ImU32 color, float thickness = 1.0F) {
            glm::vec2 pa, pb;
            if ( project(a, pa) && project(b, pb) )
                draw->AddLine({ pa.x, pa.y }, { pb.x, pb.y }, color, thickness);
        };
    // XY 平面就是谱面所在平面；固定有限网格不会随缩放无限增长。
    for ( int i = -10; i <= 10; ++i ) {
        const float t = i * 0.25F;
        line({ -2.5F, t, 0 }, { 2.5F, t, 0 }, IM_COL32(55, 59, 65, 255));
        line({ t, -2.5F, 0 }, { t, 2.5F, 0 }, IM_COL32(55, 59, 65, 255));
    }
    // 世界轴和操纵轴使用同一套固定颜色。
    // 当前约束轴另用高亮色表示。
    // 颜色状态不依赖皮肤音符颜色，避免混淆对象类别。
    constexpr ImU32 colors[]{ IM_COL32(245, 80, 80, 255),
                              IM_COL32(90, 220, 120, 255),
                              IM_COL32(90, 150, 255, 255) };
    for ( int axis = 0; axis < 3; ++axis ) {
        glm::vec3 end(0);
        end[axis] = 2;
        line(glm::vec3(0), end, colors[axis], 2);
        glm::vec2 label;
        if ( project(end, label) )
            draw->AddText({ label.x, label.y },
                          colors[axis],
                          axis == 0   ? "X"
                          : axis == 1 ? "Y"
                                      : "Z");
    }
    // 画布矩形具有真实宽高比，辅助网格不被当作谱面中的新增物件。
    // 矩形表示真实画布所在平面和宽高比例。
    // 它只作空间参照，实际成像由预览窗口展示。
    // 该辅助对象不会写入项目内容。
    const float halfWidth = size.x / std::max(1.0F, size.y) * 0.5F;
    const std::array<glm::vec3, 4> plane{ { { -halfWidth, -0.5F, 0 },
                                            { halfWidth, -0.5F, 0 },
                                            { halfWidth, 0.5F, 0 },
                                            { -halfWidth, 0.5F, 0 } } };
    // 白框向世界正 Y 延伸，底边和宽度不变，与真实内容坐标保持一致。
    // 原始单位平面继续用于相机视锥，画布长度不能改变相机 FOV。
    auto extendedPlane = plane;
    extendedPlane[2].y += visual.canvasCameraExtension();
    extendedPlane[3].y += visual.canvasCameraExtension();
    for ( int i = 0; i < 4; ++i )
        line(extendedPlane[i],
             extendedPlane[(i + 1) % 4],
             IM_COL32(235, 235, 235, 255),
             2);
    // 相机对象与最终渲染共用同一世界变换。
    // 局部负 Z 为观察前方。
    // 移动坐标轴以相机位置为原点，但始终沿世界轴。
    const auto      transform = canvasCameraTransform(visual);
    const glm::vec3 position(transform[3]);
    // 视锥与真实固定 FOV 相同，以近端小矩形表达朝向，不遮挡操纵轴。
    // 视锥采用固定四角，不随观察距离增加细分。
    // 横向比例继承画布比例，竖屏和宽屏都保持方向正确。
    // 小视锥用于辨认对象，不限制实际远裁剪面。
    std::array<glm::vec3, 4> corners;
    for ( int i = 0; i < 4; ++i ) {
        corners[i] = glm::vec3(
            transform * glm::vec4(plane[i] * 0.3F + glm::vec3(0, 0, -0.3F), 1));
        line(position, corners[i], IM_COL32(255, 190, 65, 255), 2);
    }
    for ( int i = 0; i < 4; ++i )
        line(corners[i], corners[(i + 1) % 4], IM_COL32(255, 190, 65, 255), 2);
    // 拾取仅考虑本帧三条轴或三条环；重叠时选择离鼠标最近的候选。
    int   hit     = -1;
    float nearest = 9;
    // 句柄尺寸随辅助观察距离增长，拉远后仍能操作。
    // 视觉半径不写入目标相机配置。
    // 最终成像不会因句柄大小改变。
    const float radius = m_distance * 0.09F;
    for ( int axis = 0; axis < 3; ++axis ) {
        const ImU32 color =
            m_axis == axis ? IM_COL32(255, 245, 135, 255) : colors[axis];
        // 旋转环采用固定分段数，限制每帧计算量。
        // 拾取使用绘制的同一折线，而不是另建近似碰撞体。
        // 平移轴仅需一条线段及末端圆点。
        const int steps = m_rotate ? 64 : 1;
        for ( int n = 0; n < steps; ++n ) {
            glm::vec3 a = position, b = position;
            if ( m_rotate ) {
                const float angle = n * 6.2831853F / 64,
                            next  = (n + 1) * 6.2831853F / 64;
                a[(axis + 1) % 3] += std::cos(angle) * radius;
                a[(axis + 2) % 3] += std::sin(angle) * radius;
                b[(axis + 1) % 3] += std::cos(next) * radius;
                b[(axis + 2) % 3] += std::sin(next) * radius;
            } else
                b[axis] += radius;
            glm::vec2 pa, pb;
            if ( project(a, pa) && project(b, pb) ) {
                draw->AddLine({ pa.x, pa.y }, { pb.x, pb.y }, color, 3);
                const float distance = cameraHandleDistance(mouse, pa, pb);
                if ( distance < nearest ) {
                    nearest = distance;
                    hit     = axis;
                }
                if ( !m_rotate )
                    draw->AddCircleFilled({ pb.x, pb.y }, 5, color);
            }
        }
    }
    draw->PopClipRect();
    // 快捷键与工具按钮一致，只更换句柄，不创建变换事务。
    // 输入框内的 G/R 属于文本，不能改变当前工具。
    // 拖动中不接受模式切换，避免同一累计偏移被重新解释。
    if ( hovered && !io.WantTextInput && m_axis < 0 ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_G, false) ) m_rotate = false;
        if ( ImGui::IsKeyPressed(ImGuiKey_R, false) ) m_rotate = true;
    }
    // 唯一的变换入口是按下已命中的轴或旋转环。
    // 空白处点击及快捷键之后的鼠标空移都不能改变姿态。
    if ( hovered && !m_orbiting && m_axis < 0 && hit >= 0 &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        m_axis = hit;
        // 事务起点必须按值保存，不能借用随后被修改的配置引用。
        // 取消时同时恢复位置和姿态。
        // 轴约束切换也以同一份起点重新计算。
        m_startPosition = visual.canvasCameraPosition;
        m_startRotation = visual.canvasCameraRotation;
        m_dragDelta     = { 0, 0 };
        m_rotationDelta = 0;
        return;
    }
    if ( m_axis < 0 ) return;
    // 数值控件、弹窗或其它窗口获得焦点时结束手势，防止跨窗口继续修改。
    if ( !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
         io.WantTextInput ) {
        m_axis = -1;
        return;
    }
    // 右键只在已开始变换时表示取消，不进入谱面擦除。
    // 取消立即恢复内存配置，下一次正常渲染读取恢复后的姿态。
    // 无需等待会话线程或提交特殊刷新命令。
    if ( ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
        visual.canvasCameraPosition = m_startPosition;
        visual.canvasCameraRotation = m_startRotation;
        m_axis                      = -1;
        return;
    }
    // 约束按世界轴解释，与彩色句柄保持一致。
    // 轴切换不改变当前移动或旋转工具。
    // 姿态计算仍从起点出发，不叠加上一个轴的历史偏移。
    if ( ImGui::IsKeyPressed(ImGuiKey_X, false) ) m_axis = 0;
    if ( ImGui::IsKeyPressed(ImGuiKey_Y, false) ) m_axis = 1;
    if ( ImGui::IsKeyPressed(ImGuiKey_Z, false) ) m_axis = 2;
    // Shift 只缩小本帧增量，实现连续精细调整。
    // 累计值与帧率无关，也不按固定时间抽样。
    // 局部反馈在输入发生的当前帧完成。
    m_dragDelta += glm::vec2(io.MouseDelta.x, io.MouseDelta.y) *
                   (io.KeyShift ? 0.1F : 1.0F);
    visual.canvasCameraPosition = m_startPosition;
    visual.canvasCameraRotation = m_startRotation;
    if ( m_rotate ) {
        // 世界轴左乘旋转再分解回同一欧拉约定，避免直接增角产生局部轴歧义。
        glm::vec3 axis(0);
        // 旋转只使用当前命中的世界轴，不支持鼠标空移的自由旋转。
        axis[m_axis] = 1;
        glm::vec2 center;
        if ( project(position, center) ) {
            // 屏幕 Y 向下，观察轴另一侧时需要反转环的旋转方向。
            const float direction =
                glm::dot(axis, eye - position) >= 0 ? -1.0F : 1.0F;
            m_rotationDelta +=
                cameraRingDelta(
                    mouse - glm::vec2(io.MouseDelta.x, io.MouseDelta.y) -
                        center,
                    mouse - center) *
                direction * (io.KeyShift ? 0.1F : 1.0F);
        }
        // 世界轴旋转左乘初始姿态，然后分解回 XYZ 显示值。
        // 这里只读取旋转分量，位置不会绕世界原点公转。
        // 下一帧的橙色视锥和数值框都读取同一结果。
        const auto rotated = glm::rotate(glm::mat4(1), m_rotationDelta, axis) *
                             canvasCameraTransform(visual);
        float      z{}, y{}, x{};
        cameraEulerAngles(rotated, z, y, x);
        visual.canvasCameraRotation = { glm::degrees(x),
                                        glm::degrees(y),
                                        glm::degrees(z) };
    } else {
        // 平移轴投影固定于按下时的位置。
        // 不能随目标移动逐帧改变换算基准。
        // 同一鼠标终点应产生相同世界偏移。
        const glm::vec3 start(
            m_startPosition[0], m_startPosition[1], m_startPosition[2]);
        glm::vec3 end = start;
        end[m_axis] += 1;
        glm::vec2 a, b;
        if ( project(start, a) && project(end, b) ) {
            const auto  delta   = b - a;
            const float length2 = glm::dot(delta, delta);
            // 与视线平行的轴不作除零放大；转动观察视图即可获得可拖动投影。
            if ( length2 > 1 )
                visual.canvasCameraPosition[m_axis] =
                    std::clamp(m_startPosition[m_axis] +
                                   glm::dot(m_dragDelta, delta) / length2,
                               -100.0F,
                               100.0F);
        }
    }
    // 所有变换均由句柄按下开始，鼠标松开确认。
    // 结束后保留最后一帧的位置与姿态。
    // 下一次变换重新冻结起点，不继承旧累计增量。
    if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) m_axis = -1;
}
}  // namespace MMM::Canvas
