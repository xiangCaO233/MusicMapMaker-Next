// 资源导入设置在此修改持久化字段，不借用基础视图的传递包含。
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "ui/imgui/manager/SettingsView.h"

#include "audio/AudioMarkerService.h"
#include "common/AudioInfoUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSettingsSnapshot.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectCommandService.h"
#include "logic/ProjectController.h"
#include "logic/ProjectResourceService.h"
#include "logic/SharedResourceRenameService.h"
#include "logic/VisualResourceRenameService.h"
#include "logic/session/context/SessionContext.h"
// 导入流程访问活动谱面的资源字段，完整模型依赖仅放在实现端。
#include "mmm/beatmap/BeatMap.h"
#include "mmm/metadata/BaseMapMeta.h"
#include "mmm/project/AudioResource.h"
#include "mmm/project/AudioTrackType.h"
#include "mmm/project/Project.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/ProjectMediaTask.h"
#include "ui/utils/UIWidgetUtils.h"

#include <ImGuiFileDialog.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <imgui.h>
#include <memory>
#include <mutex>
#include <nfd.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

/// @file SettingsView_BeatmapResourceImport.cpp
/// @brief 设置页的音频、图片与视频选择、工程复制及资源重命名。
/// @details 本文件只负责用户确认后的低频导入，常规设置渲染不读取文件。
///
/// 入口分为两段：Clay 行回调只记录目标及点击时的工程和谱面身份；
/// SettingsView::update 在行回调退出、会话锁释放后驱动文件选择器。
/// 原生对话框同步返回，内置对话框跨帧返回，两者共用同一导入处理。
///
/// 音频先按后缀分类再执行解码探针，图片只接受渲染器已支持的格式。
/// 工程内已有文件按相对路径复用，工程外文件按不覆盖原则复制到根目录。
/// 音频需登记为 Main 项目资源，谱面元数据随后引用该已登记路径。
/// 封面只改缩略图字段，背景按请求时的图片或视频类型写入。
/// 错误信息保留在设置页，供用户修复源文件或目录权限后重新尝试。
/// 取消或失败不清空当前谱面的音频与图片引用。
///
/// 选择器打开时用户可能切换项目或谱面，导入前必须再次核对身份。
/// 身份不匹配时不复制、不提交旧草稿，取消选择也不改变已有引用。
/// 元数据更新始终经过逻辑命令，以保留会话校验与自动保存语义。

namespace MMM::UI
{
namespace Utils
{
/// @brief 文件线程交付的共享检查和复制结果。
struct ProjectMediaRenameResult {
    /// @brief 只有原会话、原载图实例可以提交复制后的引用。
    std::shared_ptr<const Logic::BeatmapSettingsSnapshot> m_sourceIdentity;
    /// @brief 原工程实例的代次，即使用户重开原目录也须拒绝旧结果。
    std::uint64_t m_generation{};
    /// @brief 同一文件名得到两次用户确认时才允许复制。
    std::string m_newName;
    /// @brief 共享检查的路径、提示来源和错误。
    Logic::SharedResourceRenamePlan m_plan;
    /// @brief 当前资源保持原样，副本继承其音轨设置。
    std::optional<AudioResource> m_audio;
    /// @brief 确认时采集的字段差量基准，不会覆盖其他并发输入。
    BaseMapMeta m_baseline;
    /// @brief 文件任务计算的副本引用，收尾仅按字段差量提交。
    BaseMapMeta m_updated;
    /// @brief 目标媒体已经完整复制，仍须原会话复核才能提交引用。
    bool m_copied{};
};

/// @brief 复制用户明确授权的共享媒体及配套标记。
/// @warning 文件池执行，绝不持有会话锁或引用可变工程。
void copySharedMedia(ProjectMediaRenameResult&    result,
                     const std::filesystem::path& projectRoot, bool cover)
{
    // - 任务结果独占本次计划，可直接写入错误和输出字段。
    // - 工程根按实际路径解析，确保输出引用基准与预检使用的路径相同。
    // - 任何路径失败都在复制前返回，外层只消费错误信息。
    auto&           plan = result.m_plan;
    std::error_code error;
    const auto      root = std::filesystem::canonical(projectRoot, error);
    if ( error ) {
        plan.m_error = error.message();
        return;
    }
    // - 计划目的地已经过工程边界校验，此处只计算保存到谱面的相对引用。
    // - 将冻结基线复制到更新值，稍后交付者可仅合并变化字段。
    // - 源标题、作者等与资源无关的元数据不会在文件任务中重写。
    const auto relative = plan.m_destination.lexically_relative(root);
    result.m_updated    = result.m_baseline;
    // - 音频副本复制轨道设置，源条目不从项目资源表移除。
    // - 独立资源 ID 从目的文件名生成，冲突要在回收时重新确认。
    // - 路径内表示使用 UTF-8；实际文件操作仍使用 filesystem::path。
    if ( result.m_audio ) {
        result.m_audio->m_id =
            Config::pathToUtf8(plan.m_destination.filename());
        result.m_audio->m_path          = Config::pathToUtf8(relative);
        result.m_updated.song_file_hint = relative;
        // - 音频提示字段作为本次操作的唯一入口，旧兼容路径必须清掉。
        // - 显式清除标记也要更新，否则加载器会忽略新提示。
        result.m_updated.main_audio_path.clear();
        result.m_updated.song_file_hint_cleared = false;
    } else {
        // -
        // 视觉资源依赖现有路径映射规则，避免单独处理封面和背景时产生不同语义。
        // - 共享资源副本只更新冻结谱面的对应字段，其他谱面仍引用源文件。
        // - 如果没有命中真实选择项，不能以其他字段的偶然变化判定成功。
        Logic::VisualResourceRenameService::remapMetadata(
            result.m_updated, root, plan.m_source, plan.m_destination);
        // 链接源的 canonical 名与原字段可能不同。不能只改另一个字段而误报成功。
        if ( cover ? result.m_updated.cover_path == result.m_baseline.cover_path
                   : result.m_updated.main_cover_path ==
                         result.m_baseline.main_cover_path ) {
            plan.m_error = "无法映射当前资源路径，请重新选择该资源后重试";
            return;
        }
    }
    // - 配套测量文件的名称由完整目的音频名派生，包含媒体扩展名。
    // - 图片和视频没有音频测量副本，因此检测只在音频路径执行。
    auto companion = plan.m_destination;
    companion += ".mmm-timing.json";
    // 正式资源或配套已存在时都拒绝覆盖。失败清理由创建该文件的 worker 执行。
    if ( result.m_audio &&
         (std::filesystem::exists(companion, error) || error) ) {
        plan.m_error = "目标音频的配套测量文件已存在或不可访问";
        return;
    }
    // - 实际复制仍禁止覆盖，不能仅靠预检时目标不存在这一瞬时判断。
    // - 预检与实际复制之间文件池可能让出线程，其他操作能够抢先占用目的路径。
    if ( !std::filesystem::copy_file(plan.m_source,
                                     plan.m_destination,
                                     std::filesystem::copy_options::none,
                                     error) ) {
        plan.m_error = "复制资源失败：" + error.message();
        return;
    }
    if ( result.m_audio ) {
        // - 配套文件在媒体复制之后处理，错误时先回收本次媒体。
        // - 保留已有目标配套文件，不能用目的路径递归清理。
        // - 测量数据和媒体都准备完成才允许登记正式音轨。
        error = Audio::AudioMarkerService::copyCompanion(plan.m_source,
                                                         plan.m_destination);
        if ( error ) {
            std::error_code cleanup;
            // 只有目的媒体是本任务创建的；配套冲突时保留原有文件。
            std::filesystem::remove(plan.m_destination, cleanup);
            plan.m_error = "复制音频测量数据失败：" + error.message();
            return;
        }
    }
    // - 此位只证明文件任务完成；回收时仍需核对会话、工程及资源输入。
    // - 不得把文件成功当作谱面和项目模型已经更新。
    result.m_copied = true;
}
}  // namespace Utils

/// @brief 在设置页锁外驱动音频、图片或视频资源选择器。
/// @param dpiScale 当前 UI 内容缩放。
/// @details 按钮在 Clay 行回调中只写入请求；本函数运行时该回调持有的会话锁
/// 已释放。原生选择器同步完成，内置选择器跨帧完成，两者都调用同一导入入口。
/// 文件类型过滤与资源分类 helper 的白名单一致，背景遵循点击时的媒体类型。
///
/// 请求位消费后，Native 分支在当前调用内完成选择、导入和目标清理。
/// IGFD 分支在打开后逐帧显示，只有确认或取消才清理目标。
/// 两种分支不能共享另一个窗口的对话框 key，避免误取其他导入结果。
/// 最近浏览目录只在确认选择后更新，取消不会修改编辑器偏好。
/// @warning 用户点击才打开文件选择器；普通帧只检查一个布尔值和对话框状态。
void SettingsView::renderBeatmapResourcePicker(float dpiScale)
{
    // - 每次设置帧先消费独立的异步导入结果，选择器已经关闭时也能完成提交。
    // - 未完成的导入不会阻塞窗口，回收器只试探就绪状态。
    pollBeatmapResourceImport();
    if ( m_beatmapResourceTarget == BeatmapResourceTarget::None ) return;

    constexpr const char* PICKER_ID = "SettingsBeatmapResourcePicker";
    auto*                 dialog    = ImGuiFileDialog::Instance();
    if ( m_openBeatmapResourcePicker ) {
        // 此处的类型来自按钮点击快照，不从可能已切换的活动谱面再次推断。
        // 选择器返回后还会核对项目/谱面身份，因此取消与切谱不会误改资源。
        // 消费一次性请求，避免原生窗口在用户取消后下帧再次弹出。
        m_openBeatmapResourcePicker = false;
        const bool audio =
            m_beatmapResourceTarget == BeatmapResourceTarget::Audio;
        const bool video = m_importBackgroundVideo;
        // 两种选择器共享相同的媒体快照，不能只改变扩展名而遗漏导入校验。
        const auto& settings =
            Config::AppConfig::instance().getEditorSettings();
        // - 过滤提示使用冻结媒体类型，翻译只影响可见标签和标题。
        // - 后缀集合始终属于实际导入入口的验证规则，不随当前翻译变化。
        const char* title = TR(audio   ? "ui.settings.beatmap.import_audio"
                               : video ? "ui.settings.beatmap.import_video"
                                       : "ui.settings.beatmap.import_image")
                                .data();
        if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
            // 原生选择器可能阻塞，因此绝不从持有 session 锁的设置行调用。
            // 过滤器仅帮助用户选择，导入函数仍会重新验证真实后缀。
            PlayPopupOpenFeedback();
            nfdu8char_t*            selected = nullptr;
            const nfdu8filteritem_t filter{
                title,
                audio   ? "mp3,ogg,wav,flac,opus,aac,m4a"
                : video ? "mp4,avi,mkv,webm,mov,flv,m4v"
                        : "png,jpg,jpeg,bmp"
            };
            // - 原生选择器返回结果码，不把无结果和读取错误合并处理。
            // - 最近目录通过编辑器选项传入，浏览不直接改变当前谱面路径。
            const auto result = NativeFileDialog::openFile(
                &selected, &filter, 1, settings.lastFilePickerPath.c_str());
            if ( result == NFD_OKAY ) {
                // 路径先按值复制，再释放 NFD 所有的 UTF-8 缓冲区。
                // 导入过程不借用 NFD 缓冲，释放后也不会悬挂引用。
                const auto source = Config::utf8ToPath(selected);
                NFD_FreePathU8(selected);
                importBeatmapResource(source);
            } else if ( result == NFD_ERROR ) {
                m_beatmapResourceImportError = NFD_GetError();
            }
            // - 原生选择已经在本次调用内结束，立即清掉对话框请求。
            // - 已排队的文件任务拥有独立源身份，仍可继续完成。
            m_beatmapResourceTarget = BeatmapResourceTarget::None;
            m_resourceImportSource.reset();
            return;
        }

        // IGFD 的固定 key 与主菜单音频选择器、创建向导选择器相互隔离。
        // 单选和只读文件名避免把未存在的手输名称当成已选资源。
        IGFD::FileDialogConfig config;
        config.path = settings.lastFilePickerPath;
        // - 只允许选择一个资源，导入任务只有一个固定目的字段。
        // - 只读文件名输入防止把不存在的手输路径误认为对话框结果。
        config.countSelectionMax = 1;
        config.flags             = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_HideColumnType |
                       ImGuiFileDialogFlags_ReadOnlyFileNameField;
        dialog->OpenDialog(PICKER_ID,
                           title,
                           audio   ? ".mp3,.ogg,.wav,.flac,.opus,.aac,.m4a"
                           : video ? ".mp4,.avi,.mkv,.webm,.mov,.flv,.m4v"
                                   : ".png,.jpg,.jpeg,.bmp",
                           config);
        PlayPopupOpenFeedback();
    }

    // - 共享对话框实例可能被其他功能使用，必须以本入口的固定键查询。
    // - 尚未打开或已经关闭时，不读取其他选择器保留的跨帧数据。
    if ( !dialog->IsOpened(PICKER_ID) ) return;
    // 弹窗显示和结果消费只在内置模式运行；取消不导入也不写错误。
    // Close 必须晚于 IsOk 和 GetFilePathName，下一帧不沿用旧结果。
    Utils::CenteredModalPopupScope scope(dpiScale);
    Utils::prepareCenteredModalWindow({ 600, 400 });
    if ( dialog->Display(
             PICKER_ID, ImGuiWindowFlags_NoCollapse, { 600, 400 }) ) {
        if ( dialog->IsOk() ) {
            const auto source = Config::utf8ToPath(dialog->GetFilePathName());
            // - 先按冻结输入提交文件任务，然后才处理选择器自己的最近路径。
            // - 即使任务较晚执行，也不会借用跨帧会被修改的对话框存储。
            importBeatmapResource(source);
            // 最近浏览目录属于编辑器设置，不混入谱面元数据。
            auto& engine = Logic::EditorEngine::instance();
            auto  config = engine.getEditorConfig();
            config.settings.lastFilePickerPath = dialog->GetCurrentPath();
            engine.setEditorConfig(config);
        }
        dialog->Close();
        m_beatmapResourceTarget = BeatmapResourceTarget::None;
        m_resourceImportSource.reset();
    }
}

/// @brief 把选中的媒体复制进原项目并写入原谱面的资源引用。
/// @param source 文件选择器返回的绝对或可解析路径。
/// @details 先验证类型；音频还需解码探针。项目/谱面身份与点击时快照不一致时
/// 拒绝绑定，避免用户在文件选择器打开期间切换谱面造成串写。复制复用创建向导的
/// 安全导入 helper；主音频仍经过 EditorEngine 登记并保存项目资源清单。
/// 封面和背景只改各自元数据字段，背景类型与请求一致；已有封面不覆盖。
///
/// 类型判定及音频探针先于会话锁执行，不合格文件不占用工程目录。
/// 工程根与载图实例共同标识选择事务，复制前在锁内同时核对。
/// 文件成功复制后才提交资源差量，设置页等待发布的新快照回显结果。
/// 用户尚未结束输入的字段继续留在草稿，资源选择不隐式确认其它控件。
/// 选择器关闭即销毁请求；素材不匹配或项目已切换时不产生编辑命令。
/// 载图身份在复制前及会话消费时各核对一次，因为两次处理之间可能再次重载。
/// 主音频登记后反查资源表，防止绑定到没有 Main 条目的普通文件。
/// 已登记的同路径文件即使导入命令去重，也仍允许绑定为谱面主音频。
/// 最终使用 CmdUpdateBeatmapMetadata 保持采样重定向和保存逻辑一致。
/// @warning 仅在用户选择后入队；文件池不访问打开会话。
void SettingsView::importBeatmapResource(const std::filesystem::path& source)
{
    const auto type  = Utils::classifyProjectResource(source);
    const bool audio = m_beatmapResourceTarget == BeatmapResourceTarget::Audio;
    const bool video = m_importBackgroundVideo;
    // 视频与图片都写背景路径，但解码类型不同，不能仅凭目标是 Background 接受。
    // 分类只检查后缀；复制 helper 继续验证真实普通文件与工程路径。
    if ( type != (audio   ? Utils::ProjectResourceType::Audio
                  : video ? Utils::ProjectResourceType::Video
                          : Utils::ProjectResourceType::Image) ) {
        // 对话框过滤不是数据校验；手动路径仍可能给出错误类型。
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_wrong_type").toString();
        return;
    }
    // - 无锁工程投影只用于读取根和代次；可变会话随后使用明确的短锁检查。
    // - 临时协作工程不可作为实际导入的用户项目。
    const auto project = Logic::ProjectController::instance().uiSnapshot();
    auto&      engine  = Logic::EditorEngine::instance();
    std::unique_lock sessionLock(engine.getSessionMutex(), std::try_to_lock);
    // - 源身份属于发起选择时的上下文，不能在确认后改用现在的活动页。
    // - 同一设置视图只跟踪一个导入任务，不用新请求覆盖仍可交付的旧 future。
    const auto& request = m_resourceImportSource;
    // 文件池尚未接到任务前拒绝陈旧选择。会话锁忙时保留旧工程不变。
    if ( !sessionLock || m_pendingImport.valid() || !request || !project ||
         project->m_isTemporaryProject ||
         request->projectRoot != project->m_projectRoot ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return;
    }
    const auto session = engine.getActiveSession();
    if ( !session || request->session.lock() != session ||
         !session->getContext().currentBeatmap ||
         request->beatmap.lock() != session->getContext().currentBeatmap ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return;
    }
    // - 两处身份检查完成后即释放锁；文件池只接收路径、类型与工程代次。
    // - 磁盘复制和解码探针可能阻塞，不能延长锁的持有范围。
    // - 后续提交还要按代次复核，解锁后的切换不影响冻结输入的安全性。
    sessionLock.unlock();
    // - 生命周期标记由设置视图持有；失效时文件任务不能把结果交给已关闭入口。
    // - 正式提交之前的任务资源仍由工作者各自管理。
    m_pendingImportLifetime = std::make_shared<const bool>(true);
    m_pendingImport = Utils::queueProjectMediaImport(project->m_projectRoot,
                                                     source,
                                                     audio,
                                                     project->m_generation,
                                                     m_pendingImportLifetime);
    // - 排队失败不会产生可轮询的结果，立刻反馈给用户。
    // - 已有资源引用都仍然留在原位置，下一次可重新选择。
    if ( !m_pendingImport.valid() ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_failed").toString();
        return;
    }
    // - 冻结结果目标、媒体类型及代次，不能在下一帧从当前选择器状态读取。
    // - 关闭选择器只结束询问阶段，后续回收依靠这些任务字段。
    m_pendingImportSource     = request;
    m_pendingImportGeneration = project->m_generation;
    m_pendingImportTarget     = m_beatmapResourceTarget;
    m_pendingImportVideo      = video;
    m_beatmapResourceImportError.clear();
}

/// @brief 在短锁内复核源工程和谱面，只接受完整复制后的结果。
/// @warning 每帧仅轮询，不等待文件任务或会话锁。
void SettingsView::pollBeatmapResourceImport()
{
    // - 先确认 future 有效，再以零等待窗口检查就绪，避免普通帧阻塞。
    // - 后台正在复制时已发布的 UI 仍可独立推进。
    if ( !m_pendingImport.valid() ||
         m_pendingImport.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready )
        return;
    auto&            engine = Logic::EditorEngine::instance();
    std::unique_lock sessionLock(engine.getSessionMutex(), std::try_to_lock);
    if ( !sessionLock ) return;

    // - 就绪且成功获得锁才消费 future；锁忙则在下帧保留并重试。
    // - get 之后 this 不再持有该轮任务的 pending 状态，成功失败都只消费一次。
    const auto operation = m_pendingImport.get();
    // - 释放仅用于取消判断的任务令牌，状态保留在交付结果中。
    // - 完成时间先于本轮提交；无需把令牌带进后续逻辑命令。
    m_pendingImportLifetime.reset();
    const auto request = std::move(m_pendingImportSource);
    // - 本轮投影的工程代次与排队时一起比较，防止同目录重开后读取旧任务。
    // - 与真实活动会话配合判断，项目路径相同不足以建立原对象的身份。
    const auto identity = Logic::ProjectController::instance().uiSnapshot();
    auto*      project  = engine.getCurrentProject();
    const auto session  = engine.getActiveSession();
    if ( !identity || identity->m_generation != m_pendingImportGeneration ||
         identity->m_isTemporaryProject || !project || !session ||
         request->session.lock() != session ||
         !session->getContext().currentBeatmap ||
         request->beatmap.lock() != session->getContext().currentBeatmap ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return;
    }
    // - 先区分上下文过期，再处理文件操作的实际失败。
    // - 失败不解析不完整的路径，也不修改项目登记和谱面引用。
    if ( operation->m_error ) {
        m_beatmapResourceImportError = operation->m_error.message();
        return;
    }
    // - 文件路径由 operation 所有，本次提交作用域内借用即可。
    // - 提交命令对路径和更新值保留自己的副本，不跨帧保存观察指针。
    const auto imported = &operation->m_path;
    const bool audio    = m_pendingImportTarget == BeatmapResourceTarget::Audio;
    const bool video    = m_pendingImportVideo;
    // 以此刻的会话值作为差量基线，文件操作不提交本页未发布的文本草稿。
    auto baseline    = session->getContext().currentBeatmap->m_baseMapMetadata;
    auto updatedMeta = baseline;
    if ( audio ) {
        // 导入既有工程文件时命令可能因去重返回“未新增”；仍需确认资源表
        // 中确实有对应 Main 条目，不能只凭文件复制成功就绑定谱面。
        const auto audioId =
            Logic::ProjectCommandService::registerPreparedMainAudio(*project,
                                                                    *imported);
        if ( audioId.empty() ) {
            m_beatmapResourceImportError =
                TR("ui.settings.beatmap.import_failed").toString();
            return;
        }
        // 媒体及配套文件已准备完成；这里仅保存更新后的工程清单。
        engine.saveProject();
        // 外部格式提示字段优先于旧主音频路径，保持与下拉选择一致。
        updatedMeta.song_file_hint = *imported;
        updatedMeta.main_audio_path.clear();
        updatedMeta.song_file_hint_cleared = false;
        // - 封面入口只替换缩略图字段，不更改背景模式或其独立的路径。
        // - 背景入口按请求时的媒体类别设置 renderer 需要的类型。
    } else if ( m_pendingImportTarget == BeatmapResourceTarget::Cover ) {
        updatedMeta.cover_path = *imported;
    } else {
        // 类型取请求快照；视频不能作为缩略图的隐式封面。
        updatedMeta.main_cover_path = *imported;
        updatedMeta.cover_type = video ? CoverType::VIDEO : CoverType::IMAGE;
        if ( !video && updatedMeta.cover_path.empty() )
            updatedMeta.cover_path = *imported;
    }

    // 只发布本次资源操作修改的字段。消费队列时再次检查载图实例，
    // 并把这些字段合并到最新元数据，保留队列中先到的其他修改。
    Logic::submitBeatmapSettingsCommand(
        *request,
        Logic::CmdUpdateBeatmapMetadata{
            .baseMeta = std::move(updatedMeta),
            .settingsBaseline =
                std::make_shared<const BaseMapMeta>(std::move(baseline)) });
    m_beatmapResourceImportError.clear();
}

/// @brief 驱动资源重命名模态弹窗，并在确认时执行一次文件事务。
/// @param dpiScale 当前设置窗口缩放。
/// @warning 每帧仅检查请求和绘制弹窗；磁盘访问只在确认分支发生。
void SettingsView::renderBeatmapResourceRename(float dpiScale)
{
    // 固定内部 ID 使语言切换不影响弹窗打开状态。
    const auto title = TR("ui.file_manager.rename_title").toString() +
                       "###SettingsResourceRename";
    if ( m_openResourceRenamePopup ) {
        m_pendingRename = {};
        // 新请求不能继承上一轮复制授权，取消后重新打开也必须重新提示。
        m_resourceRenameCopyPending = false;
        // - 清除上一轮名称，同时要求新请求重新通过共享预检。
        // - 关闭后重开同一个资源也不能借用旧的写盘授权。
        m_resourceRenameCopyName.clear();
        FeedbackOpenPopup(title.c_str());
        m_openResourceRenamePopup = false;
    }
    bool open = true;
    // 模态父上下文来自设置窗口；不能放回 Clay 回调里打开阻塞式交互。
    // 窗口宽度只应用于弹窗，不扩大底层设置行或路径组合框。
    Utils::CenteredModalPopupScope scope(dpiScale);
    if ( !scope.begin(title.c_str(),
                      m_pendingRename.valid() ? nullptr : &open,
                      ImGuiWindowFlags_NoCollapse,
                      { 420.0F * dpiScale, 0.0F }) )
        return;
    const bool renamed = pollBeatmapResourceRename();
    // - 回收已完成的事务后关闭窗口；失败时保留当前缓冲和错误。
    // - 没有关闭命令时仍可重新检查文件冲突或资源是否被其他动作更换。
    if ( renamed ) {
        m_renameResourceTarget = BeatmapResourceTarget::None;
        ImGui::CloseCurrentPopup();
    }
    // - 执行期间不再次发同名任务，避免 UI 从同一个计划创建两份副本。
    // - 弹窗关闭后的本帧也禁用控件，防止之前缓存的键盘事件重新触发。
    ImGui::BeginDisabled(m_pendingRename.valid() || renamed);
    ImGui::TextWrapped("%s", TR("ui.settings.beatmap.rename_hint").data());
    // 改名不会转码，提示保留扩展名；最终仍由逻辑入口强制验证。
    // 提示只是解释文件事务，不能作为略过路径校验的依据。
    if ( m_focusResourceRenameInput ) {
        // 只在首帧选择旧名称，后续输入不反复抢焦点。
        ImGui::SetKeyboardFocusHere();
        m_focusResourceRenameInput = false;
    }
    ImGui::SetNextItemWidth(-1.0F);
    // 输入采用固定容量而非临时字符串，ImGui 跨帧编辑始终有稳定存储。
    // Enter 只记录确认意图，不先关闭窗口，以便错误时保留输入。
    const bool enter = ImGui::InputText("##ResourceFileName",
                                        m_resourceRenameBuffer.data(),
                                        m_resourceRenameBuffer.size(),
                                        ImGuiInputTextFlags_EnterReturnsTrue |
                                            ImGuiInputTextFlags_AutoSelectAll);
    // 字符串始终作为数据传给格式化 API，文件名中的百分号不参与格式解析。
    if ( !m_beatmapResourceImportError.empty() ) {
        // 失败保留弹窗及输入，用户可直接修正目标名称后重试。
        ImGui::TextWrapped("%s", m_beatmapResourceImportError.c_str());
    }
    // 修改目标名称撤销原确认，不能看过一个文件名的提示却复制成另一个名字。
    if ( m_resourceRenameCopyPending &&
         std::string_view(m_resourceRenameBuffer.data()) !=
             m_resourceRenameCopyName )
        m_resourceRenameCopyPending = false;
    // 提示只对应当前输入名，第一次普通“重命名”不能直接视为复制授权。
    // 此处不启动新的嵌套模态，避免键盘确认同时穿透两层弹窗。
    if ( m_resourceRenameCopyPending ) {
        // 只展示上次显式预检的结果，不在常态帧重复加载谱面或查询文件系统。
        const auto hint = TR_FMT("ui.settings.beatmap.rename_copy_hint",
                                 m_resourceRenameReferenceCount);
        ImGui::TextWrapped("%s", hint.c_str());
    }
    // 复制的按钮文案显式说明实际操作，Enter 与按钮采用同一个二次确认状态。
    // 点取消或标题栏关闭都不会调用文件复制入口。
    const bool confirm =
        FeedbackButton(TR(m_resourceRenameCopyPending
                              ? "ui.settings.beatmap.rename_copy_confirm"
                              : "ui.file_manager.context.rename")
                           .data());
    // 键盘确认和鼠标确认走同一个事务，不能重复改名或跳过错误处理。
    // 成功才关闭，名称冲突、权限失败等都允许原地重试。
    if ( (confirm || enter) &&
         renameBeatmapResource(m_resourceRenameCopyPending) ) {
        m_renameResourceTarget = BeatmapResourceTarget::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if ( FeedbackButton(TR("ui.common.cancel").data()) ) {
        // 取消不修改物理文件，也不提交任何谱面元数据。
        m_renameResourceTarget = BeatmapResourceTarget::None;
        m_beatmapResourceImportError.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
    // 标题栏关闭与取消都结束目标身份；下次点击完整重新准备缓冲。
    if ( !open ) m_renameResourceTarget = BeatmapResourceTarget::None;
}

/// @brief 对仍然绑定的原资源执行重命名并刷新设置草稿中的引用。
/// @return 文件与引用同步成功返回 true，失败保留具体原因。
/// @warning 显式确认路径：持有会话注册表锁，只允许低频调用。
bool SettingsView::renameBeatmapResource(bool confirmCopy)
{
    // - 确认分阶段提交，只有上一阶段结果回收后才能授权下一步。
    // - 切换资源期间尝试获取短锁；竞争不通过时保留窗口和编辑内容。
    if ( m_pendingRename.valid() ) return false;
    auto&            engine = Logic::EditorEngine::instance();
    std::unique_lock lock(engine.getSessionMutex(), std::try_to_lock);
    if ( !lock ) return false;
    const auto  identity = Logic::ProjectController::instance().uiSnapshot();
    const auto& request  = m_resourceRenameSource;
    auto*       project  = engine.getCurrentProject();
    const auto  session  = engine.getActiveSession();
    if ( !identity || !request || !project || !session ||
         identity->m_isTemporaryProject || request->session.lock() != session ||
         !session->getContext().currentBeatmap ||
         request->beatmap.lock() != session->getContext().currentBeatmap ||
         project->m_projectRoot != m_resourceRenameProjectRoot ||
         session->getContext().currentBeatmap->m_baseMapMetadata.map_path !=
             m_resourceRenameBeatmapPath ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return false;
    }
    // - 结果对象携带一份冻结源身份及命令元数据，允许文件线程独立处理。
    // - 先建立基准，再用该基准确定活动引用，避免计划围绕另一个实例生成。
    auto result = std::make_shared<Utils::ProjectMediaRenameResult>();
    result->m_sourceIdentity = request;
    result->m_generation     = identity->m_generation;
    result->m_newName        = m_resourceRenameBuffer.data();
    result->m_baseline =
        session->getContext().currentBeatmap->m_baseMapMetadata;
    const bool  audio = m_renameResourceTarget == BeatmapResourceTarget::Audio;
    const bool  cover = m_renameResourceTarget == BeatmapResourceTarget::Cover;
    const auto& live  = result->m_baseline;
    // - 主音频兼容两种旧元数据引用，取当前实际有效优先级。
    // - 和弹窗打开时采样的原路径比较，避免把独立导入的媒体当成旧名。
    const auto& current =
        audio ? (live.song_file_hint.empty() ? live.main_audio_path
                                             : live.song_file_hint)
              : (cover ? live.cover_path : live.main_cover_path);
    if ( m_renameResourceTarget == BeatmapResourceTarget::None ||
         current != m_resourceRenamePath ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return false;
    }
    // 只提取预检需要的项目表和会话引用；时间线、撤销栈与音频对象留在逻辑侧。
    Project projectCopy;
    projectCopy.m_projectRoot    = project->m_projectRoot;
    projectCopy.m_audioResources = project->m_audioResources;
    projectCopy.m_beatmaps       = project->m_beatmaps;
    // - 打开页可能有未保存的新链接，不能只检查磁盘工程列表。
    // - 使用轻量的路径引用投影，后台不遍历活注册表。
    std::vector<Logic::OpenResourceReferenceSnapshot> openMaps;
    for ( const auto& entry : engine.getSessionEntries() )
        if ( !entry.isLogoPlaceholder && entry.session &&
             entry.session->getContext().currentBeatmap )
            openMaps.push_back(Logic::SharedResourceRenameService::capture(
                *entry.session->getContext().currentBeatmap, audio));
    // - 第一次预检没有文件复制授权，界面只拿共享计数。
    // - 只有第二次确认仍使用同名输入时才允许工作者创建副本。
    const bool copyAuthorized = confirmCopy && m_resourceRenameCopyPending &&
                                m_resourceRenameCopyName == result->m_newName;
    lock.unlock();

    // - 文件池缺失时返回明确错误，不在界面线程回退执行磁盘预检。
    // - 后台捕获项目表和会话投影的所有权，执行时不会引用会话锁。
    auto* worker = Runtime::AppThreadPool::instance().getFileThreadPool();
    if ( worker )
        m_pendingRename = worker->enqueue([result,
                                           project  = std::move(projectCopy),
                                           openMaps = std::move(openMaps),
                                           current,
                                           audio,
                                           cover,
                                           copyAuthorized] {
            // - 视觉资源入口已经持有路径，音频字段还可能是工程资源 ID。
            // - 先用冻结项目表解析主音轨身份，实际查找不读取活动项目模型。
            auto source = current;
            if ( audio ) {
                const auto* resource = Logic::ProjectResourceService::
                    findAudioResourceForReference(project,
                                                  result->m_baseline.map_path,
                                                  Config::pathToUtf8(current));
                // - 无法确认为主音轨时不继续尝试同名文件。
                // - 音效同名或旧路径悬空都不能凭扩展名猜测资源。
                if ( !resource || resource->m_type != AudioTrackType::Main ) {
                    result->m_plan.m_error = "无法识别当前主音频";
                    return result;
                }
                result->m_audio = *resource;
                source          = Config::utf8ToPath(resource->m_path);
            }
            result->m_plan =
                Logic::SharedResourceRenameService::inspectCaptured(
                    project,
                    result->m_baseline.map_path,
                    source,
                    result->m_newName,
                    result->m_audio ? &*result->m_audio : nullptr,
                    openMaps);
            // - 后台真正复制前再查当前工程代次，降低退出或切工程后写盘的机会。
            // - 结果即使已复制也要由 UI 回收边界重验会话。
            const auto active =
                Logic::ProjectController::instance().uiSnapshot();
            if ( result->m_plan.m_error.empty() &&
                 result->m_plan.m_requiresCopy && copyAuthorized && active &&
                 active->m_generation == result->m_generation )
                Utils::copySharedMedia(*result, project.m_projectRoot, cover);
            return result;
        });
    m_beatmapResourceImportError =
        m_pendingRename.valid()
            ? std::string{}
            : TR("ui.settings.beatmap.import_failed").toString();
    return false;
}

/// @brief 非阻塞回收预检或副本结果，复核绑定后才能应用。
bool SettingsView::pollBeatmapResourceRename()
{
    // - 闲置帧或正在执行文件操作时返回，让弹窗持续显示进度状态。
    // - 仅对同一有效 future 进行一次非阻塞就绪检测。
    if ( !m_pendingRename.valid() ||
         m_pendingRename.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready )
        return false;
    auto&            engine = Logic::EditorEngine::instance();
    std::unique_lock lock(engine.getSessionMutex(), std::try_to_lock);
    if ( !lock ) return false;
    // - 工作者不持有实时模型，回收时才进入短锁下的会话检查。
    // - 所有会话访问局限在本次消费作用域，不将裸指针捕获到后续工作。
    auto        result   = m_pendingRename.get();
    const auto  identity = Logic::ProjectController::instance().uiSnapshot();
    auto*       project  = engine.getCurrentProject();
    const auto  session  = engine.getActiveSession();
    const auto& request  = result->m_sourceIdentity;
    // - 目标字段、工程代次、活动对象及确认名称都仍需匹配。
    // - 仅靠文件复制成功不能证明结果应交给当前打开的设置页。
    if ( m_renameResourceTarget == BeatmapResourceTarget::None || !identity ||
         identity->m_generation != result->m_generation ||
         identity->m_isTemporaryProject || !project || !session ||
         request->session.lock() != session ||
         !session->getContext().currentBeatmap ||
         request->beatmap.lock() != session->getContext().currentBeatmap ||
         result->m_newName != m_resourceRenameBuffer.data() ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return false;
    }
    const auto& live = session->getContext().currentBeatmap->m_baseMapMetadata;
    const auto& current =
        m_renameResourceTarget == BeatmapResourceTarget::Audio
            ? (live.song_file_hint.empty() ? live.main_audio_path
                                           : live.song_file_hint)
            : (m_renameResourceTarget == BeatmapResourceTarget::Cover
                   ? live.cover_path
                   : live.main_cover_path);
    // - 选定路径可能在项目代次不变时被另一个导入命令替换。
    // - 执行前确认仍指向原资源，避免把结果写到更新后的引用。
    if ( current != m_resourceRenamePath ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return false;
    }
    // - 错误文本属于此次预检或复制，成功前不会清掉当前资源。
    // - 需要复制且未实际复制表示只完成预检，需要界面二次授权。
    const auto& plan             = result->m_plan;
    m_beatmapResourceImportError = plan.m_error;
    if ( !plan.m_error.empty() ) return false;
    // - 保存此次授权相关名称而非仅保存一个布尔量。
    // - 引用计数用于解释为何走副本，不能在普通帧重新扫描。
    if ( plan.m_requiresCopy && !result->m_copied ) {
        m_resourceRenameCopyPending    = true;
        m_resourceRenameCopyName       = result->m_newName;
        m_resourceRenameReferenceCount = plan.m_references.size();
        return false;
    }
    if ( result->m_copied ) {
        // 复制只移动本谱面的字段。文件完整落盘后才注册克隆的主轨配置。
        if ( result->m_audio ) {
            const auto& cloned = *result->m_audio;
            const auto  existing =
                std::find_if(project->m_audioResources.begin(),
                             project->m_audioResources.end(),
                             [&](const auto& resource) {
                                 return resource.m_id == cloned.m_id;
                             });
            // - 监视器可能已把副本登记在实时表中，接受前检查类型与规范路径。
            // - 同名 ID 若已被其他实体占用，不能覆盖它的音轨设置。
            const bool discovered = existing != project->m_audioResources.end();
            if ( discovered &&
                 (existing->m_type != AudioTrackType::Main ||
                  Config::utf8ToPath(existing->m_path).lexically_normal() !=
                      Config::utf8ToPath(cloned.m_path).lexically_normal()) ) {
                m_beatmapResourceImportError = "目标音频 ID 已被其它资源使用";
                return false;
            }
            // 目录监视可能先登记刚复制的路径。复用该条目并继承源配置，不插入重复
            // ID。
            const auto previous = discovered
                                      ? std::optional<AudioResource>(*existing)
                                      : std::nullopt;
            // - 保存排除表作为事务回滚依据，注册时解除新文件的排除。
            // - Project 保存不成功时恢复内存表的两个受影响部分。
            const auto previousExcluded = project->m_excludedAudioPaths;
            std::erase(project->m_excludedAudioPaths, cloned.m_path);
            if ( discovered )
                *existing = cloned;
            else
                project->m_audioResources.push_back(cloned);
            // - 先恢复原登记或删除本次追加项，再恢复排除列表。
            // - 副本留在磁盘以支持恢复，项目引用仍指向最后成功的版本。
            if ( !Logic::ProjectController::instance().saveProject() ) {
                if ( previous )
                    *existing = *previous;
                else
                    project->m_audioResources.pop_back();
                project->m_excludedAudioPaths = previousExcluded;
                Logic::ProjectController::instance().saveProject();
                // 副本已经在原工程落盘，保留恢复材料，不能删除可能已被其他入口选择的文件。
                m_beatmapResourceImportError = "无法保存项目，已保留媒体副本";
                return false;
            }
        }
        // 从实时值开始，仅保留原计划中仍对应同一旧值的字段。用户在复制期间
        // 单独选了另一张背景时，重命名封面的结果不得还原新的背景。
        auto       updated       = live;
        const auto copyUnchanged = [&](auto BaseMapMeta::* field) {
            if ( live.*field == result->m_baseline.*field )
                updated.*field = result->m_updated.*field;
        };
        // - 兼容主音频引用字段与两种视觉路径各按独立差量合并。
        // - 多项同时指向同一媒体时，只刷新仍保持原值的字段。
        copyUnchanged(&BaseMapMeta::song_file_hint);
        copyUnchanged(&BaseMapMeta::song_file_hint_cleared);
        copyUnchanged(&BaseMapMeta::main_audio_path);
        copyUnchanged(&BaseMapMeta::cover_path);
        copyUnchanged(&BaseMapMeta::main_cover_path);
        Logic::submitBeatmapSettingsCommand(
            *request,
            Logic::CmdUpdateBeatmapMetadata{
                .baseMeta = updated,
                .settingsBaseline =
                    std::make_shared<const BaseMapMeta>(live) });
        // - 设置草稿更新的是此次托管字段；文本及其他用户未保存值继续保留。
        // - 实际持久化仍由已排队的逻辑命令完成，保证与同会话编辑顺序一致。
        m_editingMeta.song_file_hint  = updated.song_file_hint;
        m_editingMeta.main_audio_path = updated.main_audio_path;
        m_editingMeta.cover_path      = updated.cover_path;
        m_editingMeta.main_cover_path = updated.main_cover_path;
        // - 已成功提交该次复制后释放二次确认状态。
        // - 再次打开须基于新引用重新计算是否共享。
        m_resourceRenameCopyPending = false;
        return true;
    }
    const auto& newName = result->m_newName;
    // 独占资源继续采用既有事务，保留其路径、历史和磁盘引用的一致性处理。
    // 不让这次共享资源修复改变其他改名入口的项目级语义。
    m_resourceRenameCopyPending = false;
    if ( m_renameResourceTarget == BeatmapResourceTarget::Audio ) {
        // 音频必须通过现有事务，同时迁移资源 ID、玩家绑定和采样引用。
        // 这里先按登记路径匹配，旧格式的纯 ID 提示仍可直接命中资源。
        const auto absolute = [&](const std::filesystem::path& value) {
            // 项目资源表可能存相对路径，比较前统一到同一项目根。
            // 词法归一不读取磁盘，避免遍历资源时反复查询文件系统。
            return (value.is_absolute() ? value
                                        : project->m_projectRoot / value)
                .lexically_normal();
        };
        const auto resource = std::find_if(
            project->m_audioResources.begin(),
            project->m_audioResources.end(),
            [&](const auto& item) {
                // 只允许主音频，音效同名时不能误改谱面背景音乐的其他资源。
                return item.m_type == AudioTrackType::Main &&
                       (absolute(Config::utf8ToPath(item.m_path)) ==
                            absolute(current) ||
                        item.m_id == Config::pathToUtf8(current));
            });
        if ( resource == project->m_audioResources.end() ) {
            // 未登记的提示字段不代表有效主轨，要求先导入建立完整资源身份。
            m_beatmapResourceImportError =
                TR("ui.settings.beatmap.rename_missing_audio").toString();
            return false;
        }
        // - 独占资源直接改名的命令会改变磁盘路径，先要求真实目标留在当前工程。
        // - canonical 用于识别链接最终去向，不能以表面相对路径替代。
        std::error_code pathError;
        const auto      root =
            std::filesystem::canonical(project->m_projectRoot, pathError);
        // - 工程根错误时不执行第二次解析，保持第一次失败原因。
        // - 源路径为实际文件；词法相对比较只在取得两个路径后才有意义。
        const auto source =
            pathError ? std::filesystem::path{}
                      : std::filesystem::canonical(
                            absolute(Config::utf8ToPath(resource->m_path)),
                            pathError);
        const auto relative = source.lexically_relative(root);
        // 根据路径分量而非字符串前缀检查，防止同名前缀的相邻目录被误认。
        // 空相对路径也不能代表有效音频文件，须作为无法解析的目标拒绝。
        // 链接指向项目外时不允许从此入口改动外部资源；重命名不是资源迁移。
        // canonical 的错误也在实际改名前拒绝，项目模型不会产生部分变更。
        if ( pathError || relative.empty() || relative.is_absolute() ||
             *relative.begin() == ".." ) {
            m_beatmapResourceImportError =
                TR("ui.settings.beatmap.rename_outside_project").toString();
            return false;
        }
        // 同步事务返回实际失败原因，仍保留全局资源变更事件用于缓存刷新。
        engine.handleRenameAudioResource({ resource->m_id, newName },
                                         &m_beatmapResourceImportError);
    } else {
        // 图片与视频共用文件事务，媒体类型及其他未保存字段不变。
        // 不从新名称猜测类型，视频改名后仍继续由视频解码管线消费。
        // 事务返回错误时页面绑定未改，不需要 UI 手动拼接回滚路径。
        m_beatmapResourceImportError =
            engine.renameBeatmapVisualResource(current, newName);
    }
    // - 错误处理由下层文件事务负责；非空失败说明使窗口保留原始输入供重试。
    // - 只有真实事务成功后才刷新草稿字段。
    if ( !m_beatmapResourceImportError.empty() ) return false;
    // 逻辑层已经完成真实文件与引用事务，不再提交第二份完整元数据命令。
    // 额外命令可能重复提交，并覆盖并发刷新过的结构字段。
    // 刷新全部资源字段，封面/背景共用文件时不能只刷新点击的那一行。
    // 文本草稿保留，避免低频资源操作覆盖尚在编辑的标题和作者。
    m_editingMeta.song_file_hint = live.song_file_hint;
    // 旧兼容字段也同步，避免下一次整份元数据提交重新带入第二个旧引用。
    m_editingMeta.main_audio_path = live.main_audio_path;
    m_editingMeta.cover_path      = live.cover_path;
    // 两张视觉资源独立时，未命中的字段保持原值，不能隐式重新绑定。
    m_editingMeta.main_cover_path = live.main_cover_path;
    // 类型与时间偏移没有改变，下一帧仍由正常结构字段同步读取。
    return true;
}
}  // namespace MMM::UI
