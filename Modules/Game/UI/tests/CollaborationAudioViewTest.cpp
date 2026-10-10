#include "ui/UIManager.h"

#include "canvas/Basic2DCanvas.h"
#include "canvas/CanvasSnapshotPrepare.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "mmm/project/AudioResource.h"
#include "mmm/project/Project.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/imgui/audio/AudioSpectrumView.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"

#include <memory>
#include <string>

namespace
{
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
/// @param x 波形或频谱按钮所在半区的横坐标。
/// @param name 分析窗口稳定注册键。
/// @return 控件实际注册目标分析窗口时返回 true。
/// @details 扫描纵向位置以容忍字体和翻译造成的行高变化。
/// 只读参数区同样受到点击，不能因此写入音轨配置或播放参数。
bool clickAnalysis(MMM::UI::UIManager&              manager,
                   MMM::UI::AudioTrackControllerUI& controller, float x,
                   const std::string& name)
{
    // 每次完整按下释放，并提前悬浮一帧供 ImGui 更新命中窗口。
    // 只在控制器内容区点击，不触发窗口关闭或移动。
    for ( float y = 95; y < 630; y += 8 ) {
        const ImVec2 pointer(x, y);
        drawController(manager, controller, pointer, false);
        drawController(manager, controller, pointer, true);
        drawController(manager, controller, pointer, false);
        // 注册表中存在真实窗口才能证明按钮可用，单纯渲染不算成功。
        // 找到目标即停止，避免继续点击已经成功的按钮造成重复打开。
        if ( manager.getView<MMM::UI::IUIView>(name) ) return true;
    }
    XERROR("Read-only audio analysis button did not open {}", name);
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
    passed &= clickAnalysis(manager, controller, 520, "AudioSpectrum");
    passed &= clickAnalysis(manager, controller, 220, "AudioWaveform");
    // 点击禁用区不能改资源；退出绑定应同时恢复本机数据源并释放缓存所有权。
    passed &= original->m_audioResources.front().m_config.volume == 0.37F &&
              !original->m_audioResources.front().m_config.muted;
    // 解除绑定同步恢复数据源与只读状态，不能遗留有所有权的空壳项目。
    manager.setCollaborationAudioProject(nullptr);
    passed &= !manager.isAudioReadOnly() && !manager.getAudioProject() &&
              lifetime.expired();
    if ( !passed ) XERROR("Collaboration read-only audio view regression");
    return passed;
}
}  // namespace

/// @brief 使用隔离的真实 ImGui 上下文运行访客音轨浏览回归。
/// @return 所有检查通过为零。
int main()
{
    // 配置根由 CTest 框架隔离，测试不读取用户保存的窗口位置。
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    // 禁止写入 imgui.ini，固定字体和显示尺寸以稳定鼠标命中。
    io.IniFilename        = nullptr;
    io.DisplaySize        = ImVec2(900, 740);
    io.DeltaTime          = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int            width  = 0;
    int            height = 0;
    // 无图形后端仍需构建字体图集，避免 NewFrame 拒绝运行。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 所有分析视图在其内部作用域析构，覆盖早于 GPU 初始化的关闭路径。
    bool passed = false;
    {
        // 管理器只构造一次，避免已销毁视图的 Clay context
        // 成为下一次初始化目标。 全部窗口先于 ImGui
        // 上下文析构，维持生产代码的资源释放顺序。
        MMM::UI::UIManager manager;
        passed = checkGuestAudio(manager) && checkHiddenCanvasPlayback(manager);
    }
    // 所有拥有 UI 窗口的对象已销毁，随后释放全局上下文。
    ImGui::DestroyContext();
    return passed ? 0 : 1;
}
