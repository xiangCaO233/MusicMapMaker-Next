/// @file NewBeatmapWizardReadModelTest.cpp
/// @brief 覆盖 CPU 布局期间的模板快照和两类视觉资源列表。
/// 测试启动真实会话及文件任务，避免独立的快照算法替身掩盖入口错接。
/// 资源夹具只要求名称与路径，媒体内容无需经过解码器。
/// 测试不初始化图形后端。CPU 侧 ImGui 布局足以区分视图是否等待会话锁。
/// 目录夹具使用实际文件池，计数断言只读取本轮向导持有的任务状态。
#include "ui/imgui/manager/NewBeatmapWizard.h"

#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectUiState.h"
#include "logic/SessionTypes.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <imgui.h>
#include <memory>
#include <mutex>
#include <string>

namespace MMM::UI
{
/// @brief 使用真实发布模型与文件线程验证向导热路径。
/// @details 友元只访问 UI 读取边界和 future。等待由测试线程执行，不往产品入口
/// 插入睡眠、完成回调或可替换的同步算法。每个场景只在断言后清理自己创建的状态。
/// 模板会话使用真实的编辑命令载入，选择仍会走产品端的活会话核验。
struct NewBeatmapWizardReadModelTestAccess {
    /// @brief 验证会话锁被另一个线程持有时模板控件仍能绘制。
    /// @param wizard 测试线程独占的向导，不需要真实窗口。
    /// @return 绘制完成、未重新发布且同一快照在源关闭后不可选择时为真。
    /// @details 由测试线程控制释放锁的时点。两秒只作为失败超时，
    /// 完成后再读绘制线程的结果，不用短暂休眠猜测线程调度。
    static bool templateDrawing(NewBeatmapWizard& wizard)
    {
        // 用不同的标签名与内部名验证投影来自实际 SessionEntry 和 BeatMap。
        // 通过注册表建立相机身份，避免使用人为填充的测试候选绕过发布。
        auto& engine                  = Logic::EditorEngine::instance();
        auto  chart                   = std::make_shared<BeatMap>();
        chart->m_baseMapMetadata.name = "template-chart";
        const auto index = engine.createSession(chart, "Template Tab");
        if ( index < 0 ) return false;

        // 初始化与正常逻辑线程顺序一致。显式发布使测试不依赖实际渲染循环。
        const auto session = engine.getSessionEntries()[index].session;
        session->update(0.0, engine.getEditorConfig(), true);
        engine.refreshAudioTimelineFingerprints();
        // 先验证正向 ID 文件列表，防止空候选让不加锁的绘制断言偶然通过。
        // 快照保持强引用到旧版本，以便在关闭源之后测试陈旧点击。
        const auto source = engine.getSessionUiSnapshot();
        if ( source->templateOptions.size() != 1 ||
             source->templateOptions.front().displayName != "Template Tab" )
            return false;

        // 测试线程在发出绘制前取得锁，并持有至读取完成或超时。
        // 若绘制退回到同步遍历，会挂在真正的注册表锁上而被发现。
        std::unique_lock hold(engine.getSessionMutex());
        auto             drawn  = std::async(std::launch::async, [&] {
            // ImGui 上下文完全从本次测试线程创建、绘制与销毁。
            ImGui::CreateContext();
            // 只初始化 CPU 字体图集和显示范围，不创建 Vulkan 或 GLFW 窗口。
            // 禁用 ini 保存，独立测试不触碰宿主的 ImGui 配置。
            auto& io              = ImGui::GetIO();
            io.IniFilename        = nullptr;
            io.DisplaySize        = ImVec2(640, 480);
            unsigned char* pixels = nullptr;
            int            width  = 0;
            int            height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            ImGui::NewFrame();
            ImGui::Begin("template-read-test");
            // 再从正常 UI 入口取得发布版本，不能把前面获取的快照直接传进绘制
            // 跳过可能退化为锁访问的桥接层。
            const auto current = engine.getSessionUiSnapshot();
            wizard.renderTemplateSourceControls(current->templateOptions,
                                                nullptr);
            ImGui::End();
            ImGui::EndFrame();
            ImGui::DestroyContext();
            return current == source;
        });
        const bool       noWait = drawn.wait_for(std::chrono::seconds(2)) ==
                            std::future_status::ready;
        // 即使误入会话锁，也要先释放再收回异步任务，以便给出测试失败结果。
        hold.unlock();
        const bool samePublication = drawn.get();
        // 关闭后仍可读取旧字符串，但重新选择必须回到实时注册表核验相机。
        // 不生成工程工作区快照，测试只涉及向导而不写入宿主当前设置。
        engine.closeSession(index, false);
        // 保留中的旧快照仍可读取，但不能再确认关闭的源。
        return noWait && samePublication &&
               engine.getSessionUiSnapshot()->templateOptions.empty() &&
               !wizard.selectTemplate(source->templateOptions.front());
    }

    /// @brief 验证一次扫描、静置复用、事件刷新和旧项目结果淘汰。
    /// @param wizard 测试创建的真实新建谱面向导。
    /// @param directory 本次 CTest 独占的构建输出目录。
    /// @return 列表可复用，工程变更后能拒绝旧结果并重新枚举时为真。
    /// @details 保持实际路径不变，仅模拟工程接管序号变化，覆盖最容易误复用
    /// 旧结果的场景。目录通知通过真实 EventBus，序号不会由测试直接写入。
    /// 两种列表共用一项候选序列，校验媒体类别可确保视频不出现在封面选择中。
    static bool resourceRead(NewBeatmapWizard&            wizard,
                             const std::filesystem::path& directory)
    {
        Logic::ProjectUiState project;
        project.m_projectRoot = directory;
        project.m_generation  = 100;
        // 使用大写图片扩展名验证大小写无关匹配，另一个扩展名必须排除。
        // 不创建实质媒体，打开下拉框不应解码文件内容。
        std::ofstream(directory / "art.PNG").put('a');
        std::ofstream(directory / "clip.mp4").put('b');
        std::ofstream(directory / "ignored.txt").put('c');

        const auto contains =
            [](const auto& list, const char* path, bool video) {
                return std::ranges::any_of(list.entries, [&](const auto& item) {
                    return item.path == path && item.video == video;
                });
            };
        // 第一次调用只排任务。等待只发生在测试夹具中，由 future 确定完成。
        if ( wizard.readVisualResources(project) ||
             !wizard.m_visualResourcesJob.valid() )
            return false;
        // future 就绪后仍要经过同一个读取入口完成消费和身份校验。
        // 测试等待期间没有持有向导或逻辑锁，失败不会要求线程碰撞。
        wizard.m_visualResourcesJob.wait();
        const auto* initial = wizard.readVisualResources(project);
        if ( !initial || initial->entries.size() != 2 ||
             !contains(*initial, "art.PNG", false) ||
             !contains(*initial, "clip.mp4", true) )
            return false;

        // 没有失效通知时静置帧应借用原列表，不能发现刚写下的第三个候选。
        std::ofstream(directory / "later.bmp").put('d');
        // 概念上是连续的下拉帧。验证同一缓存对象及无进行中任务两个条件，
        // 避免后台重复扫描只是还未完成而让相同条目数偶然成立。
        for ( int frame = 0; frame < 8; ++frame ) {
            const auto* current = wizard.readVisualResources(project);
            if ( current != initial || current->entries.size() != 2 ||
                 wizard.m_visualResourcesJob.valid() )
                return false;
        }

        // 真实事件必须使既有列表在下一次绘制立即失效。
        Event::EventBus::instance().publish(
            Event::ProjectDirectoryRefreshedEvent{});
        if ( wizard.readVisualResources(project) ||
             !wizard.m_visualResourcesJob.valid() )
            return false;
        wizard.m_visualResourcesJob.wait();
        // 旧扫描已经完成但还未交付；同路径重开单靠路径无法区分。
        ++project.m_generation;
        if ( wizard.readVisualResources(project) ||
             !wizard.m_visualResourcesJob.valid() )
            return false;
        wizard.m_visualResourcesJob.wait();
        // 最终可选项必须包含目录通知前刚写的图片，并属于最新工程代次。
        // 这也验证丢弃旧结果后可以继续提交，而不是让列表永久处于等待。
        const auto* refreshed = wizard.readVisualResources(project);
        return refreshed && refreshed->projectGeneration == 101 &&
               refreshed->entries.size() == 3 &&
               contains(*refreshed, "later.bmp", false);
    }
};
}  // namespace MMM::UI

/// @brief 在无图形后端测试环境运行向导读模型和目录刷新回归。
/// @param argc 应只包含程序名和一个输出目录参数。
/// @param argv 由 CTest 提供构建树中的测试输出根。
/// @return 两种读取模型均达到预期时返回零。
/// 线程池启动于夹具执行前，和正式程序的资源请求时序一致。
/// 每轮运行独占子目录，旧目录中未清理的夹具不会成为这次候选。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    const auto root = MMM::Config::utf8ToPath(argv[1]);
    // 明确限制到 CTest 传入的绝对路径，避免生成文件落入源码目录。
    if ( !root.is_absolute() ) return 1;
    const auto directory =
        root / std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if ( error ) return 1;

    auto& pool = MMM::Runtime::AppThreadPool::instance();
    pool.init();
    MMM::UI::NewBeatmapWizard wizard;
    using Access      = MMM::UI::NewBeatmapWizardReadModelTestAccess;
    const bool passed = Access::templateDrawing(wizard) &&
                        Access::resourceRead(wizard, directory);
    // 无论断言是否通过，先让工作线程收尾再释放向导内 future 和订阅。
    pool.shutdown();
    return passed ? 0 : 1;
}
