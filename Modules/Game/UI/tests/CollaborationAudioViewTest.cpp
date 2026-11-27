#include "ui/UIManager.h"

#include "canvas/Basic2DCanvas.h"
#include "canvas/CanvasSnapshotPrepare.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "mmm/project/AudioResource.h"
#include "mmm/project/Project.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/imgui/audio/AudioSpectrumView.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <future>

#include <memory>
#include <string>
#include <string_view>

namespace MMM::UI
{
/// @brief 直接驱动生产状态机，确定性覆盖结果晚于时间线切换的交付。
struct AudioSpectrumViewTestAccess {
    /// @brief 同长度换轨、往返换轨和退出工程时都不能显示旧像素。
    static bool checkSourceChanges()
    {
        AudioSpectrumView view("SpectrumSourceTest");
        // 已可见的旧图和未完成的旧计算同时存在，换轨时必须分别撤销。
        // 无纹理的占位绘制项足以检测状态清理，不依赖图形后端。
        view.retarget(11, true);
        view.m_spectrumReady      = true;
        view.m_texturesNeedReload = true;
        view.m_spectrumDrawCmds.emplace_back();
        std::promise<bool> oldResult;
        view.m_calcFuture = oldResult.get_future();

        // 模拟 A→B→A：最后一次仍是 A 的文件，但音频调度世代已经改变。
        view.retarget(12, true);
        view.retarget(13, true);
        // 换轨时立即校验旧渲染状态撤销，后台任务仍可留到下一轮回收。
        const bool hidden =
            !view.m_spectrumReady && !view.m_texturesNeedReload &&
            view.m_spectrumDrawCmds.empty() && view.m_recalculateRequested;

        // 旧工作者已经生成一批看似有效的像素，但该批次已撤销交付资格。
        view.m_cachedIntensityL        = { 19 };
        view.m_cachedIntensityR        = { 21 };
        view.m_cachedNumTotalSegments  = 1;
        view.m_pendingNumFrequencyBins = 1;
        // 手工交付成功结果，覆盖与取消状态竞争时的优先级。
        oldResult.set_value(true);
        view.pollCalculation();
        // 丢弃旧结果时必须回收 future，否则新任务仍会被进行中的标志挡住。
        if ( !hidden || view.m_calcFuture.valid() || view.m_texturesNeedReload )
            return false;

        // 同样的数据只有从当前任务返回时才可进入上传阶段。
        std::promise<bool> currentResult;
        // 新世代使用独立取消源，才能区分拒绝旧结果与拒绝所有结果。
        view.m_calcStopSource = std::stop_source{};
        view.m_calcFuture     = currentResult.get_future();
        // 新任务交付后先进入纹理上传队列，不能提前标记已绘制。
        currentResult.set_value(true);
        view.pollCalculation();
        if ( !view.m_texturesNeedReload || view.m_pendingChunksL.size() != 1 ||
             view.m_spectrumReady )
            return false;

        // 退出工程也要撤销已经就绪但尚未上传的分块。
        view.retarget(0, false);
        return !view.m_texturesNeedReload && view.m_pendingChunksL.empty() &&
               !view.m_spectrumReady;
    }
};
}  // namespace MMM::UI

namespace
{
/// @brief 记录真实控制器是否经平台入口提交窗口与关闭状态，不创建业务替身。
/// @details 菜单布局由 MobileDockLayoutTest
/// 验证；此处专门防止控制器绕过该入口。
struct ControllerPresentation final : MMM::UI::IWorkspacePresentation {
    /// @brief 下一帧模拟平台菜单已经返回明确关闭命令。
    bool m_closeRequested{};
    /// @brief 记录身份与关闭指针契约是否同时满足。
    bool m_routed{};
    /// @brief 原生测试窗口之外不需要宿主工作区。
    void renderWorkspace(MMM::UI::MainDockSpaceUI&,
                         MMM::UI::UIManager&) override
    {
    }
    /// @brief 与移动外壳相同，关闭桌面 X 反馈路径。
    bool allowsHoverFeedback() const override { return false; }
    /// @brief 在真实 Begin/End
    /// 作用域中消费关闭指针，返回隐藏内容以隔离音频参数。
    /// @warning 只由测试帧调用，不执行模型操作、文件访问或等待。
    bool beginWindow(std::string_view id, const std::string& title, bool* open,
                     ImGuiWindowFlags flags) override
    {
        // 只有稳定资源身份和原始打开状态都到达，关闭动作才可安全交给外壳。
        m_routed = id.starts_with("TrackController_") && title.ends_with(id) &&
                   open && *open;
        IWorkspacePresentation::beginWindow(id, title, nullptr, flags);
        if ( m_closeRequested && open ) *open = false;
        // 测试入口路由即可；折叠内容不能改变控制器所属的真实窗口生命周期。
        return false;
    }
};

/// @brief 验证主画布隐藏后音频分析仍读取最新播放、暂停与跳转状态。
/// @param manager 本进程唯一的 UI 管理器，保持 Clay 上下文生命周期连续。
/// @return 隐藏画布持续消费标题和分析快照但不恢复绘制时返回 true。
/// @details 使用真实折叠窗口进入生产可见性分支，不初始化 GPU。
bool checkHiddenCanvasPlayback(MMM::UI::UIManager& manager)
{
    auto buffer = std::make_shared<MMM::Common::Render::RenderSnapshotBuffer>();
    MMM::Canvas::Basic2DCanvas canvas(
        "AnalysisTestCanvas", 640, 480, buffer, "AnalysisTestCamera");
    // 画布未注册逻辑会话，生产端只由本用例发布，避免后台线程改变断言时刻。
    // 保留共享缓冲所有权，分析端按生产代码的非拥有读取方式检查同一槽位。
    // 折叠与后台标签走同一隐藏内容分支，不能创建 RenderContext。
    ImGui::NewFrame();
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_Always);
    canvas.update(&manager);
    ImGui::Render();
    // 无分析窗口也要消费标题元数据，但不能仅为消费快照恢复渲染。
    MMM::UI::UiFrameSnapshot frame;
    bool passed = !canvas.isDirty() && !canvas.shouldRecordOffscreen() &&
                  canvas.needsParallelUiPrepare(frame);
    // 其它分析目标不影响标签元数据消费，也不允许唤醒此画布的离屏绘制。
    frame.audioAnalysisCameraId = "OtherCamera";
    passed &=
        canvas.needsParallelUiPrepare(frame) && !canvas.shouldRecordOffscreen();
    frame.audioAnalysisCameraId = "AnalysisTestCamera";
    passed &= canvas.needsParallelUiPrepare(frame);
    // 连续发布两个播放时刻，再暂停跳转；不能由旧快照无限外推掩盖更新丢失。
    for ( int index = 0; index < 3; ++index ) {
        // 时间与几何在一次提交中发布，不能拼接不同逻辑代际的字段。
        auto* published = buffer->getWorkingSnapshot();
        published->clear();
        published->hasBeatmap      = true;
        published->currentTime     = index == 2 ? 3.0 : 10.0 + index;
        published->playbackTime    = published->currentTime;
        published->isPlaying       = index != 2;
        published->snapshotSysTime = MMM::Canvas::currentSteadySeconds();
        // 终点必须大于测试时间，否则时间 helper 会按媒体尾端钳制补间。
        // 固定一倍速以直接区分暂停原值与播放时的亚帧推进。
        published->totalTime     = 60.0;
        published->playbackSpeed = 1.0;
        // 提供动态顶点，确认隐藏消费不会顺带修改几何位置。
        published->vertices.resize(1);
        published->vertices.front().pos.y = 123.0F;
        published->dynamicVertexCount     = 1;
        const double expectedTime         = published->currentTime;
        // 记录本代时钟，不使用真实 sleep；推进量由快照解析函数计算。
        const double resolveAt = published->snapshotSysTime + 0.02;
        buffer->pushWorkingSnapshot();
        // 运行真实准备与帧边界交换，分析窗口沿用 getReadingSnapshot 读取。
        canvas.prepareUiFrameData(frame);
        canvas.swapPreparedUiFrameData();
        const auto* reading = buffer->getReadingSnapshot();
        // 检查消费槽身份而非只比较时间，防止旧读槽被直接改写成预期值。
        // 几何原值与禁用渲染同时成立，才能证明没有用后台完整渲染绕过问题。
        passed &= reading->currentTime == expectedTime &&
                  reading->isPlaying == (index != 2) &&
                  reading->vertices.front().pos.y == 123.0F &&
                  !canvas.isDirty() && !canvas.shouldRecordOffscreen();
        // 播放使用新快照继续亚帧推进；暂停后即使 UI 时钟推进也不得继续走。
        passed &=
            reading == published &&
            (index == 2
                 ? reading->resolveCurrentTimeAt(resolveAt) == expectedTime
                 : reading->resolveCurrentTimeAt(resolveAt) > expectedTime);
    }
    // 分析窗口关闭后仍轻量消费标签状态，但保持后台绘制关闭。
    frame.audioAnalysisCameraId.clear();
    passed &=
        canvas.needsParallelUiPrepare(frame) && !canvas.shouldRecordOffscreen();
    if ( !passed ) XERROR("Hidden canvas audio playback snapshot regression");
    return passed;
}

/// @brief 提交一帧真实音轨控制器，不初始化音频设备或 Vulkan。
/// @param manager 提供访客数据源和分析窗口注册表。
/// @param controller 被测生产控制器。
/// @param pointer 鼠标所在屏幕坐标。
/// @param pressed 左键当前状态。
/// @warning 测试 UI 每帧入口，只驱动真实控件和内存绘制列表。
void drawController(MMM::UI::UIManager&              manager,
                    MMM::UI::AudioTrackControllerUI& controller, ImVec2 pointer,
                    bool pressed)
{
    // 事件在 NewFrame 前排队，与应用输入转换的处理时序一致。
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, pressed);
    ImGui::NewFrame();
    // 固定位置和尺寸让测试点击覆盖全部控制行，不依赖用户工作区 ini。
    ImGui::SetNextWindowPos(ImVec2(80, 40), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(620, 650), ImGuiCond_Always);
    // 注册分析视图仍调用真实 UIManager，验证稳定注册键与控件事件一致。
    // 不运行总帧循环，避免未初始化的 GPU 视图参与渲染准备。
    controller.update(&manager);
    ImGui::Render();
}

/// @brief 通过真实鼠标点击验证只读控制器的分析入口仍可用。
/// @param label 生产按钮的本地化标签，用于计算该窗口内的控件 ID。
/// @param widthFraction 按钮所在半区的中心在实际内容宽度中的比例。
/// @param name 分析窗口稳定注册键。
/// @return 控件实际注册目标分析窗口时返回 true。
/// @details 只悬浮查找目标，避免误触折叠区或改变后续控件的布局。
/// @warning 仅测试中扫描可见帧；通过正常输入触发真实控件回调。
bool clickAnalysis(MMM::UI::UIManager&              manager,
                   MMM::UI::AudioTrackControllerUI& controller,
                   const char* label, float widthFraction,
                   const std::string& name)
{
    // 首次窗口大小由控制器本身设置，不能从夹具请求的尺寸推算命中范围。
    const auto windowName =
        "###" +
        MMM::UI::AudioTrackControllerUI::makeViewName(controller.getTrackId());
    auto* window = ImGui::FindWindowByName(windowName.c_str());
    if ( !window ) {
        XERROR("Read-only audio controller window was not submitted");
        return false;
    }
    // Clay 在控制器窗口中直接提交按钮，控件 ID 从该窗口的根栈计算。
    // 只用翻译文本识别按钮，最终的打开状态仍由稳定注册键检查。
    const auto  clip   = window->InnerClipRect;
    const auto  target = window->GetID(label);
    const float x      = clip.Min.x + clip.GetWidth() * widthFraction;
    // 使用实际内容裁剪区，容忍字体、主题和额外参数区造成的行位置变化。
    // ImGui 只给未裁剪、可交互的控件分配 HoveredId。
    // 先提交无按键的指针帧，让窗口命中在下一帧按下前完成更新。
    for ( float y = clip.Min.y + 2; y < clip.Max.y; y += 4 ) {
        const ImVec2 pointer(x, y);
        drawController(manager, controller, pointer, false);
        const auto& context = *ImGui::GetCurrentContext();
        if ( context.HoveredWindow != window || context.HoveredId != target )
            continue;
        // 只在确认命中真实按钮后发送按下与释放，保持生产点击语义。
        drawController(manager, controller, pointer, true);
        drawController(manager, controller, pointer, false);
        if ( manager.getView<MMM::UI::IUIView>(name) ) return true;
        // 已经命中却没有打开时立即判为交互失败，继续搜索会掩盖此处回归。
        XERROR("Read-only audio analysis button did not open {}", name);
        return false;
    }
    // 将不可点击与回调失败分开报告，便于定位裁剪或禁用范围的变化。
    XERROR("No clickable {} button in the read-only audio controller", name);
    return false;
}

/// @brief 验证访客资源的只读浏览、所有权和真实分析按钮。
/// @param manager 与隐藏画布测试共用的 UI 管理器。
/// @return 全部行为符合预期时返回 true。
/// @details 不打开本机项目，模拟已认证资源包发布后的访客状态。
bool checkGuestAudio(MMM::UI::UIManager& manager)
{
    auto project = std::make_shared<MMM::Project>();
    // 临时资源包与真实访客缓存的工程标记一致，不绑定磁盘工程。
    project->m_isTemporaryProject = true;
    MMM::AudioResource resource;
    resource.m_id = "guest-main";
    // 保留认证清单的相对缓存路径；测试不访问或创建这个文件。
    resource.m_path = "files/authenticated.wav";
    resource.m_type = MMM::AudioTrackType::Main;
    // 非默认音量便于识别误用缺失项目时的中性配置或改写资源配置。
    resource.m_config.volume = 0.37F;
    project->m_audioResources.push_back(resource);
    const auto* original = project.get();
    // weak_ptr 仅在测试中检查所有权，不用于生产 UI 的逐帧数据读取。
    std::weak_ptr<const MMM::Project> lifetime = project;
    // 发布与逻辑会话相同的只读数据，不要求本机项目存在。
    manager.setCollaborationAudioProject(project);
    // 去掉调用者引用后，缓存必须由 UI 发布状态独立保持存活。
    project.reset();
    // 验证可浏览资源并未成为 EditorEngine 的可写本机项目。
    bool passed = manager.getAudioProject() == original &&
                  manager.isAudioReadOnly() && !lifetime.expired() &&
                  !MMM::Logic::EditorEngine::instance().getCurrentProject();
    // 直接调用生产控制器，确保分析区未被整个窗口的只读作用域禁用。
    MMM::UI::AudioTrackControllerUI controller(
        resource.m_id,
        "Guest audio",
        MMM::UI::AudioTrackControllerUI::TrackType::Main);
    drawController(manager, controller, ImVec2(-1, -1), false);
    drawController(manager, controller, ImVec2(-1, -1), false);
    // 两个按钮位于不同半区，都要通过实际点击验证，不能只检查绘制成功。
    // 同一访客仍需能够查看两种分析结果，不能以另一个按钮的打开状态代替。
    // 先检查频谱，保证它可以从初始访客界面打开，无需先注册波形窗口。
    // 保留后续只读配置断言，验证分析入口不改变音轨编辑权限。
    passed &= clickAnalysis(manager,
                            controller,
                            TR("ui.audio_manager.open_spectrum").data(),
                            .75F,
                            "AudioSpectrum");
    passed &= clickAnalysis(manager,
                            controller,
                            TR("ui.audio_manager.open_waveform").data(),
                            .25F,
                            "AudioWaveform");
    // 点击禁用区不能改资源；退出绑定应同时恢复本机数据源并释放缓存所有权。
    passed &= original->m_audioResources.front().m_config.volume == 0.37F &&
              !original->m_audioResources.front().m_config.muted;
    // 解除绑定同步恢复数据源与只读状态，不能遗留有所有权的空壳项目。
    // 仍保留原始观察指针的身份记录；清空之后只检查弱引用，避免延长缓存生命周期。
    manager.setCollaborationAudioProject(nullptr);
    passed &= !manager.isAudioReadOnly() && !manager.getAudioProject() &&
              lifetime.expired();
    if ( !passed ) XERROR("Collaboration read-only audio view regression");
    return passed;
}
/// @brief 真实主轨和音效控制器都必须把稳定身份及关闭指针交给平台入口。
/// @details 若控制器直接调用 ImGui::Begin，本测试不会收到路由和关闭请求。
/// 绘制主体保持折叠，检查关闭契约不需要初始化音轨或修改任何播放参数。
bool checkControllerWindowRouting(MMM::UI::UIManager& manager)
{
    // 收起被测内容，只验证平台外壳能往返传递原始打开状态。
    ControllerPresentation       presentation;
    MMM::UI::WorkspaceFrameScope frameScope(presentation);
    bool                         passed = true;
    for ( const auto type :
          { MMM::UI::AudioTrackControllerUI::TrackType::Main,
            MMM::UI::AudioTrackControllerUI::TrackType::Effect } ) {
        // 位于单次循环作用域，确保切换轨型时上一控制器不再存活。
        MMM::UI::AudioTrackControllerUI controller(
            type == MMM::UI::AudioTrackControllerUI::TrackType::Main
                ? "routing-main"
                : "routing-effect",
            "Routing controller",
            type);
        // 初帧只验证稳定身份和可关闭状态，不在测试中直接改控制器字段。
        presentation.m_closeRequested = false;
        presentation.m_routed         = false;
        // 两种轨道使用不同资源身份，不能因上一个窗口已存在而假通过。
        // 关闭状态从公开接口读取，不访问控制器私有字段。
        // 每个窗口仍完成真实 Begin/End，不用回调计数代替实际路径执行。
        drawController(manager, controller, { -1, -1 }, false);
        passed &= presentation.m_routed && controller.isOpen();
        // 下一帧由平台明确消费关闭命令，主轨和音效轨都须反映到公开打开状态。
        // 路由探针在同一个控制器的下一帧执行，防止关闭状态来自新对象的默认值。
        presentation.m_closeRequested = true;
        drawController(manager, controller, { -1, -1 }, false);
        passed &= presentation.m_routed && !controller.isOpen();
    }
    if ( !passed ) XERROR("Audio controller bypassed workspace close contract");
    return passed;
}
}  // namespace

/// @brief 使用隔离的真实 ImGui 上下文运行访客音轨浏览回归。
/// @return 所有检查通过为零。
int main()
{
    // 配置根由 CTest 框架隔离，测试不读取用户保存的窗口位置。
    // 测试直接驱动 NewFrame 与 Render，不创建绑定用户桌面的系统窗口。
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    // 禁止写入 imgui.ini，固定字体和显示尺寸以稳定鼠标命中。
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(900, 740);
    // 给 ImGui 固定帧间隔，使鼠标输入与悬浮处理不依赖测试机实际频率。
    io.DeltaTime          = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int            width  = 0;
    int            height = 0;
    // 无图形后端仍需构建字体图集，避免 NewFrame 拒绝运行。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 所有分析视图在其内部作用域析构，覆盖早于 GPU 初始化的关闭路径。
    // 用同一个管理器观察全部注册结果，确保平台外壳路径与访客路径共享真实注册表。
    bool passed = false;
    {
        // 管理器只构造一次，避免已销毁视图的 Clay context
        // 成为下一次初始化目标。 全部窗口先于 ImGui
        // 上下文析构，维持生产代码的资源释放顺序。
        MMM::UI::UIManager manager;
        passed = checkGuestAudio(manager) &&
                 checkHiddenCanvasPlayback(manager) &&
                 checkControllerWindowRouting(manager) &&
                 MMM::UI::AudioSpectrumViewTestAccess::checkSourceChanges();
    }
    // 所有拥有 UI 窗口的对象已销毁，随后释放全局上下文。
    ImGui::DestroyContext();
    return passed ? 0 : 1;
}
