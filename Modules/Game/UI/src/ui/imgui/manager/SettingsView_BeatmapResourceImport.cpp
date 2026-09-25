#include "ui/imgui/manager/SettingsView.h"

#include "common/AudioInfoUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
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
/// @brief 设置页的音频与图片选择、工程复制及谱面引用绑定。
/// @details 本文件只负责用户确认后的低频导入，常规设置渲染不读取文件。
///
/// 入口分为两段：Clay 行回调只记录目标及点击时的工程和谱面身份；
/// SettingsView::update 在行回调退出、会话锁释放后驱动文件选择器。
/// 原生对话框同步返回，内置对话框跨帧返回，两者共用同一导入处理。
///
/// 音频先按后缀分类再执行解码探针，图片只接受渲染器已支持的格式。
/// 工程内已有文件按相对路径复用，工程外文件按不覆盖原则复制到根目录。
/// 音频需登记为 Main 项目资源，谱面元数据随后引用该已登记路径。
/// 封面只改缩略图字段，背景图片同步改背景字段和 IMAGE 类型。
/// 错误信息保留在设置页，供用户修复源文件或目录权限后重新尝试。
/// 取消或失败不清空当前谱面的音频与图片引用。
///
/// 选择器打开时用户可能切换项目或谱面，导入前必须再次核对身份。
/// 身份不匹配时不复制、不提交旧草稿，取消选择也不改变已有引用。
/// 元数据更新始终经过逻辑命令，以保留撤销与自动保存语义。

namespace MMM::UI
{
/// @brief 在设置页锁外驱动谱面资源选择器。
/// @param dpiScale 当前 UI 内容缩放。
/// @details 按钮在 Clay 行回调中只写入请求；本函数运行时该回调持有的会话锁
/// 已释放。原生选择器同步完成，内置选择器跨帧完成，两者都调用同一导入入口。
/// 文件类型过滤与资源分类 helper 的白名单一致，封面和背景都只导入图片。
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
        // 消费一次性请求，避免原生窗口在用户取消后下帧再次弹出。
        m_openBeatmapResourcePicker = false;
        const bool audio =
            m_beatmapResourceTarget == BeatmapResourceTarget::Audio;
        const auto& settings =
            Config::AppConfig::instance().getEditorSettings();
        const char* title = TR(audio ? "ui.settings.beatmap.import_audio"
                                     : "ui.settings.beatmap.import_image")
                                .data();
        if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
            // 原生选择器可能阻塞，因此绝不从持有 session 锁的设置行调用。
            // 过滤器仅帮助用户选择，导入函数仍会重新验证真实后缀。
            PlayPopupOpenFeedback();
            nfdu8char_t*            selected = nullptr;
            const nfdu8filteritem_t filter{
                title,
                audio ? "mp3,ogg,wav,flac,opus,aac,m4a" : "png,jpg,jpeg,bmp"
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
                           audio ? ".mp3,.ogg,.wav,.flac,.opus,.aac,.m4a"
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

/// @brief 把音频或图片复制进原项目并写入原谱面的资源引用。
/// @param source 文件选择器返回的绝对或可解析路径。
/// @details 先验证类型；音频还需解码探针。项目/谱面身份与点击时快照不一致时
/// 拒绝绑定，避免用户在文件选择器打开期间切换谱面造成串写。复制复用创建向导的
/// 安全导入 helper；主音频仍经过 EditorEngine 登记并保存项目资源清单。
/// 封面和背景只改各自元数据字段，背景图片切换到 IMAGE 类型；已有封面不覆盖。
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
    if ( type != (audio ? Utils::ProjectResourceType::Audio
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
    if ( !project || !session || !session->getContext().currentBeatmap ||
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
        // 背景从视频改为图片时类型必须同步；已有显式封面保持原值。
        updatedMeta.main_cover_path = *imported;
        updatedMeta.cover_type      = CoverType::IMAGE;
        if ( updatedMeta.cover_path.empty() )
            updatedMeta.cover_path = *imported;
    }

    // 文件已复制并登记后只发送一次完整元数据命令，保留同帧其他字段草稿。
    m_editingMeta = updatedMeta;
    engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ updatedMeta });
    m_beatmapResourceImportError.clear();
}
}  // namespace MMM::UI
