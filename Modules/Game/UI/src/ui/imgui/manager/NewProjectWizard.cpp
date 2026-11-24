#include "ui/imgui/manager/NewProjectWizard.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "event/project/ProjectOpenInteractionEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectStorage.h"
#include "runtime/AppThreadPool.h"
#include "ui/UIManager.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstring>
#include <ice/thread/ThreadPool.hpp>
#include <nfd.h>
#include <string>
#include <system_error>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 统一文件选择器弹窗 ID。
///
/// 原生选择器失败时会回退到 ImGuiFileDialog，固定 ID
/// 用于跨帧查询、显示和关闭同一 个统一弹窗实例，不能随翻译文本改变。
constexpr const char* PARENT_FOLDER_PICKER_ID = "NewProjectParentFolderPicker";

/// @brief 项目信息页前进按钮的演练语义目标。
constexpr std::string_view PROJECT_INFO_NEXT_TARGET =
    "new-project.project-info.next";
/// @brief 初始偏好页前进按钮的演练语义目标。
constexpr std::string_view PREFERENCES_NEXT_TARGET =
    "new-project.preferences.next";
/// @brief 保存位置页创建按钮的演练语义目标。
constexpr std::string_view LOCATION_CREATE_TARGET =
    "new-project.location.create";

/// @brief 判断字符是否适合保留在文件夹名中。
/// @param ch 待检查字符。
/// @return 字符可保留时返回 true。
///
/// 规则采用 Windows 与常见桌面文件系统的保守非法字符并集，使同一项目目录名可以
/// 跨平台打包和迁移。这里只处理单字节 ASCII 控制符及分隔符，UTF-8
/// 字节原样保留。
bool isSafeFolderNameChar(unsigned char ch)
{
    // C0 控制字符不能出现在面向用户的目录名中。
    if ( ch < 32 ) return false;
    // 路径分隔符、通配符及 Windows 保留标点统一替换。
    switch ( ch ) {
    case '/':
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|': return false;
    // 其他字节保留；多字节 UTF-8 序列不会被拆散重编码。
    default: return true;
    }
}

/// @brief 从项目标题生成适合文件夹使用的名称。
/// @param title 项目标题。
/// @return 清理后的文件夹名。
///
/// 空白与非法字符序列折叠成单个下划线，避免自动名称出现长串占位符。末尾不保留
/// 下划线、点或空格，以兼容 Windows 路径规则；清理为空时使用稳定英文回退名。
std::string makeFolderNameFromTitle(std::string_view title)
{
    // 最坏情况下输出字节数不超过输入，提前保留容量避免循环中扩容。
    std::string result;
    result.reserve(title.size());
    // 状态位同时表示上一个输出是否已经是替代空白的下划线。
    bool lastWasSpace = false;
    for ( unsigned char ch : title ) {
        if ( std::isspace(ch) ) {
            // 丢弃前导空白，并把连续空白折叠为一个下划线。
            if ( !result.empty() && !lastWasSpace ) {
                result.push_back('_');
                lastWasSpace = true;
            }
            continue;
        }

        if ( !isSafeFolderNameChar(ch) ) {
            // 非法字符与空白采用相同折叠语义，保持名称易读且不会连接两侧词语。
            if ( !result.empty() && !lastWasSpace ) {
                result.push_back('_');
                lastWasSpace = true;
            }
            continue;
        }

        // 安全字节按原顺序写入，并恢复可插入下一个分隔符的状态。
        result.push_back(static_cast<char>(ch));
        lastWasSpace = false;
    }

    // 末尾点和空格在部分文件系统上会被隐式裁剪，因此主动得到稳定目标路径。
    while ( !result.empty() && (result.back() == '_' || result.back() == '.' ||
                                result.back() == ' ') ) {
        result.pop_back();
    }
    if ( result.empty() ) {
        // 默认名只用于自动建议，用户仍可在位置步骤手动覆盖。
        result = "New_Project";
    }
    return result;
}

/// @brief 获取侧边栏页签显示文本。
/// @param tab 侧边栏页签。
/// @return 用户界面显示文本。
///
/// 向导只展示创建项目后可直接恢复的工作区页签。Settings 与未知枚举值统一显示为
/// “无”，避免把不支持的状态写入新项目请求。
const char* sidebarTabLabel(SideBarTab tab)
{
    // 返回的指针来自翻译缓存，只在当前渲染调用中消费。
    switch ( tab ) {
    case SideBarTab::FileExplorer:
        return TR("ui.wizard.new_project.sidebar.file").data();
    case SideBarTab::BeatMapExplorer:
        return TR("ui.wizard.new_project.sidebar.beatmap").data();
    case SideBarTab::AudioExplorer:
        return TR("ui.wizard.new_project.sidebar.audio").data();
    case SideBarTab::Search:
        return TR("ui.wizard.new_project.sidebar.search").data();
    case SideBarTab::None:
    case SideBarTab::Settings:
    // Settings 不属于可选初始资源栏，与 None 使用同一个显示语义。
    default: return TR("ui.wizard.new_project.sidebar.none").data();
    }
}

/// @brief 如果路径存在且是目录，则返回可传给文件选择器的 UTF-8 路径。
/// @param path 待检查路径。
/// @return 有效目录的 UTF-8 文本，无效时返回空字符串。
///
/// 该结果只作为选择器初始目录，任何检查失败都应静默退回无默认路径，而不是阻止
/// 用户打开选择器。最终创建位置仍由文件线程独立验证。
std::string existingDirectoryPathToUtf8(const std::filesystem::path& path)
{
    // 空输入不应被 absolute 解读成进程当前目录。
    if ( path.empty() ) return {};

    // 使用 error_code 保证 UI 调用链不依赖文件系统异常机制。
    std::error_code filesystemError;
    const auto absolutePath = std::filesystem::absolute(path, filesystemError);
    if ( filesystemError ) {
        // 无法解析时让调用方继续尝试历史目录或无默认路径。
        return {};
    }

    // 初始位置必须真实存在且是目录，避免原生后端拒绝无效参数。
    if ( !std::filesystem::exists(absolutePath, filesystemError) ||
         filesystemError ||
         !std::filesystem::is_directory(absolutePath, filesystemError) ||
         filesystemError ) {
        return {};
    }
    // weakly_canonical 尽量解析已有前缀，同时允许路径后段的兼容差异。
    auto pickerPath =
        std::filesystem::weakly_canonical(absolutePath, filesystemError);
    if ( filesystemError ) {
        // 规范化失败不否定已经验证过的绝对目录。
        pickerPath = absolutePath;
        filesystemError.clear();
    }
    // 原生后端接收平台偏好的分隔符，再统一编码为 UTF-8。
    pickerPath.make_preferred();
    return Config::pathToUtf8(pickerPath);
}

/// @brief 打开原生父目录选择器，默认路径不被系统接受时自动无默认路径重试。
/// @param outPath 原生文件选择器输出路径。
/// @param defaultPath 原生文件选择器初始目录。
/// @return 原生文件选择器结果。
///
/// 某些原生后端会因默认目录格式或权限返回错误，但不带默认目录仍能工作。函数只对
/// 这种错误重试一次，取消选择不会触发第二个弹窗。
nfdresult_t pickNativeParentFolder(nfdu8char_t**      outPath,
                                   const std::string& defaultPath)
{
    // 空字符串必须传 nullptr，避免后端把它解释成非法路径。
    const nfdu8char_t* defaultPathPtr =
        defaultPath.empty() ? nullptr : defaultPath.c_str();
    // 第一次优先保留用户最近位置。
    nfdresult_t result = NativeFileDialog::pickFolder(outPath, defaultPathPtr);
    if ( result != NFD_ERROR || defaultPathPtr == nullptr ) {
        // 成功、取消或本来没有默认路径时直接返回原始结果。
        return result;
    }

    // 记录后端错误供诊断，但不把实现细节直接展示给向导用户。
    const char* errorText = NFD_GetError();
    XWARN("Native folder picker rejected default path [{}]: {}",
          defaultPath,
          errorText ? errorText : "");
    if ( outPath && *outPath ) {
        // 后端可能在错误结果中仍分配路径，重试前必须按 NFD 契约释放。
        NFD_FreePathU8(*outPath);
        *outPath = nullptr;
    }
    // 第二次不提供默认目录，让系统选择器使用自身安全起点。
    return NativeFileDialog::pickFolder(outPath, nullptr);
}

}  // namespace

/// @brief 构造新建项目向导并初始化一次完整表单状态。
///
/// 固定视图名用于 UIManager 注册；真正的弹窗标题在 update 中本地化，并通过
/// `###` 后缀保持 ImGui ID 稳定。
NewProjectWizard::NewProjectWizard() : IUIView("NewProjectWizard")
{
    // 构造与每次 open 共用相同默认值来源，避免首次和再次打开行为不同。
    reset();
}

/// @brief 把 UTF-8 文本安全复制到 ImGui 固定字符缓冲。
/// @param buffer 目标缓冲区。
/// @param bufferSize 目标容量，包含结尾空字符。
/// @param value 待复制文本。
///
/// 超长输入按字节截断并始终补零。调用方的缓冲用于 ImGui InputText，因此目标容量
/// 为零时不能写入；这里不负责修复截断处的 UTF-8 边界。
void NewProjectWizard::copyToBuffer(char* buffer, std::size_t bufferSize,
                                    std::string_view value)
{
    // 零容量是合法防御输入，不能写结尾空字符。
    if ( bufferSize == 0 ) return;

    // 为 `\0` 保留一个字节，防止 ImGui 随后越界扫描。
    const std::size_t copySize = std::min(bufferSize - 1, value.size());
    // value 允许不是空字符结尾，按显式长度复制。
    std::memcpy(buffer, value.data(), copySize);
    // 无论是否截断都建立有效 C 字符串。
    buffer[copySize] = '\0';
}

/// @brief 在用户未手动编辑目录名时根据标题刷新自动建议。
///
/// 一旦位置步骤修改过目录名，后续标题编辑不得覆盖用户选择。该锁存状态只在 reset
/// 清除，因此返回前一步修改标题也会保留手动目录名。
void NewProjectWizard::refreshFolderNameFromTitle()
{
    // 手动编辑优先于自动生成规则。
    if ( m_folderNameEdited ) return;
    // 统一通过有界复制写入固定表单缓冲。
    copyToBuffer(m_folderNameBuf,
                 sizeof(m_folderNameBuf),
                 makeFolderNameFromTitle(m_titleBuf));
}

/// @brief 在文件线程检查父目录和项目入口，不创建任何目录。
/// @param parent 已冻结的父目录输入。
/// @param folder 已冻结的文件夹名称。
/// @param revision 请求序号，供 UI 排除已经过期的结果。
/// @return 路径预览和可提交状态，查询失败按不可用处理。
NewProjectWizard::LocationResult NewProjectWizard::probeLocation(
    std::filesystem::path parent, std::string folder, std::uint64_t revision)
{
    namespace fs = std::filesystem;
    // 无效是默认结论。完成所有必要查询前不发布允许创建的状态。
    // 保留请求序号以统一处理无效输入及后台返回。
    LocationResult result{ revision, {}, LocationStatus::Invalid };
    // 半空输入不会把当前目录带入不存在目标的判断。
    if ( parent.empty() || folder.empty() ) return result;

    // 权限或卷错误不能被解释成目录缺失；父目录必须可正常访问。
    // 使用一次文件状态查询判断父路径，避免存在性与目录类型分两次读取。
    // 文件系统错误和普通路径缺失都要在本次请求内结束。
    std::error_code error;
    if ( !fs::is_directory(parent, error) || error ) return result;
    // worker 内执行 absolute，包括相对路径解析所需的当前目录查询。
    // 词法规范只整理路径表示，不由 UI 解析链接或挂载点。
    result.target = fs::absolute(parent / Config::utf8ToPath(folder), error)
                        .lexically_normal();
    if ( error ) return result;

    // 查询目标本身，悬空链接属于既有对象，不能使用创建不存在目录的分支。
    const auto target = fs::symlink_status(result.target, error);
    // Windows 与 POSIX 可能返回 not_found 和错误码的不同组合。
    // 只对明确不存在的目标容许创建，访问拒绝及循环链接均无此权限。
    if ( target.type() == fs::file_type::not_found &&
         (!error || error == std::errc::no_such_file_or_directory) ) {
        result.status = LocationStatus::Available;
        return result;
    }
    // 目标存在时只允许目录，包括已解析到普通目录的入口。
    // 悬空链接通过这里返回无效，不能写入其指向的位置。
    if ( error || !fs::is_directory(result.target, error) || error )
        return result;

    // 同一个路径可能在后台被外部进程创建项目，最后确认必须重新执行这里。
    // 使用与存储层相同的入口；状态未知或非文件的占位也不能被覆盖。
    for ( const auto& path :
          { Logic::ProjectStorage::manifestPath(result.target),
            Logic::ProjectStorage::legacyProjectFilePath(result.target) } ) {
        // 探测项目文件本身，悬空链接也属于已占用的入口。
        // 两种布局都必须检查，只有第一项明确不存在时才检查后一项。
        const auto entry = fs::symlink_status(path, error);
        if ( entry.type() == fs::file_type::not_found &&
             (!error || error == std::errc::no_such_file_or_directory) )
            continue;
        // 查询失败的结论仍是 Invalid，不把权限问题显示成可覆盖项目。
        if ( !error && fs::exists(entry) )
            result.status = LocationStatus::ExistingProject;
        return result;
    }
    // 已存在普通目录允许复用，但两个项目入口都必须明确缺失。
    result.status = LocationStatus::Available;
    return result;
}

/// @brief 比较输入键，非阻塞消费探测并按需要提交任务。
/// @warning 位置页每帧调用。只检查 future 的就绪状态，不等待文件线程。
void NewProjectWizard::updateLocationValidation()
{
    // 以两份用户输入而非规范路径为键；编辑帧无需触碰正在检查的文件系统。
    // 只有输入真正改变才前进序号，静置帧不再排队。
    if ( m_probedParent != m_parentDirectory ||
         m_probedFolder != m_folderNameBuf ) {
        // 挂起的提交只针对用户点击时的名称和目录，编辑会撤销它。
        m_probedParent = m_parentDirectory;
        m_probedFolder = m_folderNameBuf;
        ++m_locationRevision;
        m_confirmLocation = false;
        m_locationResult.reset();
    }
    // future 来自文件池的 packaged_task，不具备 std::async 析构等待语义。
    // 只调用零时长轮询，再由就绪的任务清空槽；旧任务无需占用 UI 锁。
    if ( m_locationJob.valid() && m_locationJob.wait_for(std::chrono::seconds(
                                      0)) == std::future_status::ready ) {
        auto result = m_locationJob.get();
        // 更改目录、关闭或重新打开都可能早于后台返回。
        // 槽释放后的发起阶段读取最新键，快速连续编辑只处理最新值。
        if ( result.revision == m_locationRevision )
            m_locationResult = std::move(result);
    }
    // 无输入时也应消费已完成旧任务，但不能为占位路径发出查询。
    // 已得到错误结果同样缓存，否则慢盘错误会在每次渲染重排。
    if ( m_locationJob.valid() || m_locationResult || m_probedParent.empty() ||
         m_probedFolder.empty() )
        return;
    // 只有当前请求可入队；快速键入期间始终最多一项在途探测。
    // worker 只获得输入副本。关闭弹窗或销毁向导后它仍能安全返回。
    if ( auto* files = Runtime::AppThreadPool::instance().getFileThreadPool() )
        m_locationJob = files->enqueue(
            probeLocation, m_probedParent, m_probedFolder, m_locationRevision);
    // 启动或退出期间线程池可能拒绝任务，保持当前输入不可提交。
    if ( !m_locationJob.valid() )
        m_locationResult =
            LocationResult{ m_locationRevision, {}, LocationStatus::Invalid };
}

/// @brief 判断当前步骤是否允许前进或提交。
/// @return 当前步骤必填项满足时返回 true。
///
/// 项目偏好全部有安全默认值；信息步骤只要求标题，位置步骤执行完整文件系统验证。
/// @note 文件查询异步推进，此处只读取 UI 所属的最近一次完整验证。
bool NewProjectWizard::canAdvance() const
{
    // 每一步只验证本页负责的数据，避免尚未展示的位置页阻止信息页前进。
    switch ( m_currentStep ) {
    case Step::ProjectInfo: return m_titleBuf[0] != '\0';
    case Step::Preferences: return true;
    // 点击确认后必须等待新一轮结果，避免使用初次探测保存旧路径。
    case Step::Location:
        return !m_confirmLocation && m_locationResult &&
               m_locationResult->status == LocationStatus::Available;
    }
    // 防御未来新增未覆盖枚举值。
    return false;
}

/// @brief 绘制当前步骤名称、序号和页头分隔线。
///
/// 枚举到序号的映射集中在此处，确保翻译标签与 `n / 3` 进度始终一致。
void NewProjectWizard::renderStepHeader() const
{
    // 先给出 ProjectInfo 回退，随后按当前枚举覆盖。
    const char* label = TR("ui.wizard.new_project.step.info").data();
    int         index = 0;
    switch ( m_currentStep ) {
    case Step::ProjectInfo:
        label = TR("ui.wizard.new_project.step.info").data();
        index = 0;
        break;
    case Step::Preferences:
        label = TR("ui.wizard.new_project.step.preferences").data();
        index = 1;
        break;
    case Step::Location:
        label = TR("ui.wizard.new_project.step.location").data();
        index = 2;
        break;
    }

    // 页头文本不参与交互，不需要额外稳定 ID。
    ImGui::Text("%s  %d / 3", label, index + 1);
    ImGui::Separator();
}

/// @brief 绘制项目标题、艺术家和谱师信息步骤。
///
/// 标题变化会更新尚未手动编辑的目录名建议；其余元数据只写入本地表单缓冲，直到
/// 最后一步提交才发布创建事件。
void NewProjectWizard::renderProjectInfoStep()
{
    // 分隔标题复用项目设置中的元数据翻译。
    ImGui::SeparatorText(TR("ui.settings.project.info").data());
    // 标题字段是本步骤唯一必填项，也是自动目录名的来源。
    if ( renderLabeledInputText(TR("ui.settings.project.name").data(),
                                "##NewProjectTitle",
                                m_titleBuf,
                                sizeof(m_titleBuf)) ) {
        refreshFolderNameFromTitle();
    }
    // 艺术家和谱师允许保留默认值或由用户覆盖。
    renderLabeledInputText(TR("ui.settings.project.artist").data(),
                           "##NewProjectArtist",
                           m_artistBuf,
                           sizeof(m_artistBuf));
    renderLabeledInputText(TR("ui.settings.project.mapper").data(),
                           "##NewProjectMapper",
                           m_mapperBuf,
                           sizeof(m_mapperBuf));
}

/// @brief 绘制新项目的音符配色和初始侧边栏偏好。
///
/// 空配色 ID 表示继承软件默认，特殊常量表示跟随皮肤，其余字符串对应当前编辑器
/// 配置中的具名方案。只保存标识，不复制配色内容。
/// @param sourceManager 提供当前引导遮罩，可为空。
/// @warning UI 热路径：仅在向导偏好页可见时绘制；候选来自已加载配置。
void NewProjectWizard::renderPreferencesStep(UIManager* sourceManager)
{
    // 页内标题与两组组合框保持视觉层级一致。
    ImGui::SeparatorText(TR("ui.wizard.new_project.preferences").data());

    // 配色列表来自当前软件配置，创建请求只记录用户选中的方案名称。
    auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().colorPalettes;

    // 组合框预览把内部哨兵值转换为可本地化文本。
    std::string previewName;
    if ( m_colorPaletteSchemeName.empty() ) {
        // 空字符串延迟到项目打开时解析软件默认方案。
        previewName = TR("ui.settings.project.note_palette.inherit").data();
    } else if ( m_colorPaletteSchemeName ==
                Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ) {
        // 皮肤默认使用稳定 ID 存储，但界面显示翻译名称。
        previewName = TR("ui.toolbar.note_palette.skin_default_scheme").data();
    } else {
        // 用户方案名称直接展示，便于与设置页保持一致。
        previewName = m_colorPaletteSchemeName;
    }

    ImGui::TextUnformatted(TR("ui.settings.project.note_palette").data());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if ( ::MMM::UI::FeedbackBeginCombo("##NewProjectNotePalette",
                                       previewName.c_str()) ) {
        // 继承项通过清空 ID 表达，不写入某个可能随后改变的软件方案名。
        const bool inheritSelected = m_colorPaletteSchemeName.empty();
        if ( ::MMM::UI::FeedbackSelectable(
                 TR("ui.settings.project.note_palette.inherit").data(),
                 inheritSelected) ) {
            m_colorPaletteSchemeName.clear();
        }
        // 当前项成为键盘导航打开组合框后的默认焦点。
        if ( inheritSelected ) ImGui::SetItemDefaultFocus();

        // 皮肤方案使用保留 ID，避免与用户自定义同名方案混淆。
        const bool skinSelected = m_colorPaletteSchemeName ==
                                  Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
        if ( ::MMM::UI::FeedbackSelectable(
                 TR("ui.toolbar.note_palette.skin_default_scheme").data(),
                 skinSelected) ) {
            m_colorPaletteSchemeName =
                Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
        }
        if ( skinSelected ) ImGui::SetItemDefaultFocus();

        // 具名方案保持配置顺序，避免向导与设置页排序不一致。
        for ( const auto& scheme : paletteConfig.schemes ) {
            const bool selected = m_colorPaletteSchemeName == scheme.name;
            if ( ::MMM::UI::FeedbackSelectable(scheme.name.c_str(),
                                               selected) ) {
                m_colorPaletteSchemeName = scheme.name;
            }
            if ( selected ) ImGui::SetItemDefaultFocus();
        }
        // 资源数量改变时按当帧弹窗范围扩展可交互的表单区域。
        if ( sourceManager ) {
            const ImVec2 popupMin  = ImGui::GetWindowPos();
            const ImVec2 popupSize = ImGui::GetWindowSize();
            sourceManager->walkthroughSpotlight().reportCompanionRegion(
                PREFERENCES_NEXT_TARGET,
                popupMin,
                { popupMin.x + popupSize.x, popupMin.y + popupSize.y },
                ImGui::GetWindowViewport());
        }
        ::MMM::UI::FeedbackEndCombo();
    }

    // 初始侧边栏决定新项目打开后的工作区入口，不影响项目内容。
    ImGui::TextUnformatted(TR("ui.wizard.new_project.initial_sidebar").data());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if ( ::MMM::UI::FeedbackBeginCombo("##NewProjectInitialSidebar",
                                       sidebarTabLabel(m_initialSideBarTab)) ) {
        // Settings 不适合作为新项目入口，None 表示启动时不打开资源栏。
        const SideBarTab tabs[] = { SideBarTab::FileExplorer,
                                    SideBarTab::BeatMapExplorer,
                                    SideBarTab::AudioExplorer,
                                    SideBarTab::Search,
                                    SideBarTab::None };
        for ( SideBarTab tab : tabs ) {
            // 枚举值在提交时转换为持久化工作区名称。
            const bool selected = m_initialSideBarTab == tab;
            if ( ::MMM::UI::FeedbackSelectable(sidebarTabLabel(tab),
                                               selected) ) {
                m_initialSideBarTab = tab;
            }
            if ( selected ) ImGui::SetItemDefaultFocus();
        }
        // 侧边栏列表可能越过 Child 边界，同样使用实时弹窗几何。
        if ( sourceManager ) {
            const ImVec2 popupMin  = ImGui::GetWindowPos();
            const ImVec2 popupSize = ImGui::GetWindowSize();
            sourceManager->walkthroughSpotlight().reportCompanionRegion(
                PREFERENCES_NEXT_TARGET,
                popupMin,
                { popupMin.x + popupSize.x, popupMin.y + popupSize.y },
                ImGui::GetWindowViewport());
        }
        ::MMM::UI::FeedbackEndCombo();
    }
}

/// @brief 绘制目录名、父目录、目标预览及路径错误状态。
///
/// 本页不创建目录。用户输入或选择结果仅更新向导状态，点击创建后由事件消费者执行
/// 持久化，以便项目切换生命周期集中处理。
void NewProjectWizard::renderLocationStep()
{
    // 位置页首先允许用户覆盖由标题生成的目录名。
    ImGui::SeparatorText(TR("ui.wizard.new_project.location").data());

    if ( renderLabeledInputText(TR("ui.wizard.new_project.folder_name").data(),
                                "##NewProjectFolderName",
                                m_folderNameBuf,
                                sizeof(m_folderNameBuf)) ) {
        // 首次手动变化后锁住自动生成，返回信息页改标题也不覆盖。
        m_folderNameEdited = true;
    }

    // 浏览按钮按 DPI 缩放并取整，避免亚像素尺寸造成文本模糊。
    const float buttonWidth = std::floor(
        160.0f * Config::AppConfig::instance().getWindowContentScale());
    if ( ::MMM::UI::FeedbackButton(
             TR("ui.wizard.new_project.select_parent").data(),
             ImVec2(buttonWidth, 0.0f)) ) {
        // 实际打开选择器延迟到页脚绘制完成后处理。
        requestParentFolderPicker();
    }

    if ( !m_locationErrorText.empty() ) {
        // 可恢复的选择器错误使用主题危险色显示，不关闭向导。
        ImGui::TextColored(Utils::UIThemeUtils::getDangerColor(),
                           "%s",
                           m_locationErrorText.c_str());
    }

    // 父目录为空时显示明确占位，非空路径统一转为 UTF-8。
    const std::string parentText =
        m_parentDirectory.empty()
            ? TR("ui.wizard.new_project.parent.none").toString()
            : Config::pathToUtf8(m_parentDirectory);
    ImGui::TextUnformatted(TR("ui.wizard.new_project.parent").data());
    ImGui::Indent();
    ImGui::TextWrapped("%s", parentText.c_str());
    ImGui::Unindent();

    // 目标预览展示规范化后的完整路径，帮助用户在提交前确认落点。
    // 先消费最新输入再显示路径。规范路径未返回时只展示词法组合。
    // 未完成状态不会直接提交，因此预览字串不承担路径验证。
    updateLocationValidation();
    // 路径字符串也可作为等待期间的预览，它不参与提交门禁。
    const auto targetPath =
        m_locationResult
            ? m_locationResult->target
            : (m_parentDirectory.empty() || m_folderNameBuf[0] == '\0'
                   ? std::filesystem::path{}
                   : m_parentDirectory / Config::utf8ToPath(m_folderNameBuf));
    const std::string targetText =
        targetPath.empty() ? std::string("-") : Config::pathToUtf8(targetPath);
    ImGui::TextUnformatted(TR("ui.wizard.new_project.target").data());
    ImGui::Indent();
    ImGui::TextWrapped("%s", targetText.c_str());
    ImGui::Unindent();

    if ( m_locationResult &&
         m_locationResult->status == LocationStatus::ExistingProject ) {
        // 已有项目是明确覆盖风险，使用高优先级错误提示。
        const ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::TextColored(
            dangerCol,
            "%s",
            TR("ui.wizard.new_project.folder_has_project").data());
        // 工作池忙时继续展示位置布局，用户仍能更改名称、返回或取消。
    } else if ( m_locationJob.valid() && !m_locationResult ) {
        ImGui::TextDisabled("正在检查保存位置…");
    } else if ( !canAdvance() ) {
        // 其他无效状态使用弱提示，引导用户补齐或更换位置。
        ImGui::TextDisabled(
            "%s", TR("ui.wizard.new_project.location_required").data());
    }
}

/// @brief 绘制占满当前内容宽度的带标签文本输入框。
/// @param label 输入框上方的可见标签。
/// @param id ImGui 稳定控件 ID。
/// @param buffer 可编辑的空字符结尾缓冲。
/// @param bufferSize 缓冲总容量。
/// @return 本帧文本发生变化时返回 true。
/// @warning UI 热路径：仅提交 ImGui 控件，不得在这里读写文件系统。
bool NewProjectWizard::renderLabeledInputText(const char* label, const char* id,
                                              char*       buffer,
                                              std::size_t bufferSize)
{
    // 可见标签与内部 ID 分离，允许翻译变化而不影响控件状态。
    ImGui::TextUnformatted(label);
    // 负最小宽度让输入框扩展到当前内容区右边缘。
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::InputText(id, buffer, bufferSize);
}

/// @brief 绘制上一步、取消和下一步或创建操作区。
/// @param sourceManager 提供当前演练突出层，可为空。
///
/// 三个按钮共享自适应宽度。文件选择器刚关闭后的短暂帧窗口会禁用所有页脚操作，
/// 防止原生或统一弹窗的确认点击穿透到底层向导按钮。
/// @warning UI 热路径：向导可见时每帧调用；仅更新本地步骤状态或发布显式提交。
void NewProjectWizard::renderFooter(UIManager* sourceManager)
{
    // 页脚与滚动内容使用分隔线明确区分。
    ImGui::Separator();

    // 抑制状态按帧而非时间推进，避免不同刷新率下阻塞 UI 线程。
    const bool  suppressActions = shouldSuppressFooterActionsThisFrame();
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const float spacing   = ImGui::GetStyle().ItemSpacing.x;
    const float available = ImGui::GetContentRegionAvail().x;
    // 优先让三按钮在同一行，窄窗口下仍维持可点击的最小宽度。
    const float buttonWidth = std::floor(
        std::min(150.0f * dpiScale, (available - spacing * 2.0f) / 3.0f));
    const ImVec2 buttonSize{ std::max(96.0f * dpiScale, buttonWidth), 0.0f };
    // 第一步没有上一步，抑制窗口内也不允许步骤变化。
    ImGui::BeginDisabled(suppressActions || m_currentStep == Step::ProjectInfo);
    if ( ::MMM::UI::FeedbackButton(TR("ui.wizard.new_project.back").data(),
                                   buttonSize) ) {
        if ( m_currentStep == Step::Preferences ) {
            // 偏好页返回项目元数据页。
            m_currentStep = Step::ProjectInfo;
        } else if ( m_currentStep == Step::Location ) {
            // 位置页返回偏好页并保留已经填写的路径。
            m_currentStep     = Step::Preferences;
            m_confirmLocation = false;
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    // 取消在正常状态下始终可用，并关闭关联的统一选择器。
    ImGui::BeginDisabled(suppressActions);
    if ( ::MMM::UI::FeedbackButton(TR("ui.wizard.new_beatmap.cancel").data(),
                                   buttonSize) ) {
        close();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    // 前进按钮还受当前步骤有效性约束，最后一步切换为创建语义。
    ImGui::BeginDisabled(suppressActions || !canAdvance());
    const bool isLastStep     = m_currentStep == Step::Location;
    const bool advanceClicked = ::MMM::UI::FeedbackButton(
        isLastStep ? TR("ui.wizard.new_project.create").data()
                   : TR("ui.wizard.new_project.next").data(),
        buttonSize);
    // 三页共用同一视觉按钮，但按当前步骤使用不同的稳定目标身份。
    const std::string_view advanceTarget =
        m_currentStep == Step::ProjectInfo
            ? PROJECT_INFO_NEXT_TARGET
            : (m_currentStep == Step::Preferences ? PREFERENCES_NEXT_TARGET
                                                  : LOCATION_CREATE_TARGET);
    if ( sourceManager ) {
        sourceManager->walkthroughSpotlight().reportLastItem(advanceTarget);
    }
    if ( advanceClicked ) {
        if ( m_currentStep == Step::ProjectInfo ) {
            // 信息验证已经由 canAdvance 完成，进入偏好页。
            m_currentStep = Step::Preferences;
        } else if ( m_currentStep == Step::Preferences ) {
            // 偏好都有默认值，可直接进入位置页。
            m_currentStep = Step::Location;
        } else {
            // 最后一步发布请求，不在 UI 层直接建立项目目录。
            submitCreateRequest();
        }
        if ( sourceManager && !isLastStep )
            // canAdvance 已验证当前页，状态切换或请求提交后才完成引导目标。
            sourceManager->walkthroughSpotlight().completeTarget(advanceTarget);
    }
    ImGui::EndDisabled();

    if ( suppressActions && m_suppressFooterActionFrames > 0 ) {
        // 每个实际渲染帧递减一次，避免无符号下溢。
        --m_suppressFooterActionFrames;
    }
}

/// @brief 请求在本帧主向导内容绘制结束后打开父目录选择器。
///
/// 延迟处理避免在按钮调用栈中打开系统模态，并立即开启点击穿透抑制窗口。
void NewProjectWizard::requestParentFolderPicker()
{
    // bool 合并同一帧的重复请求，只打开一个选择器。
    m_pendingParentFolderPicker = true;
    // 经验帧窗口覆盖不同后端关闭弹窗后残留的鼠标释放事件。
    m_suppressFooterActionFrames = 12;
}

/// @brief 处理一次待打开的父目录选择请求并选择后端。
///
/// 初始目录优先使用当前表单父目录，其次使用编辑器最近选择路径。Native 模式失败
/// 时自动转为统一选择器；用户取消不显示错误，也不改变现有表单值。
/// @warning 仅在显式浏览请求后执行；原生选择器可能阻塞当前 UI
/// 调用直到用户关闭。
void NewProjectWizard::processPendingParentFolderPicker()
{
    // 普通渲染帧不访问文件系统或调用选择器后端。
    if ( !m_pendingParentFolderPicker ) {
        return;
    }

    // 消费请求，确保后端调用不会在后续帧自动重复。
    m_pendingParentFolderPicker = false;
    auto& config = Config::AppConfig::instance().getEditorSettings();
    m_locationErrorText.clear();
    // 当前有效父目录应优先于全局最近路径。
    std::string defaultPath = existingDirectoryPathToUtf8(m_parentDirectory);
    if ( defaultPath.empty() && !config.lastFilePickerPath.empty() ) {
        defaultPath = existingDirectoryPathToUtf8(
            Config::utf8ToPath(config.lastFilePickerPath));
    }

    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 原生模态无法由 ImGui 自动播放反馈，调用前显式触发一次。
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      outPath = nullptr;
        const nfdresult_t result =
            pickNativeParentFolder(&outPath, defaultPath);
        // 系统模态返回后恢复向导打开请求，兼容窗口焦点切换。
        m_isOpen                     = true;
        m_shouldOpen                 = true;
        m_suppressFooterActionFrames = 12;
        if ( result == NFD_OKAY ) {
            // NFD 返回 UTF-8 所有权缓冲，先复制到平台路径再释放。
            m_parentDirectory = Config::utf8ToPath(outPath);
            m_locationErrorText.clear();
            auto editorConfig =
                Logic::EditorEngine::instance().getEditorConfig();
            // 最近路径通过 EditorEngine 设置入口持久化，不直接改写配置文件。
            editorConfig.settings.lastFilePickerPath = outPath;
            Logic::EditorEngine::instance().setEditorConfig(editorConfig);
            NFD_FreePathU8(outPath);
        } else if ( result == NFD_ERROR ) {
            // 后端错误写入日志供诊断，并提供功能等价的统一选择器回退。
            const char* errorText = NFD_GetError();
            XWARN(
                "Native folder picker failed, falling back to unified "
                "picker: {}",
                errorText ? errorText : "");
            m_locationErrorText.clear();
            openUnifiedParentFolderPicker(defaultPath);
        }
        // 成功、取消和回退均已完成本次请求处理。
        return;
    }

    // 配置为统一模式时直接打开 ImGuiFileDialog。
    openUnifiedParentFolderPicker(defaultPath);
}

/// @brief 配置并打开统一的只读目录选择器。
/// @param initialPath 已验证的 UTF-8 初始目录，可为空。
///
/// 对话框只允许单选目录并隐藏文件类型列。若同一 ID 已打开，不重复播放弹窗反馈。
void NewProjectWizard::openUnifiedParentFolderPicker(
    const std::string& initialPath)
{
    // 统一选择器的行为仍取当前编辑器配置，便于未来集中扩展。
    auto& config = Config::AppConfig::instance().getEditorSettings();
    IGFD::FileDialogConfig fdConfig;
    fdConfig.path = initialPath.empty() ? std::string(".") : initialPath;
    // 目录选择只接受一个结果，文件名字段保持只读。
    fdConfig.countSelectionMax = 1;
    fdConfig.flags             = ImGuiFileDialogFlags_Modal |
                                 ImGuiFileDialogFlags_HideColumnType |
                                 ImGuiFileDialogFlags_ReadOnlyFileNameField;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened(PARENT_FOLDER_PICKER_ID);
    // 固定 ID 允许 renderParentFolderPicker 在后续帧继续驱动同一实例。
    ImGuiFileDialog::Instance()->OpenDialog(
        PARENT_FOLDER_PICKER_ID,
        TR("ui.wizard.new_project.select_parent").data(),
        nullptr,
        fdConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened(PARENT_FOLDER_PICKER_ID) ) {
        // 仅从关闭到打开的边沿播放反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 驱动统一父目录选择器并接收确认结果。
/// @param dpiScale 当前窗口 DPI 缩放。
///
/// 选择器确认后优先使用完整选择路径，后端未返回文件路径时退回当前目录。无论确认
/// 或取消都在 Display 完成帧关闭实例。
void NewProjectWizard::renderParentFolderPicker(float dpiScale)
{
    // 作用域统一设置模态圆角、阴影和 DPI 相关样式。
    Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
    if ( ImGuiFileDialog::Instance()->IsOpened(PARENT_FOLDER_PICKER_ID) ) {
        Utils::prepareCenteredModalWindow(
            ImVec2(600.0f * dpiScale, 400.0f * dpiScale));
    }
    if ( ImGuiFileDialog::Instance()->Display(
             PARENT_FOLDER_PICKER_ID,
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings,
             ImVec2(600.0f * dpiScale, 400.0f * dpiScale)) ) {
        // Display 返回 true 表示用户完成确认或取消动作。
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            std::string folderPath =
                ImGuiFileDialog::Instance()->GetFilePathName();
            if ( folderPath.empty() ) {
                // 目录模式在部分后端只填充 CurrentPath。
                folderPath = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            m_parentDirectory = Config::utf8ToPath(folderPath);
            m_locationErrorText.clear();

            auto config = Logic::EditorEngine::instance().getEditorConfig();
            // 保存当前浏览位置，而非尚未创建的目标子目录。
            config.settings.lastFilePickerPath =
                ImGuiFileDialog::Instance()->GetCurrentPath();
            Logic::EditorEngine::instance().setEditorConfig(config);
        }
        // 完成结果消费后释放固定 ID，允许下一次浏览重新打开。
        ImGuiFileDialog::Instance()->Close();
    }
}

/// @brief 查询页脚点击穿透抑制窗口是否仍生效。
/// @return 剩余抑制帧数大于零时返回 true。
bool NewProjectWizard::shouldSuppressFooterActionsThisFrame() const
{
    return m_suppressFooterActionFrames > 0;
}

/// @brief 重新检查用户确认的路径，复核期间不阻塞当前帧。
void NewProjectWizard::submitCreateRequest()
{
    // 初次结果供调节按钮状态，确认后必须再读一次，防止外部随后创建工程。
    // 编号先递增再复用同一任务入口；同步创建始终由项目所有者处理。
    if ( !canAdvance() ) return;
    ++m_locationRevision;
    // 清空已知可用结果，确保再次激活确认按钮不会使用上轮探测。
    m_locationResult.reset();
    m_confirmLocation = true;
    updateLocationValidation();
}

/// @brief 发布通过最新复核的创建请求并结束向导。
/// @pre 结果与当前请求匹配且允许创建，逻辑层负责最终文件操作。
/// @details 创建所有者收到完整值，不借用后台任务状态或 ImGui 缓冲。
void NewProjectWizard::publishCreateRequest()
{
    // 所有字段集中复制到一个事件，避免消费者读取向导生命周期内存。
    Event::ProjectCreateRequestedEvent event;
    event.m_origin = m_openOrigin;
    // 发布文件线程确认的规范路径，不在点击同步线程重新解析。
    event.m_projectPath            = m_locationResult->target;
    event.m_title                  = m_titleBuf;
    event.m_artist                 = m_artistBuf;
    event.m_mapper                 = m_mapperBuf;
    event.m_colorPaletteSchemeName = m_colorPaletteSchemeName;
    event.m_sidebarActiveTab =
        SideBarUI::workspaceNameFromTab(m_initialSideBarTab);
    // 发布后立即关闭，项目切换和落盘由订阅者负责。
    Event::EventBus::instance().publish(event);
    close();
}

/// @brief 更新并渲染新建项目向导窗口。
/// @param sourceManager 当前 UI 管理器，保留用于接口一致性。
/// @warning UI
/// 热路径：每帧仅在向导打开时渲染；除用户点击浏览目录触发文件选择器外
/// 禁止加入阻塞操作。
void NewProjectWizard::update(UIManager* sourceManager)
{
    if ( !m_isOpen ) {
        // 关闭状态不构造任何 ImGui 控件，也不处理残留浏览请求。
        return;
    }

    // 弹窗尺寸、滚动条和最小内容高度统一按窗口 DPI 缩放。
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    // 可见标题可本地化，`###` 后的内部 ID 保持布局和弹窗状态稳定。
    const std::string windowTitle =
        TR("ui.wizard.new_project.title").toString() +
        "###NewProjectWizardWindow";
    if ( m_shouldOpen ) {
        // open 只设置意图，真正 OpenPopup 必须在有效 ImGui 帧中执行。
        ::MMM::UI::FeedbackOpenPopup(windowTitle.c_str());
        // 边沿触发一次，后续帧由 BeginPopupModal 维持。
        m_shouldOpen = false;
    }

    // 模态作用域负责统一居中和主题样式。
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    // 向导尺寸固定，但随 DPI 缩放；不允许停靠或折叠破坏步骤布局。
    constexpr ImGuiWindowFlags WINDOW_FLAGS = ImGuiWindowFlags_NoCollapse |
                                              ImGuiWindowFlags_NoResize |
                                              ImGuiWindowFlags_NoDocking;
    const bool                 windowVisible =
        modalScope.begin(windowTitle.c_str(),
                         &m_isOpen,
                         WINDOW_FLAGS,
                         ImVec2(640.0f * dpiScale, 460.0f * dpiScale),
                         false);
    if ( windowVisible ) {
        // 页头位于滚动区外，切页时进度始终可见。
        renderStepHeader();

        // 为页脚按钮和垂直间距预留固定高度，剩余区域允许独立滚动。
        const float footerReserve = ImGui::GetFrameHeightWithSpacing() +
                                    ImGui::GetStyle().ItemSpacing.y * 3.0f;
        const float contentHeight =
            std::max(120.0f * dpiScale,
                     ImGui::GetContentRegionAvail().y - footerReserve);
        {
            Utils::VerticalScrollbarStyleScope verticalScrollbarStyle(dpiScale);
            // 子窗口固定显示滚动条，避免步骤内容变化造成横向宽度跳动。
            ImGui::BeginChild("##NewProjectWizardContent",
                              ImVec2(0.0f, contentHeight),
                              false,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            // 只绘制当前步骤，其他步骤的数据仍保存在成员缓冲中。
            switch ( m_currentStep ) {
            case Step::ProjectInfo: renderProjectInfoStep(); break;
            case Step::Preferences: renderPreferencesStep(sourceManager); break;
            case Step::Location: renderLocationStep(); break;
            }
            // 表单 Child 是当前页可填写的范围，其他页脚按钮继续被遮罩拦截。
            if ( sourceManager ) {
                const std::string_view target =
                    m_currentStep == Step::ProjectInfo
                        ? PROJECT_INFO_NEXT_TARGET
                    : m_currentStep == Step::Preferences
                        ? PREFERENCES_NEXT_TARGET
                        : LOCATION_CREATE_TARGET;
                const ImVec2 childMin  = ImGui::GetWindowPos();
                const ImVec2 childSize = ImGui::GetWindowSize();
                sourceManager->walkthroughSpotlight().reportCompanionRegion(
                    target,
                    childMin,
                    { childMin.x + childSize.x, childMin.y + childSize.y },
                    ImGui::GetWindowViewport());
            }
            ImGui::EndChild();
        }

        // 页脚在滚动区之后保持固定位置。
        renderFooter(sourceManager);
        // 先处理本帧的取消和后退操作。已取消的探测不会产生创建事件。
        // 复查完成后再发布；切页、取消或改名都能在此之前作废请求。
        // Back 或 Cancel 可能已在同一帧消费。只有仍在等待确认的流程可发布。
        if ( m_isOpen && m_confirmLocation && m_locationResult ) {
            m_confirmLocation = false;
            // 失败不退出向导；用户可以看到最新状态并修改输入后重试。
            if ( m_locationResult->status == LocationStatus::Available ) {
                if ( sourceManager )
                    sourceManager->walkthroughSpotlight().completeTarget(
                        LOCATION_CREATE_TARGET);
                // 演练完成与真实请求同步，不在仅排入文件探测时提前完成。
                publishCreateRequest();
            }
        }

        // 浏览请求在触发按钮及页脚全部消费后处理，减少点击穿透风险。
        processPendingParentFolderPicker();
        // 统一选择器拥有自己的跨帧状态，每帧都需要驱动。
        renderParentFolderPicker(dpiScale);
        // BeginPopupModal 成功后必须在同一分支配对 EndPopup。
        ImGui::EndPopup();
    }
}

/// @brief 重新初始化表单并请求下一帧打开向导模态。
/// @param origin 唤出向导的用户入口，提交后随项目创建流程传递。
///
/// 每次打开都丢弃上次未提交内容，使用最新软件设置生成默认谱师和父目录。
void NewProjectWizard::open(Event::ProjectOpenOrigin origin)
{
    // 入口必须先于 opened 事件冻结，后续表单提交不能按当前焦点重新猜测来源。
    m_openOrigin = origin;
    // 可见和 OpenPopup 意图分别记录，以适配 ImGui 的帧式 API。
    m_isOpen     = true;
    m_shouldOpen = true;
    // 默认值必须在首帧绘制前准备完毕。
    reset();

    // 只表示向导已经由对应入口唤出；创建成功仍由逻辑层另发 completed 事件。
    Event::ProjectOpenInteractionEvent event;
    event.m_origin = origin;
    Event::EventBus::instance().publish(event);
}

/// @brief 关闭向导及可能仍打开的统一目录选择器。
///
/// 关闭不发布取消事件，也不修改已存在项目；表单会在下次 open 时重置。
void NewProjectWizard::close()
{
    // 先停止主向导后续帧渲染。
    m_isOpen = false;
    // 关闭或提交后不让旧入口泄漏到下一次未归因打开。
    m_openOrigin = Event::ProjectOpenOrigin::Unknown;
    if ( ImGuiFileDialog::Instance()->IsOpened(PARENT_FOLDER_PICKER_ID) ) {
        // 关联子模态必须显式关闭，避免下次打开继承旧结果。
        ImGuiFileDialog::Instance()->Close();
    }
    // 通知 ImGui 结束当前向导弹窗栈。
    ImGui::CloseCurrentPopup();
}

/// @brief 从当前软件设置恢复一份干净的新项目表单。
///
/// 不访问或修改项目存储。最近路径若不可取得当前工作目录则保持为空，等待用户在
/// 位置步骤选择；配色默认继承，初始侧边栏默认文件浏览器。
void NewProjectWizard::reset()
{
    // 读取一次设置快照，确保本轮默认值来源一致。
    const auto& settings = Config::AppConfig::instance().getEditorSettings();

    // 新向导总是从项目基本信息页开始，并重新启用目录名自动生成。
    m_currentStep      = Step::ProjectInfo;
    m_folderNameEdited = false;
    copyToBuffer(m_titleBuf,
                 sizeof(m_titleBuf),
                 TR("ui.wizard.new_project.default_title").data());
    copyToBuffer(m_artistBuf, sizeof(m_artistBuf), "Unknown");
    copyToBuffer(
        m_mapperBuf,
        sizeof(m_mapperBuf),
        settings.defaultCreator.empty() ? "Unknown" : settings.defaultCreator);
    // 标题、艺术家、谱师准备完成后派生初始目录名。
    refreshFolderNameFromTitle();
    // 清除上一轮可恢复错误和弹窗防穿透状态。
    m_locationErrorText.clear();
    // 重复打开可能来自相同目录和标题，序号仍必须让上一轮的任务结果失效。
    ++m_locationRevision;
    // 向导重新打开时，旧文件任务可能已在相同路径完成，本轮不能接管其结论。
    m_locationResult.reset();
    m_confirmLocation            = false;
    m_suppressFooterActionFrames = 0;
    m_pendingParentFolderPicker  = false;

    // 空方案表示继承当前软件默认；文件浏览器是最通用的新项目入口。
    m_colorPaletteSchemeName.clear();
    m_initialSideBarTab = SideBarTab::FileExplorer;

    if ( !settings.lastFilePickerPath.empty() ) {
        // 最近路径保留平台无关 UTF-8 存储，在使用时转换为 filesystem::path。
        m_parentDirectory = Config::utf8ToPath(settings.lastFilePickerPath);
    } else {
        // 没有历史路径时尝试进程当前目录，失败则要求用户显式选择。
        std::error_code filesystemError;
        m_parentDirectory = std::filesystem::current_path(filesystemError);
        if ( filesystemError ) {
            m_parentDirectory.clear();
        }
    }
}

}  // namespace MMM::UI
