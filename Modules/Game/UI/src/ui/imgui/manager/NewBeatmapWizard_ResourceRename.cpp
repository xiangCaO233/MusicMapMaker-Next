#include "ui/imgui/manager/NewBeatmapWizard.h"

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/EditorEngine.h"
#include "logic/VisualResourceRenameService.h"
#include "mmm/project/Project.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace MMM::UI
{
/// @brief 冻结向导资源选择和项目身份，准备跨帧重命名输入。
/// @param target 音频、封面或背景，不能用导入选择器的可变目标代替。
/// @details 请求冻结项目根、资源路径及音频 ID，不保存 Project 或 Session
/// 所有权。 只从已经选中的资源提取文件名，不在此遍历目录或打开新文件。
/// 未绑定字段不产生请求，音频入口由父行遵守自动分析期间的禁用约束。
/// 临时只读工程在入口和确认阶段均拒绝改名，不能仅依赖按钮外观。
/// 名称输入保留扩展名，最终格式保护与重名检测由共享事务校验。
/// 开始新请求会清除上次改名错误，但不会清除资源导入的其他错误。
/// buffer 在向导整个生命周期内稳定，ImGui 不借用栈上临时名称。
/// @warning 用户点击入口，锁内只复制身份与文件名，不执行磁盘事务。
void NewBeatmapWizard::requestResourceRename(ResourceTarget target)
{
    auto&           engine = Logic::EditorEngine::instance();
    std::lock_guard lock(engine.getSessionMutex());
    const auto*     project = engine.getCurrentProject();
    // 未创建谱面也能拥有正式项目，因此不要求存在活动会话。
    // 入口失败不打开模态框，确认函数仍会复核防止上下文后来变化。
    if ( !project || project->m_isTemporaryProject ) return;
    const auto& selected = target == ResourceTarget::Audio ? m_selectedAudioPath
                           : target == ResourceTarget::Cover
                               ? m_selectedCoverImgPath
                               : m_selectedCoverPath;
    if ( selected.empty() ) return;
    // 仅取最终文件名，保留目录选择，重命名不是资源移动入口。
    const auto name = Config::pathToUtf8(selected.filename());
    // 容量检查在复制前完成，不能以截断后的文件名发起文件事务。
    if ( name.size() >= m_renameNameBuffer.size() ) {
        m_resourceImportError =
            TR("ui.settings.beatmap.rename_name_too_long").toString();
        return;
    }
    m_renameTarget      = target;
    m_renameProjectRoot = project->m_projectRoot;
    m_renameSourcePath  = selected;
    m_renameAudioId     = m_selectedAudioTrackId;
    // 先补 NUL 再复制，旧长文件名不能泄漏到较短的新输入后面。
    m_renameNameBuffer.fill('\0');
    std::memcpy(m_renameNameBuffer.data(), name.data(), name.size());
    // 独立弹窗状态保证导入取消或目标切换不会改变正在确认的资源。
    // 每次尝试重新获取当前失败原因，不把旧错误当作本轮结果。
    m_resourceRenameError.clear();
    m_openRenamePopup  = true;
    m_focusRenameInput = true;
}

/// @brief 绘制嵌套在新建向导中的资源改名确认框。
/// @param dpiScale 用户内容缩放，用于稳定弹窗宽度。
/// @details 该入口必须在父向导 EndPopup 之前调用，使改名属于同一模态栈。
/// 可见标题允许翻译，固定内部 ID 防止语言切换产生第二个窗口身份。
/// 首帧选中旧名称，随后只响应用户输入而不反复设置键盘焦点。
/// Enter 和按钮复用确认函数，成功才关闭，失败允许原地修正后重试。
/// 取消与标题栏关闭均不执行文件事务，也不回滚已经存在的资源选择。
/// 提示使用现有谱面设置的文案，解释改名与格式转换之间的边界。
/// 改名和导入拥有独立输入状态，不能通过资源选择器取消另一种操作。
/// @warning 每帧只处理 ImGui 状态，文件改名仅在 Enter 或确认按钮触发。
void NewBeatmapWizard::renderResourceRename(float dpiScale)
{
    const auto title = TR("ui.file_manager.rename_title").toString() +
                       "###WizardResourceRename";
    if ( m_openRenamePopup ) {
        // 父 popup 内打开，避免模态向导将资源改名弹窗挡在后方。
        FeedbackOpenPopup(title.c_str());
        m_openRenamePopup = false;
    }
    bool                           open = true;
    Utils::CenteredModalPopupScope scope(dpiScale);
    if ( !scope.begin(title.c_str(),
                      &open,
                      ImGuiWindowFlags_NoCollapse,
                      { 420.0F * dpiScale, 0.0F }) )
        return;
    ImGui::TextWrapped("%s", TR("ui.settings.beatmap.rename_hint").data());
    // 焦点只申请一次，用户调整光标时不被每帧全选打断。
    if ( m_focusRenameInput ) {
        ImGui::SetKeyboardFocusHere();
        m_focusRenameInput = false;
    }
    ImGui::SetNextItemWidth(-1.0F);
    // 输入缓冲跨帧稳定，失败保留原输入，支持原地修正名称后重试。
    // 输入框只编辑名称，确认前不改变下拉选择或项目资源表。
    const bool enter = ImGui::InputText("##WizardResourceName",
                                        m_renameNameBuffer.data(),
                                        m_renameNameBuffer.size(),
                                        ImGuiInputTextFlags_EnterReturnsTrue |
                                            ImGuiInputTextFlags_AutoSelectAll);
    if ( !m_resourceRenameError.empty() )
        ImGui::TextWrapped("%s", m_resourceRenameError.c_str());
    // 只收集确认意图，键盘与鼠标都只能提交一次事务。
    const bool confirm =
        FeedbackButton(TR("ui.file_manager.context.rename").data());
    if ( (enter || confirm) && renameSelectedResource() )
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if ( FeedbackButton(TR("ui.common.cancel").data()) ) {
        // 取消只关闭确认框，不更改向导绑定或项目文件。
        // 每次尝试重新获取当前失败原因，不把旧错误当作本轮结果。
        m_resourceRenameError.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    // 标题栏关闭同样不产生事务；下次请求会完整替换冻结身份。
}

/// @brief 改名已有项目资源并将向导绑定投影到新路径。
/// @return 成功返回 true，失败返回 false 并保留具体原因。
/// @warning 显式确认的低频路径，串行化项目改名与谱面保存，不从常态帧调用。
/// @details 不创建谱面，不重新探测歌曲，也不覆盖标题或已测量 Timing。
/// 请求身份与当前选择逐项复核，外部项目切换后不会写入新项目。
/// 源文件必须能解析为工程内路径，符号链接不能绕过工程边界。
/// 音频额外验证资源表的 ID、Main 类型和实际路径，而非只信任向导文本。
/// 音频事务维护项目资源 ID、磁盘谱面与打开会话，视觉事务维护封面背景引用。
/// 向导草稿在磁盘事务成功后才提交，失败不改变未创建谱面的绑定。
/// 三个选择的匹配结果在文件改名前冻结，避免符号链接悬空后无法判断。
/// 封面与背景同文件时一起迁移，不把未命中的其他资源强制改成新文件。
/// 幂等同名请求由共享事务处理，不手工执行覆盖或删除。
/// 时间线测量结果与音频内容有关，纯文件改名不应该将其清空。
bool NewBeatmapWizard::renameSelectedResource()
{
    auto&           engine = Logic::EditorEngine::instance();
    std::lock_guard lock(engine.getSessionMutex());
    auto*           project = engine.getCurrentProject();
    const auto&     selected =
        // 用途与原始选择一同冻结，后续确认不受导入用途状态影响。
        m_renameTarget == ResourceTarget::Audio   ? m_selectedAudioPath
        : m_renameTarget == ResourceTarget::Cover ? m_selectedCoverImgPath
                                                  : m_selectedCoverPath;
    // 向导不依赖活动谱面，但必须仍属于打开确认框时的正式项目和资源选择。
    if ( !project || project->m_isTemporaryProject ||
         project->m_projectRoot != m_renameProjectRoot ||
         selected != m_renameSourcePath || selected.empty() ||
         (m_renameTarget == ResourceTarget::Audio &&
          m_selectedAudioTrackId != m_renameAudioId) ) {
        m_resourceRenameError =
            TR("ui.settings.beatmap.import_context_changed").toString();
        return false;
    }
    // 路径解析失败必须作为事务失败返回，不能沿用未经验证的源路径。
    std::error_code error;
    // 根目录采用真实文件系统身份，阻止同名前缀目录被误认为工程内。
    const auto root = std::filesystem::canonical(project->m_projectRoot, error);
    const auto source =
        error ? std::filesystem::path{}
              : std::filesystem::canonical(
                    selected.is_absolute() ? selected : root / selected, error);
    // 分量级相对路径用于边界检查，不能用字符串 starts_with。
    const auto relative = source.lexically_relative(root);
    // canonical 拒绝指向工程外的链接，分量检查不能以字符串前缀代替。
    if ( error || relative.empty() || relative.is_absolute() ||
         *relative.begin() == ".." ) {
        m_resourceRenameError =
            TR("ui.settings.beatmap.rename_outside_project").toString();
        return false;
    }
    // 文件名始终作为数据处理，百分号或 Unicode 不参与格式化。
    const std::string newName  = m_renameNameBuffer.data();
    auto              fileName = Config::utf8ToPath(newName);
    // 省略扩展名沿用源格式，显式格式变更由事务拒绝。
    if ( fileName.extension().empty() ) fileName += source.extension();
    // 沿用原目录，只改文件名，不能从此 UI 请求目录迁移。
    const auto destination =
        (source.parent_path() / fileName).lexically_normal();
    const auto matches = [&](const std::filesystem::path& value) {
        if ( value.empty() ) return false;
        std::error_code pathError;
        const auto      absolute = std::filesystem::canonical(
            value.is_absolute() ? value : root / value, pathError);
        return !pathError && absolute == source;
    };
    // 改名前解析别名，文件改名后工程内符号链接可能变成悬空，不能再拿来判断身份。
    const std::array matchesSelection{ matches(m_selectedAudioPath),
                                       matches(m_selectedCoverImgPath),
                                       matches(m_selectedCoverPath) };
    // 草稿预映射在旧文件仍存在时进行，保证路径别名能正确匹配。
    auto updatedMeta = m_meta;
    Logic::VisualResourceRenameService::remapMetadata(
        updatedMeta, root, source, destination);
    // 每次尝试重新获取当前失败原因，不把旧错误当作本轮结果。
    m_resourceRenameError.clear();
    if ( m_renameTarget == ResourceTarget::Audio ) {
        const auto resource =
            std::find_if(project->m_audioResources.begin(),
                         project->m_audioResources.end(),
                         [&](const auto& item) {
                             return item.m_id == m_renameAudioId &&
                                    item.m_type == AudioTrackType::Main;
                         });
        // 资源表也必须仍指向冻结文件，不能仅凭保留下来的旧 ID 改名另一个文件。
        if ( resource == project->m_audioResources.end() ||
             !matches(Config::utf8ToPath(resource->m_path)) ) {
            m_resourceRenameError =
                TR("ui.settings.beatmap.rename_missing_audio").toString();
            return false;
        }
        // 音频事务同步文件、资源 ID、受管谱面和打开会话，不能只做 filesystem
        // rename。
        engine.handleRenameAudioResource({ m_renameAudioId, newName },
                                         &m_resourceRenameError);
    } else {
        // 视觉资源事务也支持尚无打开谱面的项目，同时维护已有谱面引用。
        m_resourceRenameError =
            engine.renameBeatmapVisualResource(selected, newName);
    }
    // 共享事务负责回滚，UI 不另做第二次文件改名或补救写入。
    if ( !m_resourceRenameError.empty() ) return false;
    // 项目事务已完成，随后仅更新向导内存，不重复保存受管谱面。
    // 封面与背景可能共享文件，两处选择必须同步，不能只更新点击的那一行。
    // 创建谱面的绑定继续使用工程相对路径，不能写入本机绝对目录。
    const auto newRelativePath = destination.lexically_relative(root);
    if ( matchesSelection[0] ) m_selectedAudioPath = newRelativePath;
    if ( matchesSelection[1] ) m_selectedCoverImgPath = newRelativePath;
    if ( matchesSelection[2] ) m_selectedCoverPath = newRelativePath;
    // 只有磁盘事务成功才提交草稿，其余标题、作者和媒体类型不变。
    m_meta = std::move(updatedMeta);
    // 媒体类型及偏移沿用草稿原值，改名不触发重新导入或转码。
    if ( m_renameTarget == ResourceTarget::Audio ) {
        // 音频 ID 按实际文件名维护，保留测量结果而不调用会清空 Timing
        // 的选择入口。
        m_selectedAudioTrackId = Config::pathToUtf8(fileName.filename());
    }
    return true;
}
}  // namespace MMM::UI
