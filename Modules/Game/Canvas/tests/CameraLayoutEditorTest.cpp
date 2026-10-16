#include "canvas/CameraLayoutEditor.h"
#include "canvas/CameraLayoutMath.h"
#include "config/VisualConfig.h"
#include "log/colorful-log.h"
#include <array>
#include <cmath>
#include <glm/vec4.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

namespace MMM::Canvas
{
/// @brief 测试通过模式边沿验证预览生命周期，不初始化 Vulkan。
// 这里只开放测试所需的模式选择。
// 生产位置计算、窗口绘制和生命周期判定均不替换为模拟。
// 测试不访问应用主窗口，也不需要图形设备。
struct CameraLayoutEditorTestAccess {
    /// @brief 仅查询工具选择，不模拟实际快捷键事件处理。
    static bool rotating(const CameraLayoutEditor& editor)
    {
        return editor.m_rotate;
    }
    /// @brief 模拟工具栏模式选择，避免依赖中文字体的控件宽度。
    static void mode(CameraLayoutEditor& editor, bool camera)
    {
        editor.m_cameraMode = camera;
    }
};
}  // namespace MMM::Canvas
namespace
{
/// @brief 比较投影后的屏幕位置，深度区间差异不影响二维视觉兼容。
bool sameScreen(const glm::mat4& a, const glm::mat4& b)
{
    // 中心和四角同时比较，防止只验证中心而遗漏旋转或缩放错误。
    for ( const auto point : { glm::vec4(0, 0, 0, 1),
                               glm::vec4(800, 0, 0, 1),
                               glm::vec4(400, 300, 0, 1),
                               glm::vec4(0, 600, 0, 1),
                               glm::vec4(800, 600, 0, 1) } ) {
        // 先除以齐次分量，再比较实际屏幕投影。
        // 不能直接比较矩阵元素，因为远裁剪面允许不同。
        // 容差覆盖浮点三角函数与四元数转换误差。
        const auto x = a * point, y = b * point;
        if ( glm::length(glm::vec2(x) / x.w - glm::vec2(y) / y.w) > 0.0001F )
            return false;
    }
    return true;
}
/// @brief 旧视角迁移、精确位置与持久化边界的纯数学回归。
bool checkMath()
{
    using namespace MMM::Canvas;
    // 组合旋转必须保持旧画面，而不是只在默认零角度成立。
    // 同时覆盖零俯仰、中间值和旧版最大俯仰。
    // 水平角跨正负方向，能捕获旋转乘法次序错误。
    // 所有样本都加入播放偏移，验证模型变换顺序。
    for ( float angle : { 0.0F, 35.0F, 75.0F } )
        for ( float yaw : { -120.0F, 0.0F, 60.0F } ) {
            MMM::Config::VisualConfig config;
            config.debugCanvasCameraAngleDegrees = angle;
            config.debugCanvasCameraYawDegrees   = yaw;
            config.debugCanvasCameraDistance     = 1.5F;
            // 旧函数作为迁移前的基准，不复制公式建立假基准。
            // 新结果调用生产自由相机投影函数。
            // 比较对象包括四角，因此错误缩放不会被中心点掩盖。
            const auto before =
                planarObservationProjection(800, 600, 13, angle, yaw, 1.5F);
            enableCanvasCameraPose(config);
            if ( !sameScreen(before,
                             canvasCameraPoseProjection(800, 600, 13, config)) )
                return false;
        }
    MMM::Config::VisualConfig config;
    enableCanvasCameraPose(config);
    // 默认相机为后续屏幕映射和背面拒绝提供公共基准。
    // 此处使用真实视觉配置启用函数。
    // 不人为设置测试专用投影矩阵。
    const auto centered = canvasCameraPoseProjection(800, 600, 0, config);
    // 正 X 移动相机应使画布向左，检验相机变换与模型变换方向没有混淆。
    config.canvasCameraPosition[0] = 0.25F;
    const auto shifted = canvasCameraPoseProjection(800, 600, 0, config) *
                         glm::vec4(400, 300, 0, 1);
    if ( shifted.x / shifted.w >= -0.1F ) return false;
    // 序列化走真实 ADL 接口。
    // 读取为新对象，避免原对象残留字段掩盖丢失的键。
    // 精确位置必须随启用标志一起往返保存。
    nlohmann::json json = config;
    config              = json.get<MMM::Config::VisualConfig>();
    if ( !config.canvasCameraPoseEnabled ||
         config.canvasCameraPosition[0] != 0.25F )
        return false;
    // 非数值和越界分量分别回退与限制，不能依靠异常捕获恢复配置。
    json["canvasCameraPosition"] = { "bad", 1000, nullptr };
    config                       = json.get<MMM::Config::VisualConfig>();
    if ( config.canvasCameraPosition != std::array<float, 3>{ 0, 100, 1 } )
        return false;
    // 成功投影的中心应恰好位于半宽半高。
    // 后方样本共用同一矩阵，专门验证齐次深度拒绝。
    // 这两类断言对应可见句柄和不可见句柄的输入边界。
    glm::vec2 screen;
    if ( !projectCameraHandle(
             centered, { 400, 300, 0 }, { 0, 0 }, { 800, 600 }, screen) )
        return false;
    if ( glm::length(screen - glm::vec2(400, 300)) > 0.01F ) return false;
    // 相机后方的几何不可作为句柄，避免透视除法把背后对象映到屏幕。
    if ( projectCameraHandle(
             centered, { 400, 300, 2 }, { 0, 0 }, { 800, 600 }, screen) )
        return false;
    return std::abs(cameraHandleDistance({ 5, 3 }, { 0, 0 }, { 10, 0 }) - 3) <
           0.001F;
}
/// @brief 真实 ImGui 帧检查预览只在摄像机模式存在。
bool checkWindows()
{
    // 使用真实 ImGui 状态机检查窗口是否参与本帧。
    // 不写 imgui.ini，避免污染用户布局。
    // 本测试不启动平台后端，也不创建原生窗口。
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1280, 900 };
    io.DeltaTime   = 1.0F / 60;
    unsigned char* pixels{};
    int            width{}, height{};
    // 字体图集只在初始化时准备，符合真实 NewFrame 前置条件。
    // 测试不上传纹理，预览传入空句柄。
    // 窗口存在性的断言与 GPU 是否可用无关。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    MMM::Canvas::CameraLayoutEditor editor;
    MMM::Config::VisualConfig       visual;
    bool                            passed = true;
    // 连续切换覆盖关闭后重新打开，预览不得遗留独立常驻绘制窗口。
    // 每次切换后完整执行 NewFrame、绘制和 Render。
    // 上一帧的窗口对象可能还缓存于 ImGui，但 Active 应立即清除。
    // 因此检查活动状态，而不是错误地要求内部缓存对象销毁。
    for ( bool mode : { false, true, false, true, false } ) {
        MMM::Canvas::CameraLayoutEditorTestAccess::mode(editor, mode);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 800, 700 });
        ImGui::Begin("LayoutTest");
        // 调用生产入口，不能只测试本地模式布尔值。
        // 元素模式也必须经过控件绘制，以覆盖正常返回路径。
        // 相机模式内部会创建预览，测试不手工 Begin 预览窗口。
        editor.update("test",
                      ImGui::GetCursorScreenPos(),
                      { 780, 650 },
                      ImTextureID{},
                      visual);
        ImGui::End();
        ImGui::Render();
        // 稳定内部 ID 与生产命名保持一致。
        // 窗口隐藏时允许对象仍驻留于 ImGui 缓存。
        // 只有本帧 Active 表示用户仍会看到预览。
        const auto* preview =
            ImGui::FindWindowByName("实际视角预览###CameraLayoutPreview_test");
        passed &= (preview && preview->Active) == mode;
    }
    // 退出只撤掉辅助视图，不回滚已经确认的位置。
    passed &= visual.canvasCameraPoseEnabled;
    // 键盘手势使用真实事件队列，不能仅检查状态字段变化。
    // 鼠标位于控制面板及预览之外，确保事件归属操作空间。
    MMM::Canvas::CameraLayoutEditorTestAccess::mode(editor, true);
    const auto originalPosition = visual.canvasCameraPosition;
    // 相机初值按值保存，后续断言比较真实变换前后的独立副本。
    // 不读取编辑器内部事务快照，避免两者同时错误而让测试通过。
    /// @brief 推进一帧真实操作视图，固定焦点以模拟用户点击画布。
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 800, 700 });
        ImGui::Begin("LayoutTest");
        ImGui::SetWindowFocus();
        editor.update("test",
                      ImGui::GetCursorScreenPos(),
                      { 780, 650 },
                      ImTextureID{},
                      visual);
        ImGui::End();
        ImGui::Render();
    };
    // 预热窗口悬停状态；工具快捷键不能启动变换事务。
    // 逐个推进按下、释放和移动事件，避免 ImGui 输入节流掩盖行为。
    io.AddMousePosEvent(650, 550);
    frame();
    frame();
    // 初始旋转也独立保存，不能把位置不变当作完全没有副作用。
    const auto originalRotation = visual.canvasCameraRotation;
    for ( const auto key : { ImGuiKey_R, ImGuiKey_G } ) {
        // 先旋转后移动，验证 G 确实能切回平移工具。
        // 每个快捷键都经历按下与释放，不依赖按键重复行为。
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        frame();
        // 没有按下鼠标按钮，往返运动也不应创建隐式变换事务。
        // 两个终点均留在画布内，不能依赖悬停失效来阻止错误变换。
        io.AddMousePosEvent(700, 600);
        frame();
        io.AddMousePosEvent(650, 550);
        frame();
        // 旋转和移动均只改变工具；仅检查位置会漏掉意外旋转。
        // 检查模式状态同时防止通过完全忽略快捷键来满足姿态不变断言。
        passed &= MMM::Canvas::CameraLayoutEditorTestAccess::rotating(editor) ==
                  (key == ImGuiKey_R);
        // 比较完整三分量，防止误改不可见的深度轴而漏报。
        // 这些值来自生产配置，而非编辑器内部缓存。
        passed &= visual.canvasCameraPosition == originalPosition &&
                  visual.canvasCameraRotation == originalRotation;
    }
    // 圆环正反方向及中心退化分别验证，避免旋转方向与取消混淆。
    passed &= std::abs(MMM::Canvas::cameraRingDelta({ 10, 0 }, { 0, 10 }) -
                       1.5707963F) < 0.0001F;
    passed &= std::abs(MMM::Canvas::cameraRingDelta({ 0, 10 }, { 10, 0 }) +
                       1.5707963F) < 0.0001F;
    passed &= MMM::Canvas::cameraRingDelta({ 0, 0 }, { 10, 0 }) == 0;
    // 所有帧都结束后再释放上下文。
    // 相机编辑器不拥有 GPU 资源，因此析构无需设备同步。
    // 测试产生的状态不写入用户软件设置。
    ImGui::DestroyContext();
    return passed;
}
}  // namespace
/// @brief 执行相机迁移和模式生命周期回归，不读写用户配置。
int main()
{
    if ( !checkMath() || !checkWindows() ) {
        XERROR("CameraLayoutEditorTest failed");
        return 1;
    }
    return 0;
}
