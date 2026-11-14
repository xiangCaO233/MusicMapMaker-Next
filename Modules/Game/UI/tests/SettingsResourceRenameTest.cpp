/// @file SettingsResourceRenameTest.cpp
/// @brief 回归共享媒体改名的提示、取消、复制及跨谱面隔离。
#include "ui/imgui/manager/SettingsView.h"

#include "audio/AudioMarkerService.h"
#include "common/LogicCommands.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "logic/SharedResourceRenameService.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/AudioResource.h"
#include "mmm/project/Project.h"
#include "mmm/sample/AudioSample.h"
#include "ui/utils/UIWidgetUtils.h"

#include <entt/entity/registry.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>

namespace
{
/// @brief 读取来源原始字节，不用重新序列化后的模型比较替代只读保证。
/// @param path 已由本用例写出的文件。
/// @return 文件的完整二进制字节。
std::string bytes(const std::filesystem::path& path)
{
    // 按二进制比较源文件，不通过重新解析后的对象相等代替原始字节保真。
    // 来源在前置步骤已经写出，后续资源副本必须与它逐字相同。
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file),
             std::istreambuf_iterator<char>() };
}

/// @brief 在构建输出项目内写出合法谱面，使用真实保存器建立 IMD 隐式关联。
/// @param path .mmm 或 .imd 输出路径，不接触用户文件。
/// @return 保存器成功写出时为真。
bool writeMap(const std::filesystem::path& path)
{
    // 同一套有效元数据交给两种格式保存器，不能通过改扩展名伪造 IMD。
    // 磁盘解析器会重新恢复歌曲与图片关联，覆盖从格式文件到模型的真实路径。
    MMM::BeatMap map;
    auto&        meta    = map.m_baseMapMetadata;
    meta.map_path        = path;
    meta.name            = "Shared resource rename";
    meta.title           = "Keep title";
    meta.track_count     = 4;
    meta.bgm_track_count = 1;
    meta.preference_bpm  = 120;
    // 歌曲提示与下面的 Main 采样使用同一资源，元数据和播放引用要一起改绑。
    // 图片同时作为封面和背景，复制后当前谱面的两个字段必须一致。
    meta.song_file_hint = "Song.wav";
    // 封面与背景共用文件，必须在当前谱面中一同投影到副本。
    // 其他谱面与 IMD 继续引用原文件，不能做项目全局重映射。
    meta.cover_path      = "Song.png";
    meta.main_cover_path = "Song.png";
    meta.cover_type      = MMM::CoverType::IMAGE;
    // 普通谱面故意使用非零锚点、负偏移与独立音量，不能在复制中改写它们。
    // IMD 的可写约束由下面的独立分支满足，不放宽生产保存器。
    map.m_audioSamples.push_back(
        MMM::AudioSampleEvent{ .m_timestamp       = 250,
                               .m_offsetMs        = -25,
                               .m_track           = 4,
                               .m_audioResourceId = "Song.wav",
                               .m_volume          = .6F });
    if ( path.extension() == ".imd" ) {
        // IMD 原格式只允许零点原速主轨，夹具遵循其真实格式约束。
        auto& sample       = map.m_audioSamples.front();
        sample.m_timestamp = 0;
        sample.m_offsetMs  = 0;
        sample.m_volume    = 1;
    }
    // 模型同步只用于生成合法格式夹具，不在这里执行待测的资源改名。
    // 保存后每次预检都会重新走实际磁盘读取与引用解析。
    map.sync();
    return map.saveToFile(path);
}
}  // namespace

namespace MMM::UI
{
/// @brief 通过友元准备设置页冻结请求，确认与事务全部走生产实现。
struct SettingsResourceRenameTestAccess {
    /// @brief 汇总失败标签，保留先前失败而不因后续成功覆盖。
    /// @param value 当前场景的完整断言。
    /// @param label 独立失败位置，日志可据此区分 UI 和业务错误。
    /// @param passed 汇总结果，已有失败不能被后续成功覆盖。
    static void check(bool value, const char* label, bool& passed)
    {
        if ( !value ) XERROR("Shared resource rename: {}", label);
        passed &= value;
    }

    /// @brief 冻结当前项目、谱面和资源，等价于设置行上的重命名按钮。
    /// @param view 独立测试视图，不打开真实桌面窗口。
    /// @param audio 为真时选择音乐，否则选择封面。
    /// @param name 用户输入的新文件名，不绕过生产名称校验。
    /// @details 只准备跨帧请求，不能在这里执行文件复制或改写谱面。
    /// 项目根、谱面和资源身份共同冻结，用于后续陈旧请求拒绝测试。
    /// 标题草稿刻意不同于模型，资源操作应保留用户尚未提交的文本。
    /// 输入缓冲使用生产固定容量，本用例名称均不超出容量。
    static void request(SettingsView& view, bool audio, const char* name)
    {
        auto& engine = Logic::EditorEngine::instance();
        // 请求只冻结项目、谱面和资源的值身份，不持有内部元数据的跨帧引用。
        // 测试只准备入口状态，路径校验和共享检测仍由生产函数执行。
        const auto& meta = engine.getActiveSession()
                               ->getContext()
                               .currentBeatmap->m_baseMapMetadata;
        view.m_renameResourceTarget =
            audio ? SettingsView::BeatmapResourceTarget::Audio
                  : SettingsView::BeatmapResourceTarget::Cover;
        view.m_resourceRenameProjectRoot =
            engine.getCurrentProject()->m_projectRoot;
        view.m_resourceRenameBeatmapPath = meta.map_path;
        view.m_resourceRenamePath =
            audio ? meta.song_file_hint : meta.cover_path;
        view.m_editingMeta       = meta;
        view.m_editingMeta.title = "Unsaved title draft";
        view.m_resourceRenameBuffer.fill('\0');
        std::memcpy(
            view.m_resourceRenameBuffer.data(), name, std::strlen(name));
        view.m_resourceRenameCopyPending = false;
        view.m_beatmapResourceImportError.clear();
    }

    /// @brief 从真实 ImGui 按钮验证第一次只提示，随后复制确认或取消。
    /// @param destination 不应在提示出现前创建的副本路径。
    /// @param view 已冻结当前项目、谱面及资源选择的测试视图。
    /// @param copy 是否继续点击“复制并重命名”，否则点击取消。
    /// @return 两次确认边界以及最终弹窗状态符合约定时为真。
    /// @details 第一次改名和第二次复制通过同一真实模态的可见按钮完成。
    /// 文件存在性、授权提示和模态栈三个维度共同判断流程结果。
    /// 取消通过真实按钮触发，不直接清空状态模拟成功。
    /// 上下文独立于用户桌面，不向系统注入鼠标或键盘操作。
    /// 字体只用于 CPU 布局，不进行 GPU 窗口截图验收。
    static bool popup(SettingsView&                view,
                      const std::filesystem::path& destination, bool copy)
    {
        view.m_openResourceRenamePopup = true;
        // 独立 CPU 上下文不会接管用户桌面或连接图形设备。
        // 布局和弹窗栈均属于本用例，测试结束时完整销毁。
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1200, 850 };
        io.DeltaTime   = .02F;
        // 注入事件必须在指定帧消费，禁止输入缓冲将点击延迟到另一个按钮。
        // 固定 DeltaTime 仅用于 UI 计算，不使用 sleep 或忙等驱动测试。
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels{};
        int            width{}, height{};
        // 默认字体图集提供按钮测量尺寸，不加载用户皮肤或修改个人字体偏好。
        // 禁用反馈音，使真实按钮回归不会向桌面播放提示声。
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        SetInteractionFeedbackEnabled(false);
        // 每帧在正常宿主中调用生产模态，没有另写简化版确认界面。
        // 鼠标移动、按下、释放依次经过 ImGui 的正常输入消费路径。
        const auto frame = [&](ImVec2 position, bool down = false) {
            io.AddMousePosEvent(position.x, position.y);
            io.AddMouseButtonEvent(0, down);
            ImGui::NewFrame();
            ImGui::Begin("SettingsRenameTestHost");
            view.renderBeatmapResourceRename(1);
            ImGui::End();
            ImGui::Render();
        };
        // 普通确认和复制确认的文案宽度不同，每次动作都重新定位。
        // 取消与确认位于同一行，不能复用第一次按钮的旧命中范围。
        const auto click = [&](bool cancel) {
            const auto  title  = TR("ui.file_manager.rename_title").toString() +
                                 "###SettingsResourceRename";
            const auto* window = ImGui::FindWindowByName(title.c_str());
            // 找不到生产模态窗口必须立即失败，不能继续点击旧坐标。
            // 固定内部 ID 排除本地化标题不同导致误认窗口的情况。
            if ( !window ) return false;
            const auto primary =
                TR(view.m_resourceRenameCopyPending
                       ? "ui.settings.beatmap.rename_copy_confirm"
                       : "ui.file_manager.context.rename");
            const float primaryWidth = ImGui::CalcTextSize(primary.data()).x +
                                       ImGui::GetStyle().FramePadding.x * 2;
            const float cancelWidth =
                ImGui::CalcTextSize(TR("ui.common.cancel").data()).x +
                ImGui::GetStyle().FramePadding.x * 2;
            // 按钮在末行，按实际文本宽度与窗口位置命中，不使用固定屏幕坐标。
            const ImVec2 position{
                window->DC.CursorStartPos.x +
                    (cancel ? primaryWidth + ImGui::GetStyle().ItemSpacing.x +
                                  cancelWidth * .5F
                            : primaryWidth * .5F),
                window->Pos.y + window->Size.y -
                    ImGui::GetStyle().WindowPadding.y -
                    ImGui::GetFrameHeight() * .5F
            };
            frame(position);
            // 先悬浮再按下和释放，覆盖按钮默认的松开激活语义。
            // 单独送一个按下事件不能证明可见按钮真的被正常点击。
            frame(position, true);
            frame(position);
            return true;
        };
        // 两个布局帧稳定自动尺寸，不能靠等待真实时间猜测模态是否出现。
        frame({ -100, -100 });
        frame({ -100, -100 });
        // 第一次点击后应保持模态打开、复制提示出现且目标文件不存在。
        // 三个条件联合检查，避免只显示了提示却已经产生文件副作用。
        bool passed = click(false) && view.m_resourceRenameCopyPending &&
                      !std::filesystem::exists(destination) &&
                      !ImGui::GetCurrentContext()->OpenPopupStack.empty();
        // 提示出现后重新布局，确保第二次命中的是新动作或明确取消。
        frame({ -100, -100 });
        frame({ -100, -100 });
        passed &=
            // 提示可能使窗口增高，第二次点击前已重新布局和重新计算坐标。
            // 确认或取消结束后都应关闭模态，不留下会重复触发的打开请求。
            click(!copy) && ImGui::GetCurrentContext()->OpenPopupStack.empty();
        passed &= std::filesystem::exists(destination) == copy;
        ImGui::DestroyContext();
        return passed;
    }

    /// @brief 在真实打开会话中测试复制音频及图片，确认其它格式不会被回写。
    /// @param root CMake 提供的构建输出子目录。
    /// @return 所有边界与文件保真断言通过时为真。
    /// @details 同一项目连续检查确认、取消、失败、复制及独占直接改名。
    /// 元数据、媒体字节、实际采样、资源配置与其它谱面文件分别验收。
    /// IMD 单独作为共享来源，随后再加入普通 MMM 验证跨格式隔离。
    /// 所有新文件只写到测试项目，源码已有资源始终只读。
    /// 文件复制与绑定更新按不同证据核对，不把内存路径当作磁盘提交成功。
    /// 未确认时只允许读取；取消、无效输入与陈旧上下文均不应产生副本。
    /// 原文件既保留字节也保留项目音轨配置，其他谱面能继续按旧资源 ID 读取。
    /// 磁盘引用与未保存模型引用分别覆盖，不把其中一个视为全部来源。
    /// 页面标题草稿、媒体路径与音轨配置分别断言，避免整份设置被误提交。
    /// 目标冲突与扩展名变化在任何共享复制副作用前被拒绝。
    /// 复制目的名称对其他 IMD 的反向影响也单独检查。
    /// 身份校验同时使用项目根与谱面路径，文件同名不意味着同一个上下文。
    static bool run(const std::filesystem::path& root)
    {
        // 成功与拒绝路径共享真实项目，失败操作不能破坏此前有效状态。
        // 原始文件字节在任何确认之前捕获，后续对其它谱面做严格只读检查。
        bool         passed  = true;
        auto&        engine  = Logic::EditorEngine::instance();
        auto*        project = engine.getCurrentProject();
        auto         session = engine.getActiveSession();
        SettingsView view("SettingsRenameTest");
        const auto   imd   = bytes(root / "Song_4k_Test.imd");
        const auto   other = bytes(root / "other.mmm");
        const auto   image = bytes(root / "Song.png");
        const auto   music = bytes(root / "Song.wav");
        // 项目音轨数量与配置单独核对，显示新路径不足以证明新 Main ID 可用。
        // 旧轨必须继续存在，其他谱面不会因为复制而丢失它的原始配置。
        // 复制前原始资源数是整个事务的基线，任何重复登记都会导致断言失败。
        // 这个基线在普通确认、取消和真实复制之间保持不变。
        const auto originalResources = project->m_audioResources.size();
        // 此时 IMD 是唯一的共享来源，普通谱面暂时不在项目表内。
        // 专门确认触发原因是 IMD 关联，而不是另一份 MMM 恰好挡住全局改名。
        // 磁盘仍有 other.mmm，但此刻不在项目清单，不应被预检作为关联来源。
        // 取消场景因此能单独证明 IMD 隐式关联已被正确识别。
        project->m_beatmaps.pop_back();
        request(view, false, "cancel.png");
        check(popup(view, root / "cancel.png", false),
              "prompt and cancel create nothing",
              passed);
        project->m_beatmaps.push_back({ "other", "other.mmm", "" });
        // 符号链接可能使源文件检查成功，但选中字段不能按真实路径投影。
        // 背景仍用真实路径，必须防止只改背景却把封面改名报告为成功。
        std::error_code aliasError;
        std::filesystem::create_symlink(
            "Song.png", root / "alias.png", aliasError);
        if ( !aliasError ) {
            auto& metadata =
                session->getContextMutable().currentBeatmap->m_baseMapMetadata;
            // 只有封面使用别名，背景保留真实文件名，避免两字段同时失败掩盖错误。
            // 其它 IMD 与 MMM 引用真实文件，所以预检仍应得到共享复制提示。
            // 这是字段投影问题，不能简单把 canonical 成功当作改绑成功。
            metadata.cover_path = "alias.png";
            request(view, false, "alias-copy.png");
            check(!view.renameBeatmapResource() &&
                      view.m_resourceRenameCopyPending,
                  "alias source first prompts",
                  passed);
            check(!view.renameBeatmapResource(true) &&
                      !std::filesystem::exists(root / "alias-copy.png") &&
                      metadata.cover_path == "alias.png" &&
                      metadata.main_cover_path == "Song.png",
                  "unmapped selected visual field rejects before copy",
                  passed);
            // 拒绝之后目标不存在，封面和背景都仍是确认前的绑定。
            // 此处仅恢复测试准备的别名，后续正常复制仍通过真实按钮执行。
            // 原文件和链接均保留供诊断，不以删除来源的方式掩盖预检结果。
            metadata.cover_path = "Song.png";
        }
        // 未提供符号链接权限的平台仍运行其余全部普通路径和复制事务回归。
        // 普通确认不能绕过提示；变更文件名后旧复制确认也不能复用。
        request(view, false, "first-choice.png");
        check(!view.renameBeatmapResource() && view.m_resourceRenameCopyPending,
              "business entry first prompts",
              passed);
        // 第一次提示的授权只能用于其展示的名称，更改输入后必须重新确认。
        // 该断言直接检查业务入口，避免约束仅存在于渲染代码。
        const char* revised = "revised-choice.png";
        view.m_resourceRenameBuffer.fill('\0');
        std::memcpy(
            view.m_resourceRenameBuffer.data(), revised, std::strlen(revised));
        check(!view.renameBeatmapResource(true) &&
                  !std::filesystem::exists(root / "first-choice.png") &&
                  !std::filesystem::exists(root / "revised-choice.png"),
              "changed filename requires fresh confirmation",
              passed);

        // 目标存在不能当作用户授权覆盖；当前谱面与原文件必须保持可用。
        // 扩展名变化或路径分隔符属于无效请求，不能因共享而进入复制兜底。
        // 错误返回前没有新文件、资源登记或元数据命令。
        request(view, false, "Song.wav");
        check(!view.renameBeatmapResource() &&
                  !view.m_beatmapResourceImportError.empty(),
              "extension change rejected",
              passed);
        request(view, false, "../escaped.png");
        check(!view.renameBeatmapResource(), "path traversal rejected", passed);
        // 图片确认走真实按钮并提交到原会话，后续 update 消费正常元数据命令。
        // 不能直接把设置行的路径变化当作当前谱面已经完成改绑。
        request(view, false, "renamed.png");
        check(popup(view, root / "renamed.png", true),
              "real image copy confirmation",
              passed);
        session->update(0, engine.getEditorConfig(), true);
        // 观察值来自真实会话模型，其字段随后会被命令原地更新。
        // 独立检查页面草稿与真实模型，排除仅 UI 文案改变的假成功。
        const auto& live =
            session->getContext().currentBeatmap->m_baseMapMetadata;
        check(live.cover_path == "renamed.png" &&
                  live.main_cover_path == "renamed.png",
              "current image fields rebind together",
              passed);
        // 文字草稿与真实模型不同，用于发现资源操作意外覆盖其他设置输入。
        // 这份草稿不应作为原资源仍绑定的证据。
        check(view.m_editingMeta.title == "Unsaved title draft",
              "text draft preserved",
              passed);
        check(bytes(root / "renamed.png") == image &&
                  bytes(root / "Song.png") == image,
              "image copy is byte exact and original retained",
              passed);
        // 原文件仍在时同名目标必须拒绝，不允许以副本覆盖共享原文件。
        // 原文件属于其它谱面，复制后的再次改名也不能把它当作覆盖目标。
        // 失败应保留此前成功建立的当前绑定，不自动回退或再复制一次。
        request(view, false, "Song.png");
        check(!view.renameBeatmapResource(true) &&
                  !view.m_beatmapResourceImportError.empty(),
              "existing destination is never overwritten",
              passed);
        // 新音乐保留原轨配置，不能套用导入默认音量或重定向其它谱面。
        // 音乐同样经过提示和二次确认，副本保留原有主轨配置。
        // 源音频使用有效 WAV 字节，不依靠虚假文件名满足播放资源解析。
        // 曾被排除的路径在用户显式创建副本后也应恢复正常登记和目录扫描。
        project->m_excludedAudioPaths.push_back("renamed.wav");
        request(view, true, "renamed.wav");
        check(popup(view, root / "renamed.wav", true),
              "real audio copy confirmation",
              passed);
        session->update(0, engine.getEditorConfig(), true);
        check(live.song_file_hint == "renamed.wav",
              "current music rebound",
              passed);
        check(project->m_audioResources.size() == originalResources + 1 &&
                  project->m_audioResources.back().m_config.volume == .75F,
              "old resource and copied audio configuration retained",
              passed);
        check(bytes(root / "renamed.wav") == music &&
                  bytes(root / "Song.wav") == music,
              "audio copy is byte exact and original retained",
              passed);
        // 附属测量文件按新音频完整文件名复制，原配套数据同样保持只读。
        // 对比字节及公开读取结果，避免只生成同名的空配套文件。
        check(
            bytes(root / "Song.wav.mmm-timing.json") ==
                    bytes(root / "renamed.wav.mmm-timing.json") &&
                std::find(project->m_excludedAudioPaths.begin(),
                          project->m_excludedAudioPaths.end(),
                          "renamed.wav") == project->m_excludedAudioPaths.end(),
            "companion copied and explicit destination unexcluded",
            passed);
        // 实际 Main BGM 的 ECS 资源 ID 与提示字段分别检查，不能只验证元数据。
        // 实体遍历只发生在测试内，不加入应用每帧或每 update 调用链。
        bool       sampleRebound = false;
        const auto samples =
            session->getContext()
                .sampleRegistry.view<const Logic::SampleComponent>();
        for ( const auto entity : samples )
            sampleRebound |= samples.get<const Logic::SampleComponent>(entity)
                                 .m_audioResourceId == "renamed.wav";
        check(sampleRebound, "Main BGM sample uses copied audio", passed);
        check(bytes(root / "other.mmm") == other &&
                  bytes(root / "Song_4k_Test.imd") == imd,
              "other MMM and IMD files remain byte identical",
              passed);
        // IMD 原始字节不变仍不足以证明可用，还需重新解析后选回原媒体。
        // 该检查排除新增副本意外改变隐式选名的反向影响。
        // 重载使用原 IMD 路径，不能通过把它转换成 MMM 来掩盖原格式引用丢失。
        // 原文件的编码与引用均不迁移到副本名称，仍保持其隐式前缀约定。
        const auto reloaded = BeatMap::loadFromFile(root / "Song_4k_Test.imd");
        check(reloaded.m_baseMapMetadata.song_file_hint == "Song.wav" &&
                  reloaded.m_baseMapMetadata.main_cover_path == "Song.png",
              "IMD implicit links still load original resources",
              passed);
        // 未保存的其他会话同样计入引用，不能只看磁盘旧元数据。
        // 该来源没有落盘也未登记，只在内存中共享文件。
        // 以观察指针提供给生产预检，验证磁盘与内存证据会合并处理。
        auto open                          = std::make_shared<BeatMap>();
        open->m_baseMapMetadata.map_path   = root / "unsaved.mmm";
        open->m_baseMapMetadata.cover_path = "renamed.png";
        const BeatMap* openMaps[]{ open.get() };
        const auto     plan =
            Logic::SharedResourceRenameService::inspect(*project,
                                                        live.map_path,
                                                        "renamed.png",
                                                        "again.png",
                                                        nullptr,
                                                        openMaps);
        check(plan.m_error.empty() && plan.m_requiresCopy,
              "unsaved other source counts as shared",
              passed);
        // 新文件名即使不覆盖旧文件，也可能被另一份 IMD 按前缀隐式选中。
        // 该谱面目前没有 New.png；创建它会改变原本无背景的关联，因此应拒绝。
        check(writeMap(root / "New_4k_Test.imd"),
              "prepare reverse implicit association",
              passed);
        project->m_beatmaps.push_back({ "new-imd", "New_4k_Test.imd", "" });
        const auto conflicting = Logic::SharedResourceRenameService::inspect(
            *project, live.map_path, "renamed.png", "New.png", nullptr, {});
        check(!conflicting.m_error.empty() &&
                  !std::filesystem::exists(root / "New.png"),
              "copy target cannot change another IMD implicit link",
              passed);
        // 恢复项目入口表再做下一独立场景，保留生成文件仅供失败诊断。
        project->m_beatmaps.pop_back();
        // 项目身份变化后旧请求不得创建任何副本，更不能串改新项目。
        // 此前已经完成成功复制，失败请求必须保留这些结果而不是清空项目。
        // 强制传 true 也不应绕过项目身份或首次提示约束。
        request(view, false, "stale.png");
        // 错误工程身份要在任何复制动作之前拒绝，即使调用者传入确认标志也无效。
        // 目标不应出现，以排除只阻止改绑却已写入旧工程的部分执行。
        view.m_resourceRenameProjectRoot = root / "other-project";
        check(!view.renameBeatmapResource(true) &&
                  !std::filesystem::exists(root / "stale.png"),
              "stale request rejected",
              passed);
        // 走谱面设置现有的同步收尾保存，再重新解析磁盘，不能只验证当前内存。
        // 保存仅发生在测试的显式生命周期边界，不用固定时间等待自动保存。
        check(session->flushPendingMetadataAutoSave(),
              "persist copied resource selection",
              passed);
        const auto saved = BeatMap::loadFromFile(root / "current.mmm");
        check(saved.m_baseMapMetadata.song_file_hint == "renamed.wav" &&
                  saved.m_baseMapMetadata.cover_path == "renamed.png" &&
                  saved.m_baseMapMetadata.main_cover_path == "renamed.png",
              "saved current chart reloads copied audio and image",
              passed);
        // 当前谱面成功落盘后，其他原生与 IMD 文件也必须继续保持原字节。
        check(bytes(root / "other.mmm") == other &&
                  bytes(root / "Song_4k_Test.imd") == imd,
              "persisting current chart leaves other sources untouched",
              passed);
        // 独占资源仍走直接改名，不要求复制，也不保留多余旧文件。
        std::error_code error;
        std::filesystem::copy_file(root / "Song.png", root / "solo.png", error);
        // 独占场景只替换当前封面，已验证的音乐和背景副本保持不变。
        // 先保存真实当前谱面，让既有直接改名服务能完成原来的磁盘引用事务。
        auto& activeMap = *session->getContextMutable().currentBeatmap;
        activeMap.m_baseMapMetadata.cover_path = "solo.png";
        check(!error && activeMap.saveToFile(root / "current.mmm"),
              "prepare exclusive resource",
              passed);
        request(view, false, "solo-new.png");
        check(view.renameBeatmapResource() &&
                  !view.m_resourceRenameCopyPending &&
                  !std::filesystem::exists(root / "solo.png") &&
                  bytes(root / "solo-new.png") == image,
              "exclusive resource retains direct rename behavior",
              passed);
        return passed;
    }
};
}  // namespace MMM::UI

/// @brief 只在构建输出生成夹具，不更改源码测试资源和用户工程。
/// @param argc 必须恰有一个绝对输出目录参数。
/// @param argv CMake 测试注册传入的参数。
/// @return 夹具创建、项目打开或任一行为断言失败时均返回非零。
/// 已有音效只读，媒体副本与格式谱面均写入构建输出目录。
/// 目录路径来自测试注册，不依赖当前工作目录寻找生成物。
/// 真实加载器读取本测试创建的 IMD，不使用任何用户工程。
/// 返回前先完成会话与项目清理，避免后台状态遗留给后续测试。
/// 新的输出目录确保不存在上次执行留下的资源 ID 或重命名目标。
/// 项目配置也生成于该目录，存储事务不触碰测试进程之外的项目清单。
/// 主进程返回值是验收唯一结果，夹具文件存在本身不代表行为通过。
/// 媒体只检查字节身份和实际绑定，本用例不声称完成 GPU 或实体设备验收。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    // 输出目录必须由 CMake 明确提供，禁止用相对路径写到不确定的位置。
    // 每次运行创建独立子目录，保留失败现场不会影响下一次执行。
    const auto base = MMM::Config::utf8ToPath(argv[1]);
    if ( !base.is_absolute() ) return 1;
    const auto root =
        base / std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if ( error ) return 1;
    // 媒体夹具使用项目自带音效字节，避免增加新的 LFS 二进制资源。
    std::filesystem::copy_file(MMM::Config::utf8ToPath(MMM_RESOURCE_DIR) /
                                   "skins/mmm-default/resources/audio/note.wav",
                               root / "Song.wav",
                               error);
    if ( error ) return 1;
    // 配套载荷使用当前版本公开结构，源音频字节不经过转码或标记重封装。
    // 实际复制仍由 AudioMarkerService 校验版本、字段及 BPM 数值。
    std::ofstream(root / "Song.wav.mmm-timing.json", std::ios::binary)
        << R"({"version":1,"chapters":[],"bpm_segments":[{"seconds":0.125,"bpm":135.25}]})";
    std::ofstream(root / "Song.png", std::ios::binary)
        << "image identity fixture";
    // 夹具保存失败必须退出，不能将无效谱面误判为没有共享引用。
    // 三份谱面都由正式保存器生成后，再进入项目与会话初始化。
    if ( !writeMap(root / "current.mmm") || !writeMap(root / "other.mmm") ||
         !writeMap(root / "Song_4k_Test.imd") )
        return 1;
    auto& engine     = MMM::Logic::EditorEngine::instance();
    auto& controller = MMM::Logic::ProjectController::instance();
    if ( !controller.openProject(root).m_opened ) return 1;
    auto* project = engine.getCurrentProject();
    // 资源 ID 与实际文件名一致，覆盖当前歌曲提示和自动采样的常见表示。
    // 音量刻意不同于导入默认值，复制丢失配置时断言才能识别。
    project->m_audioResources                         = { MMM::AudioResource{
        .m_id   = "Song.wav",
        .m_path = "Song.wav",
        .m_type = MMM::AudioTrackType::Main } };
    project->m_audioResources.front().m_config.volume = .75F;
    project->m_beatmaps = { { "current", "current.mmm", "" },
                            { "imd", "Song_4k_Test.imd", "" },
                            { "other", "other.mmm", "" } };
    // 使用正常会话初始化 ECS，不在测试中构造复制成功后的结果。
    // 只有当前谱面打开，其余来源以真实磁盘文件参与预检。
    const auto index = engine.createSession(
        std::make_shared<MMM::BeatMap>(
            MMM::BeatMap::loadFromFile(root / "current.mmm")),
        "Current resource rename");
    engine.setActiveSessionIndex(index);
    engine.getActiveSession()->update(0, engine.getEditorConfig(), true);
    const bool passed = MMM::UI::SettingsResourceRenameTestAccess::run(root);
    // 关闭独立会话时不刷新用户桌面布局，测试没有控制真实窗口。
    // 先关闭会话再结束项目，避免资源观察地址跨越所属项目生命周期。
    engine.closeSession(index, false);
    controller.closeProject();
    return passed ? 0 : 1;
}
