#include "ui/imgui/manager/SettingsView.h"

#include "common/AudioInfoUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/Metadata.h"
#include "mmm/project/AudioResource.h"
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
#include <string>

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
/// 元数据更新始终经过逻辑命令，以保留撤销与自动保存语义。

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
/// 最终使用 CmdUpdateBeatmapMetadata 保持采样重定向和撤销逻辑一致。
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
    const bool confirm =
        FeedbackButton(TR("ui.file_manager.context.rename").data());
    // 键盘确认和鼠标确认走同一个事务，不能重复改名或跳过错误处理。
    // 成功才关闭，名称冲突、权限失败等都允许原地重试。
    if ( (confirm || enter) && renameBeatmapResource() ) {
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
bool SettingsView::renameBeatmapResource()
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
    // 额外命令会生成无意义的撤销步骤，并可能覆盖并发刷新过的结构字段。
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
