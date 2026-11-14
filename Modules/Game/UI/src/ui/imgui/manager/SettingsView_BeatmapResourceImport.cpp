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
#include "logic/EditorEngine.h"
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
#include "ui/utils/ProjectResourceImport.h"
#include "ui/utils/UIWidgetUtils.h"

#include <ImGuiFileDialog.h>
#include <algorithm>
#include <filesystem>
#include <imgui.h>
#include <mutex>
#include <nfd.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
            m_beatmapResourceTarget = BeatmapResourceTarget::None;
            return;
        }

        // IGFD 的固定 key 与主菜单音频选择器、创建向导选择器相互隔离。
        // 单选和只读文件名避免把未存在的手输名称当成已选资源。
        IGFD::FileDialogConfig config;
        config.path              = settings.lastFilePickerPath;
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

    if ( !dialog->IsOpened(PICKER_ID) ) return;
    // 弹窗显示和结果消费只在内置模式运行；取消不导入也不写错误。
    // Close 必须晚于 IsOk 和 GetFilePathName，下一帧不沿用旧结果。
    Utils::CenteredModalPopupScope scope(dpiScale);
    Utils::prepareCenteredModalWindow({ 600, 400 });
    if ( dialog->Display(
             PICKER_ID, ImGuiWindowFlags_NoCollapse, { 600, 400 }) ) {
        if ( dialog->IsOk() ) {
            const auto source = Config::utf8ToPath(dialog->GetFilePathName());
            importBeatmapResource(source);
            // 最近浏览目录属于编辑器设置，不混入谱面元数据。
            auto& engine = Logic::EditorEngine::instance();
            auto  config = engine.getEditorConfig();
            config.settings.lastFilePickerPath = dialog->GetCurrentPath();
            engine.setEditorConfig(config);
        }
        dialog->Close();
        m_beatmapResourceTarget = BeatmapResourceTarget::None;
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
/// 项目根和谱面路径作为一次选择事务的身份，复制前在锁内同时核对。
/// 文件成功复制后才改设置页草稿，失败不碰用户输入的其他元数据。
/// 主音频登记后反查资源表，防止绑定到没有 Main 条目的普通文件。
/// 已登记的同路径文件即使导入命令去重，也仍允许绑定为谱面主音频。
/// 最终使用 CmdUpdateBeatmapMetadata 保持采样重定向和保存逻辑一致。
/// @warning 仅确认文件后执行复制与命令提交，不能从每帧设置绘制路径调用。
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
    if ( audio && !MMM::Utils::AudioInfoUtils::probeAudioInfo(source) ) {
        // 无法探测的音频不应复制进工程，也不能成为谱面的主时间基准。
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_failed").toString();
        return;
    }

    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    // 单独比较谱面文件名不足以区分工程，必须同时核对工程根目录。
    // 两项比较都在同一锁内，避免校验后会话被替换再绑定旧路径。
    auto* project = engine.getCurrentProject();
    auto  session = engine.getActiveSession();
    if ( !project ||
         Logic::ProjectController::instance().isCurrentProjectTemporary() ||
         !session || !session->getContext().currentBeatmap ||
         project->m_projectRoot != m_resourceImportProjectRoot ||
         session->getContext().currentBeatmap->m_baseMapMetadata.map_path !=
             m_resourceImportBeatmapPath ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return;
    }

    const auto imported =
        Utils::importProjectResource(project->m_projectRoot, source);
    if ( !imported ) {
        // error_code 保留权限、路径和磁盘错误，而不是用泛化失败覆盖诊断。
        // 复制失败不修改元数据，也不登记可能并不存在的音频资源。
        // 用户可保留当前选择器目录，在下次交互时换一个源文件。
        m_beatmapResourceImportError = imported.error().message();
        return;
    }
    auto updatedMeta = m_editingMeta;
    // 标题等文本继续采用本页草稿；可能被其他入口改动的资源和结构字段
    // 则重新取会话当前值，避免文件选择器打开期间的变化被整份命令覆盖。
    // 音频与封面/背景分别刷新，随后只覆盖本次按钮负责的目标字段。
    // 视频起点和偏移也属于背景结构，不应因为导入封面而回退旧值。
    const auto& liveMeta =
        session->getContext().currentBeatmap->m_baseMapMetadata;
    updatedMeta.track_count     = liveMeta.track_count;
    updatedMeta.song_file_hint  = liveMeta.song_file_hint;
    updatedMeta.main_audio_path = liveMeta.main_audio_path;
    updatedMeta.cover_path      = liveMeta.cover_path;
    updatedMeta.main_cover_path = liveMeta.main_cover_path;
    updatedMeta.cover_type      = liveMeta.cover_type;
    updatedMeta.video_starttime = liveMeta.video_starttime;
    updatedMeta.bgxoffset       = liveMeta.bgxoffset;
    updatedMeta.bgyoffset       = liveMeta.bgyoffset;
    // 这份快照仍在同一会话锁内，赋值过程中不会与逻辑线程的修改交错。
    // 命令提交后由会话规范化路径，UI 不私自改写当前 BeatMap 对象。
    if ( audio ) {
        // 导入既有工程文件时命令可能因去重返回“未新增”；仍需确认资源表
        // 中确实有对应 Main 条目，不能只凭文件复制成功就绑定谱面。
        engine.handleImportAudio(
            { Config::pathToUtf8(project->m_projectRoot / *imported),
              AudioTrackType::Main });
        const auto resource = std::find_if(
            project->m_audioResources.begin(),
            project->m_audioResources.end(),
            [&](const auto& item) {
                return item.m_type == AudioTrackType::Main &&
                       Config::utf8ToPath(item.m_path).lexically_normal() ==
                           imported->lexically_normal();
            });
        if ( resource == project->m_audioResources.end() ) {
            m_beatmapResourceImportError =
                TR("ui.settings.beatmap.import_failed").toString();
            return;
        }
        // 外部格式提示字段优先于旧主音频路径，保持与下拉选择一致。
        updatedMeta.song_file_hint = *imported;
        updatedMeta.main_audio_path.clear();
    } else if ( m_beatmapResourceTarget == BeatmapResourceTarget::Cover ) {
        updatedMeta.cover_path = *imported;
    } else {
        // 类型取请求快照；视频不能作为缩略图的隐式封面。
        updatedMeta.main_cover_path = *imported;
        updatedMeta.cover_type = video ? CoverType::VIDEO : CoverType::IMAGE;
        if ( !video && updatedMeta.cover_path.empty() )
            updatedMeta.cover_path = *imported;
    }

    // 文件已复制并登记后只发送一次完整元数据命令，保留同帧其他字段草稿。
    m_editingMeta = updatedMeta;
    engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ updatedMeta });
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
        // 新请求不能继承上一轮复制授权，取消后重新打开也必须重新提示。
        m_resourceRenameCopyPending = false;
        m_resourceRenameCopyName.clear();
        FeedbackOpenPopup(title.c_str());
        m_openResourceRenamePopup = false;
    }
    bool open = true;
    // 模态父上下文来自设置窗口；不能放回 Clay 回调里打开阻塞式交互。
    // 窗口宽度只应用于弹窗，不扩大底层设置行或路径组合框。
    Utils::CenteredModalPopupScope scope(dpiScale);
    if ( !scope.begin(title.c_str(),
                      &open,
                      ImGuiWindowFlags_NoCollapse,
                      { 420.0F * dpiScale, 0.0F }) )
        return;
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
    ImGui::EndPopup();
    // 标题栏关闭与取消都结束目标身份；下次点击完整重新准备缓冲。
    if ( !open ) m_renameResourceTarget = BeatmapResourceTarget::None;
}

/// @brief 对仍然绑定的原资源执行重命名并刷新设置草稿中的引用。
/// @return 文件与引用同步成功返回 true，失败保留具体原因。
/// @warning 显式确认路径：持有会话注册表锁，只允许低频调用。
bool SettingsView::renameBeatmapResource(bool confirmCopy)
{
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> lock(engine.getSessionMutex());
    // 校验与事务共用注册表锁，防止验证之后切谱或切项目。
    // 获取会话所有权只发生在确认分支，不引入普通帧的共享所有权复制。
    auto* project = engine.getCurrentProject();
    auto  session = engine.getActiveSession();
    // 对话框期间切换项目、谱面或转为临时工程时拒绝陈旧请求。
    if ( !project || !session || !session->getContext().currentBeatmap ||
         Logic::ProjectController::instance().isCurrentProjectTemporary() ||
         project->m_projectRoot != m_resourceRenameProjectRoot ||
         session->getContext().currentBeatmap->m_baseMapMetadata.map_path !=
             m_resourceRenameBeatmapPath ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return false;
    }
    const auto& live = session->getContext().currentBeatmap->m_baseMapMetadata;
    // 当前字段从真实会话读取，不用设置草稿作为“资源仍绑定”的证据。
    // 封面与背景分别判断，即使两个字段当前共享文件也保留各自请求身份。
    const auto& current =
        m_renameResourceTarget == BeatmapResourceTarget::Audio
            ? (live.song_file_hint.empty() ? live.main_audio_path
                                           : live.song_file_hint)
        : m_renameResourceTarget == BeatmapResourceTarget::Cover
            ? live.cover_path
            : live.main_cover_path;
    if ( current != m_resourceRenamePath ||
         m_renameResourceTarget == BeatmapResourceTarget::None ) {
        // 不能把旧弹窗应用到已重新绑定的资源，即便文件名仍然相似。
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return false;
    }
    // 身份全部通过后才访问文件系统；陈旧请求不会碰到原项目的任何文件。
    // 文件冲突和扩展名变化留给事务层处理，保持两种媒体的校验口径一致。
    const std::string newName = m_resourceRenameBuffer.data();
    // 输入仅传给逻辑事务，由它保留格式后缀并拒绝移动目录和覆盖。
    // UI 不预先改草稿；任何失败都必须继续显示旧的有效绑定。
    // 共享资源预检独立于项目全局改名事务，未授权复制前只有只读查询。
    // 不能根据旧事务的错误字符串猜测是否需要复制，路径或格式错误仍应直接失败。
    const bool audioTarget =
        m_renameResourceTarget == BeatmapResourceTarget::Audio;
    const auto* audio =
        audioTarget
            ? Logic::ProjectResourceService::findAudioResourceForReference(
                  *project, live.map_path, Config::pathToUtf8(current))
            : nullptr;
    if ( audioTarget && (!audio || audio->m_type != AudioTrackType::Main) ) {
        m_beatmapResourceImportError =
            TR("ui.settings.beatmap.rename_missing_audio").toString();
        return false;
    }
    // 音频提示允许资源 ID，因此文件身份必须来自已经解析的项目资源表。
    const auto source = audio ? Config::utf8ToPath(audio->m_path) : current;
    // 调用方已持有会话注册表锁，观察指针仅用于这次预检。
    // 复制会话列表只在显式确认发生，普通渲染帧不会复制 shared_ptr 所有权。
    std::vector<const BeatMap*> openMaps;
    const auto                  entries = engine.getSessionEntries();
    for ( const auto& entry : entries )
        if ( !entry.isLogoPlaceholder && entry.session &&
             entry.session->getContext().currentBeatmap )
            openMaps.push_back(
                entry.session->getContext().currentBeatmap.get());
    // 候选列表来自磁盘和打开模型；每次确认都重新检查，不复用陈旧授权结果。
    const auto plan = Logic::SharedResourceRenameService::inspect(
        *project, live.map_path, source, newName, audio, openMaps);
    m_beatmapResourceImportError = plan.m_error;
    // 名称冲突、扩展名变更和不可检查来源不能走“复制即可”的兜底。
    // 所有校验错误都保留原资源引用，让用户可修正后重试。
    if ( !plan.m_error.empty() ) return false;
    if ( plan.m_requiresCopy ) {
        if ( !confirmCopy || !m_resourceRenameCopyPending ||
             m_resourceRenameCopyName != newName ) {
            // 第一次点击只解释复制后果，不创建文件、不改资源表、不提交元数据。
            m_resourceRenameCopyPending    = true;
            m_resourceRenameCopyName       = newName;
            m_resourceRenameReferenceCount = plan.m_references.size();
            return false;
        }
        // 第二次确认仍重新运行了预检，新增关联或源文件消失不会沿用旧结果。
        // 这里提前返回，禁止复制成功后再次调用会搬走原文件的全局重命名。
        return copyRenameBeatmapResource(plan, audio);
    }
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
        std::error_code pathError;
        const auto      root =
            std::filesystem::canonical(project->m_projectRoot, pathError);
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
/// @brief 用户确认共享提示后建立新文件，以正常元数据动作改绑当前谱面。
/// @details 原文件、其他谱面及其撤销历史完全保留。副本沿用编码字节与音轨配置。
/// 图片共享封面和背景时两字段一起改绑；音频使用已有 Main BGM 重定向语义。
/// @warning 仅在会话锁保护的显式确认路径执行，不从普通 UI 帧调用。
bool SettingsView::copyRenameBeatmapResource(
    const Logic::SharedResourceRenamePlan& plan, const AudioResource* audio)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto  session = engine.getActiveSession();
    // 调用方已经验证身份，仍避免未来调用者绕过必要项目和会话前置条件。
    if ( !project || !session || !session->getContext().currentBeatmap )
        return false;
    // 相对路径写入项目根而非谱面目录，保持与正常资源导入的保存规范一致。
    // 文件系统错误使用返回值处理，不能引入会抛异常的 canonical 重载。
    std::error_code rootError;
    const auto      root =
        std::filesystem::canonical(project->m_projectRoot, rootError);
    if ( rootError ) {
        m_beatmapResourceImportError = rootError.message();
        return false;
    }
    // 计划中的目标已在源目录下，当前工程再次规范化仅用于生成持久化相对路径。
    // 不把开发机的绝对路径写入歌曲提示，工程整体移动后仍可解析副本。
    const auto relative = plan.m_destination.lexically_relative(root);
    // 先构造草稿，成功前不改当前模型；谱面字段由逻辑命令标脏和安排保存。
    // 源模型的资源和结构字段可能比设置草稿更新，以确认当时的真实会话为准。
    // 复制只替换对应资源字段，不提交尚未确认的标题和作者等编辑。
    auto updated = session->getContext().currentBeatmap->m_baseMapMetadata;
    // 音轨复制前先持有完整值快照，新增资源可能重分配项目 vector。
    // 后续不再借用 audio 指向的元素，避免登记新轨后解引用失效地址。
    std::optional<AudioResource> cloned;
    if ( audio ) {
        cloned         = *audio;
        cloned->m_id   = Config::pathToUtf8(plan.m_destination.filename());
        cloned->m_path = Config::pathToUtf8(relative);
        updated.song_file_hint = relative;
        updated.main_audio_path.clear();
    } else {
        Logic::VisualResourceRenameService::remapMetadata(
            updated, project->m_projectRoot, plan.m_source, plan.m_destination);
        // 源文件可能由符号链接解析到真实路径，但当前字段仍保存别名。
        // 背景成功映射不代表用户正在重命名的封面也已映射，必须检查选中字段。
        const auto& original =
            session->getContext().currentBeatmap->m_baseMapMetadata;
        const bool selectionChanged =
            m_renameResourceTarget == BeatmapResourceTarget::Cover
                ? updated.cover_path != original.cover_path
                : updated.main_cover_path != original.main_cover_path;
        if ( !selectionChanged ) {
            // 无法投影时在复制之前拒绝，不能创建副本后沿用旧绑定却误报成功。
            m_beatmapResourceImportError =
                "无法映射当前资源路径，请重新选择该资源后重试";
            return false;
        }
    }
    std::error_code error;
    // 精确配套 BPM 文件的协议名按完整音频文件名追加，不能与另一格式共用。
    auto companion = plan.m_destination;
    companion += ".mmm-timing.json";
    // 原文件配套标记按只读来源复制，目标已有配套数据也不能覆盖。
    // 即使目标音频尚不存在，旧测量旁路仍可能属于用户保留的数据。
    if ( audio && (std::filesystem::exists(companion, error) || error) ) {
        m_beatmapResourceImportError = "目标音频的配套测量文件已存在或不可访问";
        return false;
    }
    // 本次副本将长期属于项目，不是删除原文件后的临时回滚材料。
    // 其它谱面仍依赖原文件，因此复制成功也绝不删除或重命名源路径。
    // copy_options::none 拒绝覆盖；绝不先改名再尝试修复其他谱面的引用。
    if ( !std::filesystem::copy_file(plan.m_source,
                                     plan.m_destination,
                                     std::filesystem::copy_options::none,
                                     error) ||
         error ) {
        m_beatmapResourceImportError = "复制资源失败：" + error.message();
        return false;
    }
    // 先完成所有媒体字节和可选配套数据，再登记新资源并发出改绑命令。
    // 失败不会把尚不存在的音轨发布给当前谱面的播放描述。
    if ( audio ) {
        error = Audio::AudioMarkerService::copyCompanion(plan.m_source,
                                                         plan.m_destination);
        if ( error ) {
            // 配套数据损坏或写出失败时不登记新主轨，原文件始终可继续使用。
            std::error_code cleanup;
            std::filesystem::remove(plan.m_destination, cleanup);
            m_beatmapResourceImportError =
                "复制音频测量数据失败：" + error.message();
            return false;
        }
        // 克隆保持原主轨音量、变速、变调和静音配置，不采用导入音频的默认值。
        // 原资源条目保留，供 IMD 和其它谱面继续按原 ID、原路径解析。
        // 新资源以新文件名为独立主键，其他工作区的选轨也不做全局迁移。
        // 用户明确创建新资源时撤销该路径的历史排除，避免重开项目后扫描忽略副本。
        // 保存失败需同时恢复排除表，不能只回退音轨登记。
        const auto previousExcluded = project->m_excludedAudioPaths;
        std::erase(project->m_excludedAudioPaths, cloned->m_path);
        project->m_audioResources.push_back(*cloned);
        if ( !Logic::ProjectController::instance().saveProject() ) {
            // 先恢复资源表，再尽力恢复项目清单；只删除本次新建的副本。
            project->m_audioResources.pop_back();
            project->m_excludedAudioPaths = previousExcluded;
            // 项目保存可能已更新部分分片，需要再保存旧表而不只改回内存。
            // 恢复无法确认时保留副本，防止磁盘清单仍引用它却把文件删除。
            const bool restored =
                Logic::ProjectController::instance().saveProject();
            if ( restored ) {
                std::error_code cleanup;
                std::filesystem::remove(plan.m_destination, cleanup);
                // 目标配套文件在创建前已确认不存在，成功复制后属于本次事务。
                // 不触碰源文件配套数据，也不删除工程中的其它测量结果。
                std::filesystem::remove(companion, cleanup);
            }
            m_beatmapResourceImportError =
                restored
                    ? "保存项目失败，复制已取消"
                    : "保存及恢复项目清单失败，副本已保留，请检查项目目录权限";
            return false;
        }
    }
    // 发送到确认过的会话，不能在命令消费前因活动标签切换误改另一张谱面。
    // 正常元数据更新负责主 BGM 采样改绑、派生状态和尾随保存。
    // 该入口沿用设置页既有提交语义，不重写其它会话的元数据或历史。
    // 原文件继续存在，既有引用不会因当前谱面的副本选择而失效。
    // 复制不触发项目级旧路径到新路径的广播或全局主键迁移。
    session->pushCommand(Logic::CmdUpdateBeatmapMetadata{ updated });
    // 仅投影资源字段，保留设置页尚未提交的标题、作者等文字草稿。
    // 已提交的资源路径立即反映在设置行，但真实模型仍由指定会话消费命令。
    // 保留整份 live 元数据作为命令来源，避免把设置中的旧结构字段覆盖回去。
    m_editingMeta.song_file_hint  = updated.song_file_hint;
    m_editingMeta.main_audio_path = updated.main_audio_path;
    m_editingMeta.cover_path      = updated.cover_path;
    m_editingMeta.main_cover_path = updated.main_cover_path;
    // 完成后清除授权位；再次打开改名窗口必须重新检查是否还有其它谱面共享。
    // 既有错误信息只在成功时清除，失败不会被空泛成功提示覆盖。
    m_resourceRenameCopyPending = false;
    m_beatmapResourceImportError.clear();
    return true;
}
}  // namespace MMM::UI
