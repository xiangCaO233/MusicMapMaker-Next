#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"
#include "logic/ProjectController.h"
#include "runtime/AppThreadPool.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include "ui/imgui/menu/package/BmsExportTouch.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/workspace/MobileInputEditor.h"
#include "ui/workspace/MobileTouchScroll.h"
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <imgui_internal.h>
#include <string_view>
#include <thread>

namespace
{
int failures{};  ///< 所有场景独立累计，不用后续成功覆盖早期失败。
/// @brief 几何或生产状态不符合约定时保留具体失败标签。
void check(bool result, const char* label)
{
    if ( !result ) {
        ++failures;
        XERROR("BMS touch: {}", label);
    }
}
/// @brief CPU 场景提供外壳与日志接口，不创建 Vulkan 或设备音频。
/// @details 正式格式动作仍通过公共工厂及 execute/renderDeferred 执行。
/// 这里只发布已知工作区，不能代替生产弹窗自行计算尺寸。
/// 本夹具的边界如下：
/// - 初始化字体图集只提供可检查几何，不连接窗口后端或 SDL。
/// - INI 禁用，测试只能观察自己创建的上下文，不恢复个人停靠树。
/// - DPI 同时改变字体与主题，不能仅扩大分辨率让控件看起来能放下。
/// - 输入由 ImGui 正式激活队列推进，不手动设置导出选项的期望结果。
/// - 导出路径不在此测试创建，实际 ZIP 与音频由服务回归独立检查。
/// - 每个场景销毁上下文，前景模态和导航请求不能跨场景残留。
struct Fixture : MMM::UI::IWorkspacePresentation, MMM::UI::IStatusMessageSink {
    float m_scale;   ///< 字体、控件与工作区使用同一倍率。
    bool  m_mobile;  ///< 桌面反例仍必须保留原有控件密度。
    /// @brief 复用生产滚动仲裁，不能只检查存在子窗口就断言可触摸滚动。
    MMM::UI::MobileTouchScroll m_scroll;
    /// @brief 移动呈现由能力接口决定，不依赖宿主编译平台。
    bool allowsHoverFeedback() const override { return !m_mobile; }
    /// @brief CPU 回归不构建完整工作区，仅绑定生产呈现能力。
    void renderWorkspace(MMM::UI::MainDockSpaceUI&,
                         MMM::UI::UIManager&) override
    {
    }
    /// @brief 格式取消不应产生导出状态消息，测试不启动业务命令。
    void showStatusMessage(std::string, float) override {}
    /// @brief 初始化真实字体和窗口栈，禁止加载或写入个人 INI。
    Fixture(float scale, bool mobile) : m_scale(scale), m_mobile(mobile)
    {
        // 配置单例只用于本机 CPU 场景，控件大小与正式窗口读取同一倍率。
        // 显式设定字体尺寸，避免系统字体或上个测试的字号影响几何结论。
        // 内容区与主视口相同，失败必须来自生产窗口自身的尺寸约束。
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = scale > 1 ? ImVec2(2688, 1216) : ImVec2(1280, 720);
        io.DeltaTime   = .016f;
        // 真实移动外壳只仲裁触摸，鼠标事件不能冒充本回归的手指滑动。
        if ( mobile ) io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleMain = scale;
        // 图集仅构造一次，后续帧只用稳定的字体缓存测量与绘制。
        // 不比较跨平台字形像素，因此不同字体栅格化不会造成业务误判。
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        MMM::Config::AppConfig::instance().setNativeContentScale(scale);
    }
    /// @brief 上下文和配置倍率不跨场景泄漏；本回归没有业务保存。
    ~Fixture()
    {
        // 无业务命令和后台保存，清理上下文不需要等待逻辑线程确认。
        // 原生倍率回到一，后续单个用例不能继承这个场景的设备身份。
        ImGui::DestroyContext();
        MMM::Config::AppConfig::instance().setNativeContentScale(1);
    }
    /// @brief 每次输入推进完整 ImGui 帧，检查作用域配对和实际裁剪结果。
    void frame(const std::function<void()>& draw)
    {
        // 同一帧绑定外壳到正式绘制调用结束，子窗口能看到正确移动能力。
        // Render 后再检查错误计数，才能覆盖 EndChild 和 EndPopup 的恢复。
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("BmsHost");
        MMM::UI::WorkspaceFrameScope scope(*this);
        if ( m_mobile ) m_scroll.beginFrame(m_scale);
        draw();
        if ( m_mobile ) m_scroll.endFrame();
        ImGui::End();
        ImGui::Render();
        // 开放自动恢复不能掩盖样式栈错误；恢复计数本身也必须为零。
        check(GImGui->ErrorCountCurrentFrame == 0, "no ImGui recovery errors");
        check(GImGui->StyleVarStack.Size == 0, "all styles restored");
    }
};
/// @brief 真正点击 BMS 选项，验证反复选择、精度草稿和底部可见性。
/// @details 两种导出模式共用控件但不共用权威配置，绘制不得重设用户选择。
/// 名称和控件 ID 来自实际窗口，导航只激活控件，不直接改生产状态。
void options(float scale, bool package, bool mobile)
{
    // 初值明确采用保真与移动事件路径，绘制本身不能把它重设为另一策略。
    // package 的 false 在单文件表示保持位置，在资源包表示生成音乐副本。
    // 两种调用都必须覆盖，不能只测单选按钮改变了一个布尔值。
    Fixture      f(scale, mobile);
    bool         lossy = false, shift = true, open = true;
    ImGuiWindow* content{};
    ImVec2       footerMin{}, footerMax{};
    ImGuiWindow* modal{};
    const auto   draw = [&] {
        if ( open ) {
            MMM::UI::FeedbackOpenPopup("BmsOptions");
            open = false;
        }
        // 标题样式在 Begin 前建立，正文样式在 Begin 成功以后才压入。
        // 顺序与两个正式导出动作相同，测试不能通过无标题窗口绕过尺寸问题。
        MMM::UI::ExportTouchLayout              touch(scale);
        MMM::UI::Utils::CenteredModalPopupScope style(scale);
        if ( style.begin("BmsOptions", nullptr, 0, { 520 * scale, 0 }) ) {
            touch.beginContent();
            // 正文采用公共安全区域上限，不手写一个恰好能容纳测试内容的高度。
            // 下方确认行仍属于父窗，因此可以独立核对其真实裁剪边界。
            MMM::UI::MobileInputBodyScope body("BmsBody");
            // 保存的是本场景稳定窗口观察指针，只用于读取 ID 和几何。
            // 不把它交给业务动作，也不在上下文销毁后访问。
            content = ImGui::GetCurrentWindow();
            MMM::UI::drawBmsTouchOptions(package, lossy, shift);
            body.end();
            modal = ImGui::GetCurrentWindow();
            MMM::UI::FeedbackButton("Confirm", touch.actionSize());
            // 记录实际按钮矩形，不从 actionSize 返回值反推绘制结果。
            // 否则返回正确尺寸但父窗口裁掉底部时仍会假通过。
            footerMin = ImGui::GetItemRectMin();
            footerMax = ImGui::GetItemRectMax();
            touch.endContent();
            ImGui::EndPopup();
        }
    };
    // 自动尺寸需要经过完整帧；不使用固定时长等待或直接指定最终窗口高度。
    for ( int i = 0; i < 4; ++i ) f.frame(draw);
    check(!lossy && shift, "draw preserves initial fidelity and alignment");
    check(modal && footerMin.y >= modal->ClipRect.Min.y &&
              footerMax.y <= modal->ClipRect.Max.y,
          "footer fully visible");
    // ImGui 的最终顶点会对齐整数像素，允许不足一个物理像素的舍入误差。
    // 这个容差不按 DPI 放大，不能掩盖整行被压成桌面高度的问题。
    if ( mobile )
        check(footerMax.y - footerMin.y + 1 >= 48 * scale,
              "touch footer is 48dp");
    else
        check(footerMax.y - footerMin.y < 48 * scale,
              "desktop density unchanged");
    // 同一值连续激活不能把两个单选全都取消，且不能顺便开启有损选项。
    // 连续两次激活同一项覆盖重复轻点，不允许把单选实现成独立复选框。
    // 精度选项是独立草稿，切换对齐方式不得连带开启有损吸附。
    for ( int i = 0; i < 2; ++i ) {
        ImGui::ActivateItemByID(
            content->GetID(package ? "处理音乐副本" : "保持原时间位置"));
        f.frame(draw);
        check(!shift && !lossy, "radio remains exclusive");
    }
    // 明确启用一次后应保持原对齐策略；只有该控件可以改变有损标志。
    // 所有判断读取传入真实绘制函数的草稿，没有复制生产控件内部逻辑。
    ImGui::ActivateItemByID(content->GetID("MyO2 兼容吸附（有损）"));
    f.frame(draw);
    check(lossy && !shift, "lossy only changes on explicit activation");
    // 结束真实模态，下一场景不能继承前景窗口或未消费的导航事件。
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    f.frame(draw);
}
/// @brief 正式格式选择器在高 DPI 下必须可滚动且取消按钮仍在安全区。
/// @details 不选格式，避免无项目夹具触发磁盘扫描或真实系统保存选择器。
void formats(bool package, bool narrow = false)
{
    // 使用实机密度与横屏尺寸，格式列表总高度会超过可用正文。
    // 原生选择模式只用来进入真实格式选择页，取消前不能调出系统文件窗。
    Fixture f(2.7625f, true);
    // 字号不变而可用横宽变窄，复现分屏或远程镜像下完整英文格式名被裁切。
    // 同时缩短高度，保证本回归实际走到正文溢出与手势滚动路径。
    if ( narrow ) ImGui::GetIO().DisplaySize = { 720, 620 };
    MMM::Config::AppConfig::instance().getEditorSettings().filePickerStyle =
        MMM::Config::FilePickerStyle::Native;
    // 工厂拥有跨帧动作状态；创建新动作不能替代同一弹窗的连续帧验证。
    // 打包使用真实隔离工程的代次；这里只操作格式弹窗，不触发目录扫描。
    auto action = package ? MMM::UI::createPackBeatmapAction()
                          : MMM::UI::createSaveBeatmapAsAction();
    MMM::UI::MainMenuContext context{ f, nullptr, f.m_scale };
    f.frame([&] { action->execute(context, {}); });
    for ( int i = 0; i < 5; ++i )
        f.frame([&] { action->renderDeferred(context); });
    // 可见窗口必须来自生产 renderDeferred，不能让夹具自己的示意窗通过。
    // 已打开模态还需要确认滚动正文存在，单纯限制窗口高度会裁掉后续格式。
    auto* modal = ImGui::GetTopMostPopupModal();
    check(modal && modal->Size.y <= ImGui::GetIO().DisplaySize.y,
          "real format picker fits display");
    // 滚动正文与父窗分离是可观测结构，不以窗口存在替代取消按钮可达性。
    // 沿窗口树核对真实父子归属，不能把任意后台 Child 当作格式正文。
    // 只读取当前活动窗，上一帧已经关闭的历史窗口不计入结论。
    bool         child = false;
    ImGuiWindow* body  = nullptr;
    if ( modal )
        for ( auto* w : GImGui->Windows )
            if ( w->ParentWindow == modal && w->Active ) {
                child = true;
                body  = w;
            }
    check(child, "real format picker owns scrolling body");
    // 只在打包页检查五种格式的滚动，单文件导出页由原有格式覆盖维持。
    // 普通宽屏场景仍保留，防止新换行布局使常规 DPI 的取消按钮不可达。
    if ( narrow && package && body ) {
        // 五个完整格式名至少有一个会自动换行，溢出的正文必须保留滚动范围。
        // 起手在按钮中央，不能仅在标题和分隔空白处才能滚动。
        check(body->ScrollMax.y > 0,
              "wrapped format names create scrollable body");
        // 保存实际滚动起点，不直接写 Scroll 或 ScrollMax 来制造成功。
        // 输入经过完整 NewFrame 和生产仲裁，覆盖弹窗层级的命中门禁。
        const float  before = body->Scroll.y;
        const ImVec2 start{ body->InnerClipRect.GetCenter().x,
                            body->InnerClipRect.Min.y + 90 };
        auto&        io = ImGui::GetIO();
        io.AddMousePosEvent(start.x, start.y);
        f.frame([&] { action->renderDeferred(context); });
        io.AddMouseButtonEvent(0, true);
        f.frame([&] { action->renderDeferred(context); });
        // 位移超过当前 DPI 的触摸容差，按住状态仍由事件队列保持。
        // 第二帧用于消费 ImGui 的滚动目标，不依赖真实时间或固定 sleep。
        io.AddMousePosEvent(start.x, start.y - 80);
        f.frame([&] { action->renderDeferred(context); });
        f.frame([&] { action->renderDeferred(context); });
        // 抬手后再推进配对帧，检查没有因延迟轻点协议误激活格式按钮。
        // 模态身份必须保持，滚动不能偷偷推进到候选文件选择流程。
        io.AddMouseButtonEvent(0, false);
        f.frame([&] { action->renderDeferred(context); });
        f.frame([&] { action->renderDeferred(context); });
        check(body->Scroll.y > before, "swiping format button scrolls body");
        check(ImGui::GetTopMostPopupModal() == modal,
              "swiping does not select a format");
    }
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    f.frame([&] { action->renderDeferred(context); });
}
/// @brief 通过真实格式按钮触发候选任务，覆盖阻塞、取消及同路径换代。
/// @param root 测试专用项目目录，正常扫描应看到一份可支持的音频。
void candidateScan(const std::filesystem::path& root)
{
    // 复用工程控制器的公开接口打开、关闭和重开，保持真正的代次变化。
    // 该场景始终采用 CPU 绘制，不打开原生文件对话框或输出任何资源包。
    using MMM::Logic::ProjectController;
    auto& pools = MMM::Runtime::AppThreadPool::instance();
    pools.init();
    // 使用当前应用实例的文件池，手工替身不能证明 UI 与生产扫描线程隔离。
    auto* files = pools.getFileThreadPool();
    if ( !files ) {
        check(false, "file workers exist");
        return;
    }
    // 文件池工作者全被门闩占用，强制候选工作停在提交后、真正扫描前。
    // 后台只捕获共享状态，每个 worker 返回前不借用测试栈上的 promise。
    std::promise<void>               release;
    const auto                       gate = release.get_future().share();
    std::array<std::future<void>, 4> blockers;
    // 四份阻塞工作先入队，后续候选任务无法在界面检查前开始读取。
    for ( auto& worker : blockers )
        worker = files->enqueue([gate] { gate.wait(); });

    // 桌面按钮直接属于父窗，定位实际控件 ID 不依赖窗口像素。
    // 使用独立上下文，先前移动端场景不影响模态焦点或输入方式。
    Fixture                  f(1, false);
    auto                     action = MMM::UI::createPackBeatmapAction();
    MMM::UI::MainMenuContext context{ f, nullptr, f.m_scale };
    const auto               draw = [&] { action->renderDeferred(context); };
    // 多次使用同一个动作实例，旧结果不能靠重建动作的状态清理掩盖。
    const auto selectFormat = [&] {
        f.frame([&] { action->execute(context, {}); });
        for ( int frame = 0; frame < 4; ++frame ) f.frame(draw);
        // 先确认读到格式模态，防止余留的候选模态碰巧接受同名激活。
        auto* picker = ImGui::GetTopMostPopupModal();
        if ( !picker || !std::string_view(picker->Name)
                             .ends_with("PackageFormatPickerWindow") )
            return false;
        // ID 由生产弹窗生成，激活实际格式按钮，不能直接修改 action 状态。
        ImGui::ActivateItemByID(picker->GetID("Malody Chart Package (.mcz)"));
        // 候选在格式弹窗结束后的帧开启，测试不自行构造窗口或候选标志。
        for ( int frame = 0; frame < 3; ++frame ) f.frame(draw);
        // 确认新的模态已真实绘制，纯任务已提交不足以证明界面继续可用。
        const auto* candidates = ImGui::GetTopMostPopupModal();
        return candidates && std::string_view(candidates->Name)
                                 .ends_with("PackageFileSelectionModal");
    };

    // 工作者未释放时保持等待状态；若 UI 同步等待，此调用将不能返回。
    check(selectFormat(), "candidate modal opens while file pool is occupied");
    // 候选未到达时仍可接收 Escape，且之后能够发起新的扫描。
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    f.frame(draw);
    // 配对释放 Escape，后面的模态不继承持续按键或重复取消。
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    f.frame(draw);
    check(ImGui::GetTopMostPopupModal() == nullptr,
          "queued scan cancels before worker runs");
    // 仍保持工作队列被占用，第二个请求顺序只由取消与重新选择确定。
    check(selectFormat(), "scan restarts after cancellation");

    // 同路径重新打开也会改变项目身份，旧候选不能在新实例上完成。
    // 关闭并重新打开完全相同的路径，防止仅比对根名的实现蒙混通过。
    auto& controller = ProjectController::instance();
    check(controller.closeProject().m_closed &&
              controller.openProject(root).m_opened,
          "fixture reopens same project root");
    f.frame(draw);
    // 旧任务仍在文件池中排队时就应结束弹窗，不能等实际扫描完成。
    check(ImGui::GetTopMostPopupModal() == nullptr,
          "project generation change dismisses the stale candidate window");

    // 先解除门闩，确保测试失败也不会阻塞池的退出。
    // 保留失败状态累计，但不使用提前返回使工作者永久等待。
    release.set_value();
    for ( auto& worker : blockers ) worker.get();
    // 真实读取此时才允许开始。第三份请求必须获得当前工程的独立候选列表。
    check(selectFormat(), "scan can run in the reopened project");
    // 第一份确认只有在文件池恢复后才会从工作结果显示。
    bool sawCandidates = false;
    // 等待上限只属于测试，生产线程以帧为单位非阻塞轮询。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        f.frame([&] {
            // 使用 ImGui 的文本记录，验证正常候选确实到达业务弹窗。
            ImGui::LogToBuffer();
            draw();
            // 从已绘制文本中匹配真实文件名，无需读取私有缓存或比较平台字形。
            // 同一列表包含有效 WAV、排除 ZIP 才能验证筛选阶段确实生效。
            const std::string_view rendered(GImGui->LogBuffer.c_str());
            sawCandidates |=
                rendered.find("valid-candidate.wav") !=
                    std::string_view::npos &&
                rendered.find("ignored-archive.zip") == std::string_view::npos;
            // 每帧结束日志捕获，避免上一帧结果误入下一帧时序断言。
            ImGui::LogFinish();
        });
        if ( !sawCandidates ) std::this_thread::yield();
    } while ( !sawCandidates && std::chrono::steady_clock::now() < deadline );
    // 超时必须失败，不能将测试中预设的扩展名当作业务列表验证。
    check(sawCandidates,
          "async result applies format filtering to the fixture");
    // 测试清理只在门闩释放后执行；析构 action 不会加入任何 IO 同步等待。
    pools.shutdown();
}
}  // namespace
/// @brief 覆盖小屏、高 DPI、两种对齐含义以及桌面不放大的反例。
int main()
{
    // 小屏与高 DPI 是两条独立几何路径，桌面再提供不放大的负例。
    // 退出码累计全部失败，后面的格式页成功不能覆盖前面的选择状态失败。
    options(1, false, true);
    options(2.7625f, true, true);
    options(1, false, false);
    // 打包入口会冻结当前工程身份，先通过控制器打开隔离夹具。
    auto& controller = MMM::Logic::ProjectController::instance();
    // 临时工程只用来提供真实可打包身份，目录不需要任何谱面。
    // 两个夹具文件内容不重要，格式筛选只使用其实际名称和普通文件状态。
    const auto root =
        std::filesystem::current_path() / "test_output/BmsTouchDialogTest";
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if ( error || !(std::ofstream(root / "valid-candidate.wav") << "audio") ||
         !(std::ofstream(root / "ignored-archive.zip") << "archive") ||
         !controller.openProject(root).m_opened )
        return 1;
    formats(true);
    formats(false);
    formats(true, true);
    // 同一隔离目录用于重开测试，跟在原有触摸场景之后保持初始焦点可预测。
    candidateScan(root);
    // 通过生产关闭入口停止夹具的目录监听并撤下已发布快照。
    controller.closeProject();
    return failures ? 1 : 0;
}
