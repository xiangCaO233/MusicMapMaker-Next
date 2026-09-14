#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "common/AudioInfoUtils.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/logic/BeatmapCreateInteractionEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/SessionUtils.h"
#include "mmm/project/Project.h"
#include "mmm/timing/BpmNormalization.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/ProjectResourceImport.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <initializer_list>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief BPM 测量工具窗口在 UIManager 中的稳定视图名。
///
/// 创建、查找和解除回调必须共用该名称，避免同名替换后访问陈旧观察指针。
/// 名称只作为内部注册键，不参与本地化显示。
/// UIManager 保持该视图的唯一所有权。
constexpr const char* BPM_MEASUREMENT_TOOL_VIEW_NAME = "BpmMeasurementTool";

/// @brief 判断新谱面背景资源是否为已支持的视频容器。
/// @param path 待判断资源路径。
/// @return 视频扩展名返回 true。
///
/// 这里只按扩展名决定元数据 cover_type，实际容器解码能力仍由媒体加载链验证。
bool isVideoBackgroundPath(const std::filesystem::path& path)
{
    // 路径扩展名先转换为 UTF-8，再统一为小写进行平台无关比较。
    std::string extension = Config::pathToUtf8(path.extension());
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    // 白名单与资源选择器允许的视频后缀保持一致。
    return extension == ".mp4" || extension == ".avi" || extension == ".mkv" ||
           extension == ".webm" || extension == ".mov" || extension == ".flv" ||
           extension == ".m4v";
}

/// @brief 将字符串安全写入固定长度输入缓冲区。
/// @param buffer 目标缓冲区。
/// @param bufferSize 缓冲区长度。
/// @param value 待写入文本。
///
/// ImGui InputText 使用固定字符数组，本 helper 始终为末尾预留 NUL，并允许按字节
/// 截断过长 UTF-8 输入；调用方只用于显示和后续编辑，不承担规范化验证。
void copyToBuffer(char* buffer, std::size_t bufferSize, std::string_view value)
{
    // 零长度缓冲区没有可写终止符位置。
    if ( bufferSize == 0 ) return;

    // 最大复制长度比容量小一，确保下面的终止符写入有效。
    const auto copySize = std::min(bufferSize - 1, value.size());
    // string_view 不保证 NUL 结尾，因此按明确字节数复制。
    std::memcpy(buffer, value.data(), copySize);
    // 无论源字符串是否被截断都建立合法 C 字符串。
    buffer[copySize] = '\0';
}

/// @brief 判断两个项目内路径是否指向相同资源。
/// @param lhs 左侧路径。
/// @param rhs 右侧路径。
/// @return UTF-8 字符串或文件名匹配时返回 true。
///
/// 项目元数据可能保存完整相对路径，也可能只保留旧版本文件名；先尝试精确路径，
/// 再以文件名兼容历史项目。该回退不用于安全校验，只用于绑定已有音轨记录。
bool resourcePathMatches(const std::filesystem::path& lhs,
                         const std::filesystem::path& rhs)
{
    // 完整 UTF-8 表示相同可避免同名不同目录被误匹配。
    const auto lhsUtf8 = Config::pathToUtf8(lhs);
    const auto rhsUtf8 = Config::pathToUtf8(rhs);
    if ( lhsUtf8 == rhsUtf8 ) return true;
    // 旧数据没有目录时允许按 basename 找回同一资源。
    return Config::pathToUtf8(lhs.filename()) ==
           Config::pathToUtf8(rhs.filename());
}

/// @brief 扫描项目资源，跳过无权限目录和异常文件状态。
/// @param projectRoot 项目根目录。
/// @param allowedExtensions 允许的扩展名，必须为小写。
/// @return 项目根相对路径列表。
///
/// 扫描使用 error_code 和
/// skip_permission_denied，任何单个目录或文件状态错误都只
/// 跳过对应条目，不中断整个向导。返回路径相对项目根，便于直接写入谱面元数据。
/// 扩展名比较在 UTF-8 字符串上执行 ASCII 小写转换，allowedExtensions 的调用方
/// 必须提供带点号的小写值。结果保持目录迭代器顺序，UI 不依赖排序语义。
/// 目录项状态查询和 relative 转换分别复用并清除同一个 error_code，保证某个损坏
/// 条目不会让之后的正常文件全部被跳过。
/// @warning 低频路径：仅在用户展开封面或背景下拉框时递归扫描项目目录。
std::vector<std::string> collectProjectResources(
    const std::filesystem::path&            projectRoot,
    std::initializer_list<std::string_view> allowedExtensions)
{
    // error_code 版本避免文件系统异常穿过禁异常边界。
    std::vector<std::string>                      resources;
    std::error_code                               filesystemError;
    std::filesystem::recursive_directory_iterator it(
        projectRoot,
        std::filesystem::directory_options::skip_permission_denied,
        filesystemError);
    std::filesystem::recursive_directory_iterator end;
    if ( filesystemError ) return resources;

    for ( ; it != end; it.increment(filesystemError) ) {
        if ( filesystemError ) {
            // 清除当前迭代错误，让 iterator 尝试继续后续可访问节点。
            filesystemError.clear();
            continue;
        }
        if ( !it->is_regular_file(filesystemError) || filesystemError ) {
            // 目录、符号设备和无法读取状态的条目都不是可选资源。
            filesystemError.clear();
            continue;
        }

        // 扩展名按无符号字符转小写，避免负 char 传给 tolower。
        auto ext = Config::pathToUtf8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        });
        // 白名单由具体音频、封面或背景调用点提供。
        const bool accepted = std::any_of(
            allowedExtensions.begin(),
            allowedExtensions.end(),
            [&](std::string_view allowed) { return ext == allowed; });
        if ( !accepted ) continue;

        // 只把项目内部相对路径暴露给 UI 和元数据。
        auto rel =
            std::filesystem::relative(it->path(), projectRoot, filesystemError);
        if ( filesystemError ) {
            // 无法建立相对路径的条目不能安全持久化。
            filesystemError.clear();
            continue;
        }
        // 保留目录层级，避免项目内同名资源冲突。
        resources.push_back(Config::pathToUtf8(rel));
    }
    return resources;
}

/// @brief 判断相对路径是否位于项目根内。
/// @param path 待检查路径。
/// @return 路径没有越出根目录时返回 true。
///
/// 词法规范化后要求路径为非空相对路径，且首个有效分量不能是父目录。此检查用于
/// 判断 filesystem::relative 结果是否仍位于项目树内，不解析符号链接。
bool isRelativePathInsideRoot(const std::filesystem::path& path)
{
    // 空路径和绝对路径都不能作为项目内元数据值。
    if ( path.empty() || path.is_absolute() ) return false;

    // lexically_normal 合并普通的点号与父目录组合。
    const auto normalized = path.lexically_normal();
    for ( const auto& part : normalized ) {
        // 规范化后残留父目录意味着路径会越出根边界。
        if ( part == ".." ) return false;
        // 单点分量不代表实际资源，继续寻找首个有效名称。
        if ( part == "." ) continue;
        // 找到普通首分量即可确认路径从根内开始。
        return true;
    }
    return false;
}

/// @brief 若路径以项目文件夹名开头，则剥掉该多余前缀。
/// @param projectRoot 项目根目录。
/// @param path 待修正的相对路径。
/// @return 可剥离时返回剥离后的路径，否则返回空。
///
/// 某些导入路径会额外携带项目根文件夹名，例如 Project/audio.ogg；当当前根本身
/// 已是 Project 时需去掉重复层。只有第一分量精确匹配时才执行。
/// 返回空同时表示“不满足剥离条件”和“剥离后没有资源分量”；调用方必须继续通过
/// isRelativePathInsideRoot 与 exists 验证，不能仅凭非空结果信任路径。
/// 本函数只做词法分量操作，不访问文件系统，也不解析符号链接。
std::filesystem::path stripProjectFolderPrefix(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( projectRoot.empty() || path.empty() || path.is_absolute() ) {
        // 缺少根信息或输入不是相对路径时没有可安全剥离的前缀。
        return {};
    }

    // 仅接受第一分量等于项目根目录名的明确重复。
    auto iterator = path.begin();
    if ( iterator == path.end() || *iterator != projectRoot.filename() ) {
        return {};
    }

    // 从第二分量重建路径，避免字符串切片破坏平台分隔符。
    std::filesystem::path stripped;
    ++iterator;
    for ( ; iterator != path.end(); ++iterator ) {
        stripped /= *iterator;
    }
    // 返回前再次词法规整点号分量。
    return stripped.lexically_normal();
}

/// @brief 将项目资源路径规整为项目根相对路径。
/// @param project 当前项目。
/// @param path 资源路径。
/// @return 可用于谱面元数据持久化的项目相对路径。
///
/// 相对输入优先修复重复项目文件夹前缀，并且只在修复后文件真实存在时采用；否则
/// 保留原相对值。绝对输入尝试转换到项目根相对路径，根外路径保持绝对表示。
/// 相对输入未强制检查存在性，因为模板元数据可能引用尚待导入或暂时缺失的资源；
/// 只有自动剥离前缀属于推测性修复，因此要求目标真实存在才采用。
/// 本函数不复制文件，导入动作由 importProjectResource 的独立流程负责。
std::filesystem::path normalizeProjectResourcePath(
    const MMM::Project& project, const std::filesystem::path& path)
{
    // 空选择继续以空路径表示未绑定资源。
    if ( path.empty() ) return {};

    // 词法规整项目根，避免后续组合出现多余点号分量。
    const auto projectRoot = project.m_projectRoot.lexically_normal();
    if ( path.is_relative() ) {
        // 先处理压缩包或对话框可能返回的重复根目录名。
        const auto stripped = stripProjectFolderPrefix(projectRoot, path);
        if ( isRelativePathInsideRoot(stripped) ) {
            // 只有修复后目标真实存在才替换用户提供的相对路径。
            std::error_code filesystemError;
            if ( std::filesystem::exists(projectRoot / stripped,
                                         filesystemError) &&
                 !filesystemError ) {
                return stripped.lexically_normal();
            }
        }
        // 普通相对路径按词法规范化后直接用于项目元数据。
        return path.lexically_normal();
    }

    // 绝对路径若位于根内则收窄为可移植项目相对路径。
    std::error_code filesystemError;
    auto            relativePath =
        std::filesystem::relative(path, projectRoot, filesystemError);
    if ( !filesystemError && isRelativePathInsideRoot(relativePath) ) {
        // 项目内资源随项目目录移动仍可解析。
        return relativePath.lexically_normal();
    }
    // 根外资源保留绝对路径，后续导入流程可能再复制进项目。
    return path.lexically_normal();
}
}  // namespace

/// @brief 创建新谱面向导并订阅文件拖放事件。
///
/// 初始状态通过 reset 建立；只在向导打开且未进入手动 BPM 工具时缓存拖放事件，
/// 避免后台窗口截获属于其他视图的文件。
NewBeatmapWizard::NewBeatmapWizard() : IUIView("NewBeatmapWizard")
{
    // 统一初始化输入缓冲、资源选择和模板状态。
    reset();
    // 保存订阅 ID，析构时对称注销捕获 this 的回调。
    m_dropSubscription =
        Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
            [this](const Event::GLFWDropEvent& event) {
                // 事件坐标在 update 中与具体资源控件矩形匹配。
                if ( m_isOpen && !m_manualBpmMeasurementActive )
                    m_pendingDrops.push_back(event);
            });
}

/// @brief 解除文件拖放订阅并结束向导生命周期。
NewBeatmapWizard::~NewBeatmapWizard()
{
    // EventBus 不拥有向导，必须移除捕获成员地址的回调。
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(
        m_dropSubscription);
}

/// @brief 收集当前所有已打开且可用的谱面作为复制模板。
/// @return 按 EditorEngine 会话顺序排列的模板候选快照。
///
/// 会话互斥锁覆盖条目复制和 Beatmap shared_ptr 获取，返回后候选以共享所有权保证
/// 模板读取生命周期。Logo 占位会话和未装载谱面的会话不参与选择。
/// 候选同时保存列表显示名、内部名、源 map_path、会话索引和 cameraId；实际重新
/// 定位以 cameraId 为准，会话索引仅作为当时顺序信息，不作为持久化身份。
/// 返回 vector 离开锁后可安全绘制，但源会话内容可能继续变化，因此提交前还会
/// 再次取得锁并同步模板 Beatmap。
std::vector<NewBeatmapWizard::OpenTemplateOption>
NewBeatmapWizard::collectOpenTemplateOptions() const
{
    // 候选在锁内建立，离开函数后不持有 SessionEntry 引用。
    std::vector<OpenTemplateOption> options;

    // 与会话增删和同步共用递归互斥锁。
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> lock(engine.getSessionMutex());
    auto                                  entries = engine.getSessionEntries();
    // 按会话数预留上界容量，减少 push_back 重分配。
    options.reserve(entries.size());

    for ( int32_t index = 0; index < static_cast<int32_t>(entries.size());
          ++index ) {
        // Logo 画布没有实际谱面，空会话也无法提供模板。
        const auto& entry = entries[static_cast<std::size_t>(index)];
        if ( entry.isLogoPlaceholder || !entry.session ) continue;

        // currentBeatmap 是模板复制所需的完整源对象。
        const auto& ctx = entry.session->getContext();
        if ( !ctx.currentBeatmap ) continue;

        // UI 显示名与内部谱面名称分离，保留用户可辨识标签。
        const auto&        meta = ctx.currentBeatmap->m_baseMapMetadata;
        OpenTemplateOption option;
        // cameraId 是会话存在期间重新定位并同步模板的稳定键。
        option.sessionIndex = index;
        option.cameraId     = entry.cameraId;
        option.displayName =
            entry.displayName.empty() ? meta.name : entry.displayName;
        option.internalName = meta.name;
        option.mapPath      = meta.map_path;
        // shared_ptr 延长源谱面生命周期直到提交或重新同步。
        option.beatmap = ctx.currentBeatmap;
        options.push_back(std::move(option));
    }

    return options;
}

/// @brief 在本帧模板候选中定位当前选中的 cameraId。
/// @param templateOptions 当前打开会话生成的候选列表。
/// @return 匹配项的观察指针，未选择或会话已关闭时返回 nullptr。
///
/// 返回指针只在 templateOptions 未修改期间有效，不存入成员。
const NewBeatmapWizard::OpenTemplateOption*
NewBeatmapWizard::findSelectedTemplate(
    const std::vector<OpenTemplateOption>& templateOptions) const
{
    // 空键明确表示尚未选择模板。
    if ( m_templateCameraId.empty() ) return nullptr;

    // cameraId 比显示名和内部名更能区分同时打开的多个会话。
    auto it = std::find_if(templateOptions.begin(),
                           templateOptions.end(),
                           [&](const OpenTemplateOption& option) {
                               return option.cameraId == m_templateCameraId;
                           });
    // 会话关闭后候选消失，调用方会清理陈旧选择。
    if ( it == templateOptions.end() ) return nullptr;
    return &(*it);
}

/// @brief 选择一个打开谱面作为创建模板并继承资源默认值。
/// @param option 当前候选列表中的模板项。
void NewBeatmapWizard::selectTemplate(const OpenTemplateOption& option)
{
    // 切换创建模式并复制用于后续重新定位和显示的字段。
    m_createMode          = CreateMode::OpenTemplate;
    m_templateCameraId    = option.cameraId;
    m_templateDisplayName = option.displayName;
    // Beatmap 使用共享所有权，防止弹窗操作期间源会话提前释放对象。
    m_templateBeatmap = option.beatmap;

    if ( m_templateBeatmap ) {
        // 模板身份确认后再发布阶段，空候选不能推进模板创建演练。
        publishInteraction(
            Event::BeatmapCreateInteractionStage::TemplateSelected);
        // 只补充资源与尚未填写的文本字段，不覆盖用户已输入内容。
        applyTemplateResourceDefaults(*m_templateBeatmap);
        if ( !m_selectedAudioPath.empty() )
            // 模板自动带入有效音频时同样满足向导准备阶段。
            publishInteraction(
                Event::BeatmapCreateInteractionStage::AudioSelected);
    }
}

/// @brief 从模板谱面提取适合作为新谱面初始值的资源和偏好。
/// @param beatmap 当前选中的源谱面。
///
/// 音频、背景、封面和数值偏好可直接继承；标题与艺术家只填入仍为空的输入框，
/// 避免用户先填写的内容在切换模板时丢失。音频变化会使旧 BPM 测量结果失效。
/// map_length 从元数据毫秒转换为内部秒值；非法或零值不覆盖已有探测时长。轨道数
/// 与 BPM 也只接受正值，避免损坏模板把向导带入不可创建状态。
/// author、version 与内部 name
/// 不从模板预填，继续沿用当前向导用户输入和软件默认值。
void NewBeatmapWizard::applyTemplateResourceDefaults(
    const MMM::BeatMap& beatmap)
{
    // 元数据是本函数唯一读取的模板部分。
    const auto& meta = beatmap.m_baseMapMetadata;

    // song_file_hint 优先表达用户选择，旧项目回退到 main_audio_path。
    const auto& audioHint = meta.song_file_hint.empty() ? meta.main_audio_path
                                                        : meta.song_file_hint;
    if ( !audioHint.empty() ) {
        if ( m_selectedAudioPath != audioHint ) {
            // Timing 与音频波形绑定，切换资源后不能继续复用。
            m_measuredTimings.clear();
        }
        // 同时尝试绑定项目音轨 ID，供 BPM 工具使用。
        m_selectedAudioPath    = audioHint;
        m_selectedAudioTrackId = findAudioTrackIdForPath(audioHint);
    }
    if ( !meta.main_cover_path.empty() ) {
        // 背景可为图片或视频，类型字段在下方一并继承。
        m_selectedCoverPath = meta.main_cover_path;
    }
    m_meta.cover_type      = meta.cover_type;
    m_meta.video_starttime = meta.video_starttime;
    m_meta.bgxoffset       = meta.bgxoffset;
    m_meta.bgyoffset       = meta.bgyoffset;
    if ( !meta.cover_path.empty() ) {
        // 独立封面图只在模板提供时替换选择。
        m_selectedCoverImgPath = meta.cover_path;
    }
    if ( meta.preference_bpm > 0.0 ) {
        // 非正值被视为模板未设置，保留向导默认 BPM。
        m_bpm = meta.preference_bpm;
    }
    if ( meta.track_count > 0 ) {
        // 轨道数必须为正，非法模板值不覆盖默认值。
        m_trackCount = meta.track_count;
    }
    if ( meta.map_length > 0.0 ) {
        // 元数据以毫秒存储，向导内部用秒表示音频长度。
        m_audioDuration = meta.map_length / 1000.0;
    }

    // 以下文本字段只填补空输入，保护用户在选择模板前的编辑。
    if ( m_titleBuf[0] == '\0' && !meta.title.empty() ) {
        copyToBuffer(m_titleBuf, sizeof(m_titleBuf), meta.title);
    }
    if ( m_titleUnicodeBuf[0] == '\0' && !meta.title_unicode.empty() ) {
        copyToBuffer(
            m_titleUnicodeBuf, sizeof(m_titleUnicodeBuf), meta.title_unicode);
    }
    if ( m_artistBuf[0] == '\0' && !meta.artist.empty() ) {
        copyToBuffer(m_artistBuf, sizeof(m_artistBuf), meta.artist);
    }
    if ( m_artistUnicodeBuf[0] == '\0' && !meta.artist_unicode.empty() ) {
        copyToBuffer(m_artistUnicodeBuf,
                     sizeof(m_artistUnicodeBuf),
                     meta.artist_unicode);
    }
}

/// @brief 按资源路径查找项目中对应的主音轨 ID。
/// @param path 谱面元数据或资源选择得到的音频路径。
/// @return 匹配主音轨的稳定 ID，未找到时返回空字符串。
///
/// 只允许主音轨绑定谱面；先兼容完整路径与文件名，再兼容旧项目把文件名直接作为
/// ID 的形式。
std::string NewBeatmapWizard::findAudioTrackIdForPath(
    const std::filesystem::path& path) const
{
    // 没有当前项目或空路径时无法解析资源表。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || path.empty() ) return {};

    for ( const auto& resource : project->m_audioResources ) {
        // 音效等非主音轨不作为新谱面的时间基准。
        if ( resource.m_type != MMM::AudioTrackType::Main ) continue;

        // 同时支持路径比较和历史 filename-ID 约定。
        const auto resourcePath = Config::utf8ToPath(resource.m_path);
        if ( resourcePathMatches(resourcePath, path) ||
             resource.m_id == Config::pathToUtf8(path.filename()) ) {
            return resource.m_id;
        }
    }

    // 空结果会禁用 BPM 测量入口，但仍可显示路径供用户修正。
    return {};
}

/// @brief 提交前从当前源会话同步并刷新模板 Beatmap 指针。
///
/// 向导打开期间模板会话仍可继续编辑；通过 cameraId 在会话锁内重新定位并调用
/// SessionUtils::syncBeatmap，确保创建命令复制的是最新逻辑状态。
void NewBeatmapWizard::syncSelectedTemplateBeatmap()
{
    // 没有选择键时无需取得会话锁。
    if ( m_templateCameraId.empty() ) return;

    // 锁覆盖会话枚举、上下文同步和 shared_ptr 更新。
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> lock(engine.getSessionMutex());
    auto                                  entries = engine.getSessionEntries();
    for ( const auto& entry : entries ) {
        // cameraId 精确匹配当前选择，并要求会话仍存在。
        if ( entry.cameraId != m_templateCameraId || !entry.session ) continue;

        // 把编辑器运行时状态写回 Beatmap 数据模型后再共享给命令。
        auto& ctx = entry.session->getContextMutable();
        Logic::SessionUtils::syncBeatmap(ctx);
        m_templateBeatmap = ctx.currentBeatmap;
        // 找到唯一匹配项即可结束扫描。
        return;
    }
}

/// @brief 把当前输入控件和资源选择同步到待提交的基础元数据。
///
/// 文本缓冲按现值复制，轨道数至少为一，有限正时长由秒转换为毫秒。主音频实体由
/// 项目资源表管理，因此这里只写 song_file_hint；背景扩展名决定 cover_type。
/// main_audio_path 被显式清空，避免模板遗留路径与当前选择冲突；创建命令依据所选
/// 主音轨建立最终绑定。封面图和背景路径保持各自职责，不互相覆盖。
/// 函数只组装内存值，不访问磁盘或直接修改当前项目。
void NewBeatmapWizard::syncMetaFromInputs()
{
    // 固定输入缓冲区均保证 NUL 终止，可直接赋给 std::string 字段。
    m_meta.name           = m_nameBuf;
    m_meta.title          = m_titleBuf;
    m_meta.title_unicode  = m_titleUnicodeBuf;
    m_meta.artist         = m_artistBuf;
    m_meta.artist_unicode = m_artistUnicodeBuf;
    m_meta.author         = m_authorBuf;
    m_meta.version        = m_versionBuf;
    // 轨道数在 UI 侧也钳制，此处再次维护命令边界不变量。
    m_meta.track_count    = std::max(1, m_trackCount);
    m_meta.preference_bpm = m_bpm;
    // 非有限或非正探测结果不进入谱面元数据。
    m_meta.map_length =
        (m_audioDuration > 0.0 && std::isfinite(m_audioDuration))
            ? m_audioDuration * 1000.0
            : 0.0;

    // main_audio_path 由创建命令按项目主音轨解析，避免写入重复来源。
    m_meta.main_audio_path.clear();
    m_meta.song_file_hint  = m_selectedAudioPath;
    m_meta.main_cover_path = m_selectedCoverPath;
    m_meta.cover_path      = m_selectedCoverImgPath;
    if ( !m_selectedCoverPath.empty() ) {
        // 用户当前选择优先于模板遗留的 cover_type。
        m_meta.cover_type = isVideoBackgroundPath(m_selectedCoverPath)
                                ? MMM::CoverType::VIDEO
                                : MMM::CoverType::IMAGE;
    }
}

/// @brief 接收 BPM 测量工具导出的 Timing，并写回新建谱面向导。
/// @param audioTrackId 测量结果所属的音频轨道 ID。
/// @param timings 测量得到的 BPM Timing 列表。
///
/// 回调可能来自自动或手动测量，只接受向导仍打开且轨道与当前选择兼容的结果。
/// 输入中过滤非 BPM、非有限和非正值，统一规范化 BPM 与 beat_length 后稳定排序。
/// 空 audioTrackId 作为兼容调用允许接受；当回调和向导双方都有非空 ID 时必须完全
/// 相等。过滤后没有有效点不会覆盖之前结果，也不会结束手动测量流程。
/// 同时间戳点通过 stable_sort 保持工具输出顺序，创建命令后续可决定如何合并；
/// 向导只保证按时间非递减和每个 BPM 字段内部一致。
void NewBeatmapWizard::applyMeasuredTimingsFromTool(
    const std::string& audioTrackId, const std::vector<::MMM::Timing>& timings)
{
    if ( !m_isOpen || timings.empty() ) {
        // 关闭后的异步结果和空结果不改变下一次向导状态。
        return;
    }
    if ( !audioTrackId.empty() && !m_selectedAudioTrackId.empty() &&
         audioTrackId != m_selectedAudioTrackId ) {
        // 用户已切换音频时拒绝旧轨道的迟到测量结果。
        return;
    }

    // 最坏情况保留全部输入，预留容量避免过滤循环中重分配。
    std::vector<::MMM::Timing> bpmTimings;
    bpmTimings.reserve(timings.size());
    for ( const auto& timing : timings ) {
        // 新谱面的初始 Timing 只继承 BPM 效果。
        if ( timing.m_timingEffect != ::MMM::TimingEffect::BPM ) {
            continue;
        }

        // 兼容工具把 BPM 写在专用字段或通用效果参数中的两种表示。
        const double bpm =
            timing.m_bpm > 0.0 ? timing.m_bpm : timing.m_timingEffectParameter;
        if ( !std::isfinite(bpm) || bpm <= 0.0 ) {
            // 无效数值不能参与节拍长度除法。
            continue;
        }

        // 复制保留时间戳等来源信息，再统一三个 BPM 相关字段。
        auto normalized                    = timing;
        normalized.m_timestamp             = timing.m_timestamp;
        normalized.m_timingEffect          = ::MMM::TimingEffect::BPM;
        normalized.m_timingEffectParameter = ::MMM::normalizeBpmValue(bpm);
        normalized.m_bpm                   = normalized.m_timingEffectParameter;
        // beat_length 始终按规范化后的 BPM 重新计算，保持字段一致。
        normalized.m_beat_length = 60000.0 / normalized.m_bpm;
        bpmTimings.push_back(normalized);
    }

    if ( bpmTimings.empty() ) {
        // 全部被过滤时保留已有向导值，不制造空覆盖。
        return;
    }

    // stable_sort 保留同时间戳输入的原始次序，便于诊断重复点。
    std::stable_sort(bpmTimings.begin(),
                     bpmTimings.end(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.m_timestamp < rhs.m_timestamp;
                     });

    // 首个 Timing 决定向导显示的首选 BPM。
    m_measuredTimings = std::move(bpmTimings);
    m_bpm             = m_measuredTimings.front().m_bpm;
    // 只有有效结果真正回填后才推进节奏测量演练步骤。
    publishInteraction(Event::BeatmapCreateInteractionStage::TimingMeasured);
    if ( m_manualBpmMeasurementActive ) {
        // 手动流程观察此标志后恢复被暂时关闭的模态向导。
        m_manualBpmMeasurementExported = true;
    }
}

/// @brief 开始手动 BPM 测量并暂时收起向导模态弹窗。
/// @param tool 即将显示在前层的 BPM 测量工具。
///
/// ImGui 不适合叠加两个需要交互的模态层，因此手动测量期间关闭当前 popup，但保留
/// m_isOpen 和所有输入状态；工具结束后由 shouldWaitForManualBpmMeasurement
/// 重开。
void NewBeatmapWizard::beginManualBpmMeasurement(BpmMeasurementToolView& tool)
{
    // active 区分临时收起与用户主动关闭向导。
    m_manualBpmMeasurementActive   = true;
    m_manualBpmMeasurementExported = false;
    // 确保 BPM 工具窗口在向导关闭后取得前层焦点。
    tool.requestFocus();
    // 只关闭 ImGui popup，不能调用 close 清理向导数据。
    ImGui::CloseCurrentPopup();
}

/// @brief 在手动测量工具关闭或导出后恢复向导。
/// @param sourceManager 当前 UI 管理器。
/// @return 仍应等待手动测量工具时返回 true。
///
/// 绑定对象、视图注册和 open 状态必须同时成立；工具关闭或成功导出任一条件满足后
/// 都结束等待、请求下一帧重开向导，并先解除导出回调以避免悬空捕获。
bool NewBeatmapWizard::shouldWaitForManualBpmMeasurement(
    UIManager* sourceManager)
{
    if ( !m_manualBpmMeasurementActive ) {
        // 常见路径无需查找 UIManager 视图表。
        return false;
    }

    // 同时校验当前注册对象仍是开始测量时绑定的实例。
    auto* tool = sourceManager ? sourceManager->getView<BpmMeasurementToolView>(
                                     BPM_MEASUREMENT_TOOL_VIEW_NAME)
                               : nullptr;
    const bool toolOpen = tool && tool == m_boundBpmToolView && tool->isOpen();
    if ( toolOpen && !m_manualBpmMeasurementExported ) {
        // 工具仍在交互且没有导出时继续隐藏向导。
        return true;
    }

    // 关闭或导出都完成本次手动测量生命周期。
    m_manualBpmMeasurementActive   = false;
    m_manualBpmMeasurementExported = false;
    // 下一次 update 重新调用 OpenPopup，而不重置用户输入。
    m_shouldOpen = true;
    unbindBpmMeasurementTool();
    return false;
}

/// @brief 从当前绑定的 BPM 测量工具安全解除导出回调。
///
/// 先通过保存的 UIManager 和稳定视图名重新查找对象，再与保存观察指针比较；只有
/// 同一实例仍注册时才清空回调，避免触碰已销毁或同名替换的新窗口。
void NewBeatmapWizard::unbindBpmMeasurementTool()
{
    if ( m_boundBpmToolManager ) {
        // UIManager 拥有工具，观察指针本身不能证明对象仍存活。
        auto* tool = m_boundBpmToolManager->getView<BpmMeasurementToolView>(
            BPM_MEASUREMENT_TOOL_VIEW_NAME);
        if ( tool && tool == m_boundBpmToolView ) {
            // 清除捕获 this 的函数对象后，工具可独立继续或销毁。
            tool->setMeasurementExportCallback({});
        }
    }

    // 两个观察指针必须一起归零，维持绑定状态不变量。
    m_boundBpmToolView    = nullptr;
    m_boundBpmToolManager = nullptr;
}

/// @brief 格式化当前已测量 Timing 的摘要文本。
/// @return 可展示在新建谱面向导中的 Timing 摘要。
///
/// 摘要使用排序后首点的 BPM 与秒制时间，并显示有效 BPM 点总数；无结果时返回空，
/// 调用方不会绘制额外行。
std::string NewBeatmapWizard::formatMeasuredTimingSummary() const
{
    if ( m_measuredTimings.empty() ) {
        // 避免访问 front，也避免生成没有意义的本地化文本。
        return {};
    }

    // 兼容专用 BPM 字段缺失的 Timing 表示。
    const auto&  firstTiming = m_measuredTimings.front();
    const double bpm         = firstTiming.m_bpm > 0.0
                                   ? firstTiming.m_bpm
                                   : firstTiming.m_timingEffectParameter;
    // 时间戳由毫秒转换为用户可读秒数。
    return TR_FMT("ui.wizard.new_beatmap.timing_summary",
                  bpm,
                  firstTiming.m_timestamp / 1000.0,
                  m_measuredTimings.size());
}

/// @brief 检查当前项目是否已存在相同谱面内部名称。
/// @return 非空输入与任一项目谱面名称相同时返回 true。
///
/// 该检查只用于提交前提示，用户仍可在警告弹窗中确认继续创建。
bool NewBeatmapWizard::hasInternalNameConflict() const
{
    // 项目关闭过渡中没有可比较的谱面列表。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return false;

    // 空内部名由其他创建校验处理，这里不制造重复提示。
    std::string name(m_nameBuf);
    if ( name.empty() ) return false;

    // 使用精确大小写比较，与项目 m_name 身份规则保持一致。
    return std::any_of(project->m_beatmaps.begin(),
                       project->m_beatmaps.end(),
                       [&](const auto& entry) { return entry.m_name == name; });
}

/// @brief 汇总向导状态并向编辑器命令队列提交创建谱面请求。
///
/// 空白模式只携带基础元数据与测量 Timing；模板模式在提交前同步源会话，并附带
/// 模板共享对象和复制选项。命令入队后关闭向导，实际项目修改由逻辑线程执行。
/// 提交前置条件由 update 控件保证；重复名称警告确认后也复用此入口，从而不会出现
/// 两套元数据组装逻辑。向导不等待命令完成，项目反馈由其他 UI 服务处理。
/// CmdCreateBeatmap 按值接管元数据与 Timing，模板 Beatmap 通过共享所有权保证
/// 命令消费前仍然有效。
void NewBeatmapWizard::submitCreateRequest()
{
    // 最后一次从控件缓冲同步，保证失焦前输入也进入命令。
    syncMetaFromInputs();

    // 命令使用值语义携带新谱面初始状态。
    Logic::CmdCreateBeatmap cmd;
    cmd.origin         = m_origin;
    cmd.baseMeta       = m_meta;
    cmd.initialTimings = m_measuredTimings;
    if ( m_createMode == CreateMode::OpenTemplate && m_templateBeatmap ) {
        // 源谱面可能在向导打开期间发生编辑，提交前刷新一次。
        syncSelectedTemplateBeatmap();
        cmd.templateBeatmap = m_templateBeatmap;
        cmd.templateOptions = m_templateOptions;
    }

    // pushCommand 转移命令所有权，不在 UI 帧直接修改项目集合。
    Logic::EditorEngine::instance().pushCommand(std::move(cmd));
    // 清理事件绑定和临时交互状态，保留下一次 open 可重新 reset。
    close();
}

/// @brief 绘制当前打开谱面列表并处理模板选择。
/// @param templateOptions 本帧收集的可用模板候选。
///
/// 弹窗使用稳定内部名称，列表项以 cameraId 构造唯一 ID。选择后先继承默认资源，
/// 再关闭选择器并请求打开复制选项弹窗。
void NewBeatmapWizard::renderTemplatePickerPopup(
    const std::vector<OpenTemplateOption>& templateOptions)
{
    if ( ImGui::BeginPopupModal(
             "NewBeatmapTemplatePicker",
             nullptr,
             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize) ) {
        // 标题与列表之间保持明确层次。
        ImGui::TextUnformatted(
            TR("ui.wizard.new_beatmap.template.pick_title").data());
        ImGui::Separator();

        if ( templateOptions.empty() ) {
            // 会话可能在弹窗打开后关闭，空状态必须即时反馈。
            ImGui::TextDisabled(
                "%s", TR("ui.wizard.new_beatmap.template.none_open").data());
        } else {
            // 固定高度子区让大量已打开谱面通过滚动访问。
            Utils::VerticalScrollbarStyleScope verticalScrollbarStyle;
            ImGui::BeginChild(
                "TemplateBeatmapList", ImVec2(460.0f, 220.0f), true);
            for ( const auto& option : templateOptions ) {
                // 可见标签包含显示名和内部名，## 后使用唯一 cameraId。
                std::string label    = fmt::format("{} ({})##{}",
                                                   option.displayName,
                                                   option.internalName,
                                                   option.cameraId);
                bool        selected = option.cameraId == m_templateCameraId;
                if ( ::MMM::UI::FeedbackSelectable(label.c_str(), selected) ) {
                    // 选择行为同时更新模式、源对象和资源默认值。
                    selectTemplate(option);
                    m_shouldOpenTemplateOptions = true;
                    ImGui::CloseCurrentPopup();
                }
                if ( !option.mapPath.empty() && ImGui::IsItemHovered() ) {
                    // 完整源路径只在悬停时显示，避免挤占列表宽度。
                    auto pathText = Config::pathToUtf8(option.mapPath);
                    ImGui::SetTooltip("%s", pathText.c_str());
                }
            }
            ImGui::EndChild();
        }

        // 取消只关闭选择器，不改变已有模板选择。
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.wizard.new_beatmap.cancel").data(),
                 ImVec2(120.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 绘制模板内容复制范围选项弹窗。
///
/// 三个布尔项分别控制元数据、时间线和物件复制；确认与取消都只关闭弹窗，因为
/// checkbox 已直接写入向导状态，最终是否使用由创建命令决定。
/// 弹窗本身不要求模板仍存在；若源会话同时关闭，主向导下一帧会清理陈旧选择并禁用
/// 创建。所有控件使用反馈包装以保持全局悬停和点击音效一致。
void NewBeatmapWizard::renderTemplateOptionsPopup()
{
    if ( ImGui::BeginPopupModal(
             "NewBeatmapTemplateOptions",
             nullptr,
             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize) ) {
        ImGui::TextUnformatted(
            TR("ui.wizard.new_beatmap.template.options_title").data());
        ImGui::Separator();

        // 复制选项保持独立，允许只复用谱面结构的一部分。
        ::MMM::UI::FeedbackCheckbox(
            TR("ui.wizard.new_beatmap.template.copy_metadata").data(),
            &m_templateOptions.copyMetadata);
        ::MMM::UI::FeedbackCheckbox(
            TR("ui.wizard.new_beatmap.template.copy_timelines").data(),
            &m_templateOptions.copyTimelines);
        ::MMM::UI::FeedbackCheckbox(
            TR("ui.wizard.new_beatmap.template.copy_objects").data(),
            &m_templateOptions.copyObjects);

        ImGui::Spacing();
        // OK 明确结束配置但无需额外提交阶段。
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.ok").data(),
                                       ImVec2(120.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        // 取消沿用当前 checkbox 值，与即时编辑控件语义一致。
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.wizard.new_beatmap.cancel").data(),
                 ImVec2(120.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 绘制内部名称重复警告并允许用户确认覆盖意图。
///
/// 该弹窗不阻止创建，只把当前名称本地化显示并提供继续或返回编辑两个选择。
void NewBeatmapWizard::renderDuplicateNameWarningPopup(UIManager* sourceManager)
{
    if ( ImGui::BeginPopupModal(
             "NewBeatmapDuplicateNameWarning",
             nullptr,
             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize) ) {
        // 警告色来自当前主题，保持深浅主题可读性。
        ImGui::TextColored(
            Utils::UIThemeUtils::getWarningColor(),
            "%s",
            TR("ui.wizard.new_beatmap.duplicate_name.title").data());
        ImGui::Separator();

        // 从固定缓冲复制当前值，格式化期间不依赖后续控件修改。
        std::string name(m_nameBuf);
        ImGui::TextWrapped(
            "%s",
            TR_FMT("ui.wizard.new_beatmap.duplicate_name.message", name)
                .c_str());

        ImGui::Spacing();
        // 继续按钮绕过本轮重复检查并直接构造命令。
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.wizard.new_beatmap.duplicate_name.continue").data(),
                 ImVec2(140.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
            submitCreateRequest();
        }
        // 重名确认只在警告弹窗出现时可见，作为创建按钮之后的引导阶段。
        if ( sourceManager )
            sourceManager->walkthroughSpotlight().reportLastItem(
                "new-beatmap.duplicate.continue");
        ImGui::SameLine();
        // 取消只关闭警告，让用户留在主向导修改内部名。
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.wizard.new_beatmap.cancel").data(),
                 ImVec2(120.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 绘制空白创建与打开谱面模板两种来源选择及关联弹窗。
/// @param templateOptions 本帧有效的打开谱面候选。
///
/// 每帧先校验持久化选择仍在候选中；没有候选时强制回退空白模式。弹窗打开请求
/// 使用一次性布尔位，确保 OpenPopup 与对应 BeginPopup 在同一 UI 调用链中完成。
void NewBeatmapWizard::renderTemplateSourceControls(
    const std::vector<OpenTemplateOption>& templateOptions,
    UIManager*                             sourceManager)
{
    if ( templateOptions.empty() && m_createMode == CreateMode::OpenTemplate ) {
        // 最后一个源会话关闭后，模板模式已不可提交。
        m_createMode = CreateMode::Blank;
        // 清除所有与失效源对象关联的身份和共享所有权。
        m_templateCameraId.clear();
        m_templateDisplayName.clear();
        m_templateBeatmap.reset();
    }

    if ( auto* selected = findSelectedTemplate(templateOptions) ) {
        // 每帧刷新 shared_ptr 与显示名，跟随源会话对象更新。
        m_templateBeatmap     = selected->beatmap;
        m_templateDisplayName = selected->displayName;
    } else if ( !m_templateCameraId.empty() ) {
        // 指定 cameraId 已不在打开会话中，废弃陈旧选择。
        m_templateCameraId.clear();
        m_templateDisplayName.clear();
        m_templateBeatmap.reset();
    }

    // 来源选择与基础元数据分区显示。
    ImGui::SeparatorText(TR("ui.wizard.new_beatmap.creation_source").data());

    if ( ::MMM::UI::FeedbackRadioButton(
             TR("ui.wizard.new_beatmap.source.blank").data(),
             m_createMode == CreateMode::Blank) ) {
        // 回到空白模式时保留模板选择，便于用户再次切回。
        m_createMode = CreateMode::Blank;
    }
    ImGui::SameLine();

    if ( templateOptions.empty() ) {
        // 没有打开谱面时禁止进入不可完成的模板模式。
        ImGui::BeginDisabled();
    }
    const bool chooseTemplate = ::MMM::UI::FeedbackRadioButton(
        TR("ui.wizard.new_beatmap.source.template").data(),
        m_createMode == CreateMode::OpenTemplate);
    // 演练目标只记录现有控件矩形，不额外插入占位或改变单页布局。
    // 禁用态仍可定位，引导才能解释为什么必须先打开一个源谱面。
    if ( sourceManager )
        sourceManager->walkthroughSpotlight().reportLastItem(
            "new-beatmap.template.mode");
    if ( chooseTemplate ) {
        // 选择模板模式后立即请求选择器，并默认复制时间线结构。
        m_createMode                    = CreateMode::OpenTemplate;
        m_shouldOpenTemplatePicker      = true;
        m_templateOptions.copyTimelines = true;
    }
    if ( templateOptions.empty() ) {
        // 结束局部禁用并解释模板入口不可用原因。
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "%s", TR("ui.wizard.new_beatmap.template.none_open").data());
    }

    if ( m_createMode == CreateMode::OpenTemplate ) {
        // 当前选择以用户显示名呈现，未选择时显示明确占位。
        std::string selectedText =
            m_templateBeatmap
                ? TR_FMT("ui.wizard.new_beatmap.template.selected",
                         m_templateDisplayName)
                : std::string(
                      TR("ui.wizard.new_beatmap.template.not_selected").data());
        ImGui::TextWrapped("%s", selectedText.c_str());

        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.wizard.new_beatmap.template.pick").data(),
                 ImVec2(150.0f, 0.0f)) ) {
            // 延迟到本函数末尾打开，保持 ImGui popup 调用顺序。
            m_shouldOpenTemplatePicker = true;
        }
        // 选择按钮和单选项分别注册，弹出选择器前始终有可见候选目标。
        // 引导系统按最后可见目标择优，不持有 ImGui Item 生命周期。
        if ( sourceManager )
            sourceManager->walkthroughSpotlight().reportLastItem(
                "new-beatmap.template.pick");
        ImGui::SameLine();
        if ( !m_templateBeatmap ) {
            // 未选源对象时复制范围没有作用。
            ImGui::BeginDisabled();
        }
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.wizard.new_beatmap.template.options").data(),
                 ImVec2(150.0f, 0.0f)) ) {
            // 选项弹窗直接编辑 m_templateOptions。
            m_shouldOpenTemplateOptions = true;
        }
        // 复制选项是模板教程的人工确认步骤，只需提供稳定的视觉锚点。
        // 未选模板时锚点随按钮保持禁用，不能误导为可提交状态。
        if ( sourceManager )
            sourceManager->walkthroughSpotlight().reportLastItem(
                "new-beatmap.template.options");
        if ( !m_templateBeatmap ) {
            ImGui::EndDisabled();
        }
    }

    if ( m_shouldOpenTemplatePicker ) {
        // 消费一次性请求，避免每帧重置弹窗打开状态。
        ::MMM::UI::FeedbackOpenPopup("NewBeatmapTemplatePicker");
        m_shouldOpenTemplatePicker = false;
    }
    // BeginPopupModal 自行判断当前是否真正打开。
    renderTemplatePickerPopup(templateOptions);

    if ( m_shouldOpenTemplateOptions ) {
        // 选择模板后或用户点击选项按钮时打开。
        ::MMM::UI::FeedbackOpenPopup("NewBeatmapTemplateOptions");
        m_shouldOpenTemplateOptions = false;
    }
    renderTemplateOptionsPopup();
}

/// @brief 按目标资源类型打开原生或 ImGui 文件选择器。
/// @param target 音频、封面图或背景资源目标。
///
/// 两种选择器使用相同扩展名白名单和最近目录。原生对话框同步返回并立即导入；
/// ImGuiFileDialog 只在此发出打开请求，后续帧由 renderResourcePicker 消费结果。
/// 音频白名单覆盖项目探针支持的常用格式；背景白名单是封面图片集合与视频集合的
/// 并集，确保进入 importResource 后的类型判定与选择器展示保持一致。
/// 原生 UTF-8 路径由 NFD 分配，成功导入后立即释放；取消不写错误，底层失败保留
/// 诊断文本。内置对话框以稳定 key 延续到后续 UI 帧。
/// @warning 低频交互路径：原生对话框会阻塞 UI，且只由用户点击导入按钮触发。
void NewBeatmapWizard::openResourcePicker(ResourceTarget target)
{
    // 保存目标类型，异步 ImGui 对话框返回时据此分类导入结果。
    m_resourceTarget = target;
    // 新选择开始时移除上一次错误提示。
    m_resourceImportError.clear();
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    // 音频使用独立白名单，封面只允许图片，背景额外允许视频。
    const bool  audio = target == ResourceTarget::Audio;
    const char* title = TR(audio ? "ui.wizard.new_beatmap.import_audio"
                           : target == ResourceTarget::Cover
                               ? "ui.wizard.new_beatmap.import_image"
                               : "ui.wizard.new_beatmap.import_background")
                            .data();
    if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 原生窗口打开前播放统一弹窗反馈。
        PlayPopupOpenFeedback();
        // NFD 通过输出指针返回 UTF-8 路径，成功后必须释放。
        nfdu8char_t*            path = nullptr;
        const nfdu8filteritem_t filter{
            title,
            audio ? "mp3,ogg,wav,flac,opus,aac,m4a"
            : target == ResourceTarget::Background
                ? "png,jpg,jpeg,bmp,mp4,avi,mkv,webm,mov,flv,m4v"
                : "png,jpg,jpeg,bmp"
        };
        const auto result = NativeFileDialog::openFile(
            &path, &filter, 1, settings.lastFilePickerPath.c_str());
        if ( result == NFD_OKAY ) {
            // 导入在路径内存释放前完成 filesystem::path 拷贝。
            importResource(Config::utf8ToPath(path));
            NFD_FreePathU8(path);
        } else if ( result == NFD_ERROR ) {
            // 用户取消不是错误，只有 NFD_ERROR 展示诊断文本。
            m_resourceImportError = NFD_GetError();
        }
        // 原生路径已完成整个交互，不再打开 ImGui 对话框。
        return;
    }
    // 内置选择器限制单选并隐藏与只读导入无关的列和文件名编辑。
    IGFD::FileDialogConfig config;
    config.path              = settings.lastFilePickerPath;
    config.countSelectionMax = 1;
    config.flags             = ImGuiFileDialogFlags_Modal |
                               ImGuiFileDialogFlags_HideColumnType |
                               ImGuiFileDialogFlags_ReadOnlyFileNameField;
    // 稳定 key 供 renderResourcePicker 在后续帧查询同一实例。
    ImGuiFileDialog::Instance()->OpenDialog(
        "NewBeatmapResourcePicker",
        title,
        audio ? ".mp3,.ogg,.wav,.flac,.opus,.aac,.m4a"
        : target == ResourceTarget::Background
            ? ".png,.jpg,.jpeg,.bmp,.mp4,.avi,.mkv,.webm,.mov,.flv,.m4v"
            : ".png,.jpg,.jpeg,.bmp",
        config);
    // 内置弹窗也使用统一打开音效。
    PlayPopupOpenFeedback();
}

/// @brief 驱动内置 ImGui 文件选择器并导入确认路径。
///
/// 原生选择器不会进入该路径。Display 返回交互结束时，确认结果传给统一导入函数，
/// 取消则只关闭对话框；居中作用域保证不同 DPI 下保持固定逻辑尺寸。
/// 对话框 key 与 openResourcePicker 完全一致，避免误消费应用中其他选择器实例；
/// 单选上限保证 GetFilePathName 只代表一个资源，具体类型仍由导入函数校验。
/// @warning UI 热路径：只有内置选择器打开时渲染，不进行目录递归扫描。
void NewBeatmapWizard::renderResourcePicker()
{
    // ImGuiFileDialog 是进程级实例，以稳定 key 区分本向导。
    auto* dialog = ImGuiFileDialog::Instance();
    // 未打开时不创建额外模态窗口。
    if ( !dialog->IsOpened("NewBeatmapResourcePicker") ) return;
    // 居中尺寸由 helper 按当前内容缩放换算。
    Utils::CenteredModalPopupScope scope(
        Config::AppConfig::instance().getWindowContentScale());
    Utils::prepareCenteredModalWindow({ 600, 400 });
    if ( dialog->Display("NewBeatmapResourcePicker",
                         ImGuiWindowFlags_NoCollapse,
                         { 600, 400 }) ) {
        // 只有确认操作才读取路径并导入，取消不产生错误提示。
        if ( dialog->IsOk() )
            importResource(Config::utf8ToPath(dialog->GetFilePathName()));
        // 无论确认或取消，完成后都释放对话框打开状态。
        dialog->Close();
    }
}

/// @brief 校验、复制并绑定用户选择的项目资源。
/// @param path 原生对话框、内置选择器或拖放提供的源路径。
///
/// 先按目标类型检查扩展分类，音频再执行解码探针；随后在会话锁内复制资源到项目。
/// 音频通过 EditorEngine 注册为主音轨并反查生成
/// ID，图片和视频直接更新元数据路径。 所有失败均写入
/// m_resourceImportError，不使用异常。
/// 成功返回后 imported 是项目根相对路径，可直接持久化或与资源表比较。会话锁覆盖
/// 当前项目查询、文件复制、音轨注册和资源反查，避免项目切换造成跨项目绑定。
/// 背景图片在尚无独立封面时可兼作封面；视频永远不会写入图片封面字段。
/// @warning 低频路径：可能探测和复制文件，只由明确的选择或拖放动作触发。
void NewBeatmapWizard::importResource(const std::filesystem::path& path)
{
    // 资源分类 helper 统一处理扩展名大小写和支持列表。
    const auto type = Utils::classifyProjectResource(path);
    // 封面只接受图片，背景接受图片或视频，音频必须是音频。
    const bool compatible =
        m_resourceTarget == ResourceTarget::Audio
            ? type == Utils::ProjectResourceType::Audio
            : type == Utils::ProjectResourceType::Image ||
                  (m_resourceTarget == ResourceTarget::Background &&
                   type == Utils::ProjectResourceType::Video);
    if ( !compatible ) {
        // 类型不匹配时不触碰项目文件系统。
        m_resourceImportError =
            TR("ui.wizard.new_beatmap.drop_wrong_type").toString();
        return;
    }
    // 项目资源表和导入命令需要与会话切换互斥。
    auto&           engine = Logic::EditorEngine::instance();
    std::lock_guard lock(engine.getSessionMutex());
    // 项目可能在文件选择器打开期间被关闭。
    auto* project = engine.getCurrentProject();
    if ( !project ) return;
    if ( m_resourceTarget == ResourceTarget::Audio &&
         !MMM::Utils::AudioInfoUtils::probeAudioInfo(path) ) {
        // 扩展名正确但无法探测的音频不复制进项目。
        m_resourceImportError =
            TR("ui.wizard.new_beatmap.import_failed").toString();
        return;
    }
    // importProjectResource 负责目标命名、复制和项目内相对路径结果。
    const auto imported =
        Utils::importProjectResource(project->m_projectRoot, path);
    if ( !imported ) {
        // 保留底层 error_code 文本供用户定位权限或文件问题。
        m_resourceImportError = imported.error().message();
        return;
    }
    if ( m_resourceTarget == ResourceTarget::Audio ) {
        // 先让逻辑引擎登记复制后的绝对路径为主音轨。
        engine.handleImportAudio(
            { Config::pathToUtf8(project->m_projectRoot / *imported),
              MMM::AudioTrackType::Main });
        // 再按规范化相对路径反查新建资源，取得 BPM 工具需要的稳定 ID。
        const auto resource = std::find_if(
            project->m_audioResources.begin(),
            project->m_audioResources.end(),
            [&](const auto& audio) {
                return audio.m_type == MMM::AudioTrackType::Main &&
                       Config::utf8ToPath(audio.m_path).lexically_normal() ==
                           imported->lexically_normal();
            });
        if ( resource == project->m_audioResources.end() ) {
            // 引擎未生成主音轨时不能继续绑定谱面。
            m_resourceImportError =
                TR("ui.wizard.new_beatmap.no_main_audio").toString();
            return;
        }
        // 选择 ID 后探测元数据并填充标题、艺术家和时长。
        m_selectedAudioTrackId = resource->m_id;
        onAudioSelected(*imported);
    } else if ( m_resourceTarget == ResourceTarget::Cover ) {
        // 封面字段独立于背景资源。
        m_selectedCoverImgPath = *imported;
    } else {
        // 背景路径与媒体类型必须同步更新。
        m_selectedCoverPath = *imported;
        m_meta.cover_type   = isVideoBackgroundPath(*imported)
                                  ? MMM::CoverType::VIDEO
                                  : MMM::CoverType::IMAGE;
        if ( m_meta.cover_type == MMM::CoverType::IMAGE &&
             m_selectedCoverImgPath.empty() )
            // 首张背景图片在未指定封面时也作为默认封面。
            m_selectedCoverImgPath = *imported;
    }
    // 成功完成全部绑定后清除旧错误。
    m_resourceImportError.clear();
}

/// @brief 将落在最近资源控件上的单文件拖放转交统一导入流程。
/// @param target 当前控件代表的资源目标类型。
///
/// GLFW 事件坐标相对主视口，先转换为屏幕坐标再与 ImGui item 矩形比较。一次拖放
/// 必须恰好包含一个路径；处理完当前命中控件后清空整帧队列，避免其他控件重复导入。
/// 命中采用半开屏幕矩形，防止相邻资源控件共享边缘像素时重复接受。允许被 active
/// item 阻挡时仍判定 hover，使用户可以直接把文件拖到下拉选择控件上。
/// @warning UI 热路径：只遍历本帧短暂缓存的 GLFW 拖放事件，不访问文件系统。
void NewBeatmapWizard::handleResourceDrop(ResourceTarget target)
{
    // 没有事件或当前 item 未悬停时留给后续资源控件尝试命中。
    if ( m_pendingDrops.empty() ||
         !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) )
        return;
    // item 矩形使用屏幕坐标，GLFW drop 位置需加主视口原点。
    const auto minimum = ImGui::GetItemRectMin();
    const auto maximum = ImGui::GetItemRectMax();
    const auto origin  = ImGui::GetMainViewport()->Pos;
    for ( const auto& drop : m_pendingDrops ) {
        // 多视口坐标通过主 viewport 偏移转换。
        const float x = origin.x + drop.pos.x;
        const float y = origin.y + drop.pos.y;
        if ( x < minimum.x || x >= maximum.x || y < minimum.y ||
             y >= maximum.y )
            // 不在当前控件区域的事件继续由后续资源控件尝试。
            continue;
        if ( drop.paths.size() != 1U ) {
            // 批量拖入会让目标与错误归属不明确，因此明确拒绝。
            m_resourceImportError =
                TR("ui.wizard.new_beatmap.drop_single_file").toString();
            continue;
        }
        // 命中后记录控件目标并复用选择器导入校验。
        m_resourceTarget = target;
        importResource(Config::utf8ToPath(drop.paths.front()));
    }
    // 当前控件一旦悬停命中就消费本帧队列，防止重复处理。
    m_pendingDrops.clear();
}

/// @brief 驱动新谱面模态向导、资源选择和创建提交的一帧 UI。
/// @param sourceManager 用于查找或注册 BPM 测量工具。
///
/// 页面依次处理基础元数据、节拍偏好、模板来源、音频、封面和背景。文件拖放只在
/// 紧邻的资源控件后命中，自动 BPM 分析期间禁用可能切换音频的入口。最终创建条件
/// 要求已选择音频，模板模式还必须具有有效模板对象。
/// 固定内部窗口 ID 保护翻译切换后的 popup 状态；所有子弹窗都在父 popup 的 Begin
/// 与 End 之间驱动。关闭、手动测量暂停和项目消失分别走独立清理路径。
/// 资源下拉框只在展开时扫描项目目录，普通帧仅枚举内存中的主音轨资源表。
/// 手动 BPM 工具通过关闭当前 popup 暂时接管焦点，自动分析则保持向导可见并展示
/// 进度；两种路径共用轨道 ID 校验和导出回调。切换音频总会废弃旧 Timing。
/// 创建按钮禁用范围只覆盖提交操作，用户仍可取消或修正资源；重名属于可确认警告，
/// 不等同于不可创建条件。
/// 音频下拉与导入按钮注册为同一复合引导区域，避免水平布局变化拆散提示范围。
/// 创建与重名确认互斥可见，使同一步骤的候选目标能按弹窗状态自然切换。
/// 交互阶段只在用户真实到达对应状态时发布，关闭向导不会伪造成功结果。
/// @warning UI
/// 热路径：向导打开时每帧执行；项目资源递归扫描仅在对应下拉框展开时发生。
void NewBeatmapWizard::update(UIManager* sourceManager)
{
    if ( !m_isOpen || shouldWaitForManualBpmMeasurement(sourceManager) ) {
        // 隐藏期间不保留可能在其他窗口产生的拖放事件。
        m_pendingDrops.clear();
        return;
    }

    // 模态尺寸和居中位置统一按当前窗口缩放计算。
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    Utils::CenteredModalPopupScope windowScope(dpiScale);
    constexpr ImGuiWindowFlags     WINDOW_FLAGS =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;
    // 固定内部 ID 使翻译标题变化不丢失 popup 状态。
    const std::string windowTitle =
        TR("ui.wizard.new_beatmap.title").toString() +
        "###NewBeatmapWizardWindow";
    if ( m_shouldOpen ) {
        // open 请求只消费一次，避免每帧强制把已关闭弹窗重新打开。
        ::MMM::UI::FeedbackOpenPopup(windowTitle.c_str());
        m_shouldOpen = false;
        XINFO("NewBeatmapWizard: Opening wizard window...");
    }

    // begin 同时维护外部 m_isOpen，用户关闭窗口会直接更新成员。
    const bool windowVisible =
        windowScope.begin(windowTitle.c_str(),
                          &m_isOpen,
                          WINDOW_FLAGS,
                          ImVec2(680.0f * dpiScale, 820.0f * dpiScale),
                          false);
    if ( !windowVisible ) {
        // popup 被遮挡或关闭时不让坐标陈旧的 drop 延续到下一帧。
        m_pendingDrops.clear();
        if ( !m_isOpen ) {
            // 用户关闭必须解除 BPM 工具中捕获 this 的回调。
            unbindBpmMeasurementTool();
        }
        return;
    }

    // 统一固定缓冲输入调用，标签由翻译键提供稳定可见文本。
    auto DrawInput = [&](const char* label, char* buf, size_t bufSize) {
        ImGui::InputText(label, buf, bufSize);
    };

    // 基础信息到视觉资源共同组成教程的“补充谱面信息”目标区域。
    const ImVec2 detailsAreaMin = ImGui::GetCursorScreenPos();
    // 基础信息字段直接编辑向导缓冲，提交时统一同步到 m_meta。
    ImGui::SeparatorText(TR("ui.settings.beatmap.info").data());
    DrawInput(
        TR("ui.settings.beatmap.name").data(), m_nameBuf, sizeof(m_nameBuf));
    DrawInput(
        TR("ui.settings.beatmap.title").data(), m_titleBuf, sizeof(m_titleBuf));
    DrawInput(TR("ui.settings.beatmap.title_unicode").data(),
              m_titleUnicodeBuf,
              sizeof(m_titleUnicodeBuf));
    DrawInput(TR("ui.settings.beatmap.artist").data(),
              m_artistBuf,
              sizeof(m_artistBuf));
    DrawInput(TR("ui.settings.beatmap.artist_unicode").data(),
              m_artistUnicodeBuf,
              sizeof(m_artistUnicodeBuf));
    DrawInput(TR("ui.settings.beatmap.mapper").data(),
              m_authorBuf,
              sizeof(m_authorBuf));
    DrawInput(TR("ui.settings.beatmap.version").data(),
              m_versionBuf,
              sizeof(m_versionBuf));

    // 偏好区保存双精度 BPM，但 ImGui 控件临时使用 float。
    ImGui::SeparatorText(TR("ui.settings.beatmap.preference").data());
    float bpm = (float)m_bpm;
    if ( ::MMM::UI::FeedbackDragFloat(
             TR("ui.settings.beatmap.bpm").data(),
             &bpm,
             0.1f,
             static_cast<float>(::MMM::MIN_NORMALIZED_BPM),
             static_cast<float>(::MMM::MAX_NORMALIZED_BPM),
             "%.2f") ) {
        // 使用领域 helper 统一限制项目支持的 BPM 范围。
        m_bpm = ::MMM::normalizeBpmValue(bpm);
        if ( m_measuredTimings.size() == 1 ) {
            // 单一测量点允许用户微调，并同步三个派生字段。
            auto& timing                   = m_measuredTimings.front();
            timing.m_timingEffect          = ::MMM::TimingEffect::BPM;
            timing.m_timingEffectParameter = ::MMM::normalizeBpmValue(m_bpm);
            timing.m_bpm                   = timing.m_timingEffectParameter;
            timing.m_beat_length           = 60000.0 / timing.m_bpm;
        } else if ( !m_measuredTimings.empty() ) {
            // 多 BPM 图不能整体按一个控件安全缩放，手动修改时清除结果。
            m_measuredTimings.clear();
        }
    }
    // BPM 控件用于复核半频或倍频误判，不代替实际听音判断。
    if ( sourceManager )
        sourceManager->walkthroughSpotlight().reportLastItem(
            "new-beatmap.timing.bpm");
    if ( !m_measuredTimings.empty() ) {
        // 摘要提醒用户创建命令将携带测量得到的 Timing 点。
        const auto timingSummary = formatMeasuredTimingSummary();
        ImGui::TextDisabled("%s", timingSummary.c_str());
    }

    if ( ImGui::InputInt(TR("ui.settings.beatmap.tracks").data(),
                         &m_trackCount) ) {
        // 谱面至少需要一个逻辑轨道。
        if ( m_trackCount < 1 ) m_trackCount = 1;
    }

    // 文件选择期间项目可能被关闭，因此绘制资源区前再次查询。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        // 无项目时给出错误并保持向导打开，等待生命周期更新。
        m_pendingDrops.clear();
        ImGui::TextColored(Utils::UIThemeUtils::getDangerColor(),
                           "%s",
                           TR("ui.wizard.new_beatmap.no_project").data());
        ImGui::EndPopup();
        return;
    }

    // 模板候选以本帧打开会话为准，关闭源会立即反映。
    auto templateOptions = collectOpenTemplateOptions();
    renderTemplateSourceControls(templateOptions, sourceManager);

    ImGui::SeparatorText(TR("ui.settings.beatmap.resource").data());

    // 音频预览显示项目相对路径，未选择时显示本地化占位。
    std::string audioPreview =
        m_selectedAudioPath.empty()
            ? TR("ui.wizard.new_beatmap.select_audio").data()
            : Config::pathToUtf8(m_selectedAudioPath);
    /// @brief 当前项目是否至少存在一个可绑定到谱面的主音轨。
    const bool hasSelectableMainAudio =
        std::any_of(project->m_audioResources.begin(),
                    project->m_audioResources.end(),
                    [](const auto& resource) {
                        // 音效轨道不能成为谱面的时间基准。
                        return resource.m_type == MMM::AudioTrackType::Main;
                    });

    // 标签宽度在当前字体下测量，保证按钮列对齐且完整显示翻译文本。
    const char* measureBpmLabel =
        TR("ui.wizard.new_beatmap.measure_bpm_manual").data();
    const char* autoBpmLabel =
        TR("ui.wizard.new_beatmap.measure_bpm_auto").data();
    // 已注册 BPM 工具可在向导多次打开期间复用。
    auto* bpmTool = sourceManager->getView<BpmMeasurementToolView>(
        BPM_MEASUREMENT_TOOL_VIEW_NAME);
    // 后台自动分析期间锁定音频选择，防止结果与轨道错配。
    const bool backgroundAutomaticMeasurementActive =
        bpmTool && bpmTool->isBackgroundAutomaticMeasurementActive();
    const char* importAudioLabel =
        TR("ui.wizard.new_beatmap.import_audio").data();
    const char* importCoverLabel =
        TR("ui.wizard.new_beatmap.import_image").data();
    const char* importBackgroundLabel =
        TR("ui.wizard.new_beatmap.import_background").data();
    /// @brief 三行共享最长导入标签的宽度，保持下拉框和按钮列对齐。
    const float importButtonWidth =
        std::max({ ImGui::CalcTextSize(importAudioLabel).x,
                   ImGui::CalcTextSize(importCoverLabel).x,
                   ImGui::CalcTextSize(importBackgroundLabel).x }) +
        ImGui::GetStyle().FramePadding.x * 2.0f;
    const float measureBpmWidth = ImGui::CalcTextSize(measureBpmLabel).x +
                                  ImGui::GetStyle().FramePadding.x * 2.0f;
    const float autoBpmWidth    = ImGui::CalcTextSize(autoBpmLabel).x +
                                  ImGui::GetStyle().FramePadding.x * 2.0f;
    const float comboWidth =
        std::max(1.0f,
                 ImGui::GetContentRegionAvail().x - importButtonWidth -
                     ImGui::GetStyle().ItemSpacing.x);

    // 音频下拉、导入和拖放作为一个整体受自动分析状态约束。
    if ( backgroundAutomaticMeasurementActive ) {
        ImGui::BeginDisabled();
    }
    ImGui::SetNextItemWidth(comboWidth);
    // 音频行以一个复合语义区域覆盖下拉框与导入按钮，布局变化时仍可整体跟随。
    const ImVec2 audioRowMin = ImGui::GetCursorScreenPos();
    if ( ::MMM::UI::FeedbackBeginCombo("##NewBeatmapAudioSelect",
                                       audioPreview.c_str()) ) {
        // 只列出项目资源表中的主音轨。
        for ( const auto& res : project->m_audioResources ) {
            if ( res.m_type != MMM::AudioTrackType::Main ) continue;

            bool        isSelected = (m_selectedAudioTrackId == res.m_id);
            std::string label      = res.m_id + "##" + res.m_path;
            if ( ::MMM::UI::FeedbackSelectable(label.c_str(), isSelected) ) {
                // 先记录稳定 ID，再统一处理路径和音频元数据。
                m_selectedAudioTrackId = res.m_id;
                onAudioSelected(Config::utf8ToPath(res.m_path));
            }
            if ( isSelected ) ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", res.m_path.c_str());
        }
        ::MMM::UI::FeedbackEndCombo();
    }
    ImGui::SameLine();
    // 自动分析未运行时，刚绘制的 combo 区域可接受音频拖放。
    if ( !backgroundAutomaticMeasurementActive )
        handleResourceDrop(ResourceTarget::Audio);
    if ( FeedbackButton(importAudioLabel, ImVec2(importButtonWidth, 0.0f)) )
        openResourcePicker(ResourceTarget::Audio);
    if ( sourceManager )
        sourceManager->walkthroughSpotlight().reportTarget(
            "new-beatmap.audio",
            audioRowMin,
            ImGui::GetItemRectMax(),
            ImGui::GetWindowViewport());
    // 没有稳定主音轨 ID 时 BPM 工具无法解析音频池资源。
    if ( m_selectedAudioTrackId.empty() ) {
        ImGui::BeginDisabled();
    }
    // 两个 BPM 按钮共享工具创建、回调绑定和轨道选择流程。
    auto openBpmTool = [&](bool autoMeasure) {
        auto*      tool    = bpmTool;
        const bool wasOpen = tool && tool->isOpen() &&
                             !tool->isBackgroundAutomaticMeasurementActive();
        if ( !tool ) {
            // 首次使用时创建视图，并在 UIManager 中转移唯一所有权。
            auto toolView = std::make_unique<BpmMeasurementToolView>(
                TR("ui.tools.bpm_measure").data());
            tool    = toolView.get();
            bpmTool = tool;
            sourceManager->registerView(BPM_MEASUREMENT_TOOL_VIEW_NAME,
                                        std::move(toolView));
        }
        if ( tool ) {
            // 先解除旧实例回调，再绑定当前工具与管理器观察指针。
            unbindBpmMeasurementTool();
            m_boundBpmToolView    = tool;
            m_boundBpmToolManager = sourceManager;
            // 导出回调只转交领域过滤函数，不直接修改项目。
            tool->setMeasurementExportCallback(
                [this](const std::string&                audioTrackId,
                       const std::vector<::MMM::Timing>& timings) {
                    applyMeasuredTimingsFromTool(audioTrackId, timings);
                });
            if ( autoMeasure ) {
                // 自动模式允许后台运行，并告知工具窗口之前是否已打开。
                tool->requestAutomaticMeasurement(m_selectedAudioTrackId,
                                                  wasOpen);
            } else {
                // 手动模式打开选定轨道，并临时关闭当前模态向导。
                tool->openWithAudioTrack(m_selectedAudioTrackId);
                beginManualBpmMeasurement(*tool);
            }
            if ( !autoMeasure && !wasOpen ) {
                // 新出现的手动工具窗口播放一次弹出反馈。
                ::MMM::UI::PlayPopupOpenFeedback();
            }
        }
    };
    if ( ::MMM::UI::FeedbackButton(measureBpmLabel,
                                   ImVec2(measureBpmWidth, 0.0f)) ) {
        openBpmTool(false);
    }
    ImGui::SameLine();
    if ( ::MMM::UI::FeedbackButton(autoBpmLabel, ImVec2(autoBpmWidth, 0.0f)) ) {
        openBpmTool(true);
    }
    // 自动测偏按钮是推荐入口；手动工具仍沿用原有并列布局与行为。
    if ( sourceManager )
        sourceManager->walkthroughSpotlight().reportLastItem(
            "new-beatmap.timing.auto");
    if ( m_selectedAudioTrackId.empty() ) {
        ImGui::EndDisabled();
    }
    if ( backgroundAutomaticMeasurementActive ) {
        // 分析期间结束控件禁用，并在原位置展示规范化进度。
        ImGui::EndDisabled();
        const float progress =
            std::clamp(bpmTool->getAutomaticMeasurementProgress(), 0.0f, 1.0f);
        // 百分比文本与进度条使用同一夹取后的值。
        const std::string progressText =
            fmt::format("{} {:.0f}%",
                        TR("ui.tools.bpm_measure.auto_analyzing").data(),
                        progress * 100.0f);
        ImGui::ProgressBar(
            progress, ImVec2(-FLT_MIN, 0.0f), progressText.c_str());
    }
    if ( hasSelectableMainAudio ) {
        // 有资源时仍提示下拉框只展示主音轨的设计约束。
        ImGui::TextDisabled(
            "%s", TR("ui.wizard.new_beatmap.main_audio_only_hint").data());
    } else {
        // 没有主音轨时用主题警告色解释创建入口不可用。
        ImGui::TextColored(Utils::UIThemeUtils::getWarningColor(),
                           "%s",
                           TR("ui.wizard.new_beatmap.no_main_audio").data());
    }

    // 封面只可指向图片，独立于可为视频的背景。
    std::string coverImgPreview =
        m_selectedCoverImgPath.empty()
            ? TR("ui.wizard.new_beatmap.select_cover_img").data()
            : Config::pathToUtf8(m_selectedCoverImgPath);

    ImGui::SetNextItemWidth(comboWidth);
    if ( ::MMM::UI::FeedbackBeginCombo("##NewBeatmapCoverImageSelect",
                                       coverImgPreview.c_str()) ) {
        // 仅在展开下拉时扫描项目图片，避免普通帧执行文件系统遍历。
        std::vector<std::string> resources = collectProjectResources(
            project->m_projectRoot, { ".png", ".jpg", ".jpeg", ".bmp" });

        for ( const auto& resPath : resources ) {
            // 相对路径既作为可见标签也作为选择值。
            bool isSelected = (m_selectedCoverImgPath == resPath);
            if ( ::MMM::UI::FeedbackSelectable(resPath.c_str(), isSelected) ) {
                m_selectedCoverImgPath = resPath;
            }
            if ( isSelected ) ImGui::SetItemDefaultFocus();
        }
        ::MMM::UI::FeedbackEndCombo();
    }

    // 最近绘制的封面 combo 接收单图片文件拖放。
    handleResourceDrop(ResourceTarget::Cover);
    ImGui::SameLine();
    if ( FeedbackButton(importCoverLabel, ImVec2(importButtonWidth, 0.0f)) )
        openResourcePicker(ResourceTarget::Cover);

    // 背景允许图片或视频，预览同样显示项目相对路径。
    std::string coverPreview =
        m_selectedCoverPath.empty()
            ? TR("ui.wizard.new_beatmap.select_cover").data()
            : Config::pathToUtf8(m_selectedCoverPath);

    ImGui::SetNextItemWidth(comboWidth);
    if ( ::MMM::UI::FeedbackBeginCombo("##NewBeatmapBackgroundSelect",
                                       coverPreview.c_str()) ) {
        // 仅在下拉展开时扫描项目中的图片和视频文件。
        std::vector<std::string> resources =
            collectProjectResources(project->m_projectRoot,
                                    { ".png",
                                      ".jpg",
                                      ".jpeg",
                                      ".bmp",
                                      ".mp4",
                                      ".avi",
                                      ".mkv",
                                      ".webm",
                                      ".mov",
                                      ".flv",
                                      ".m4v" });

        for ( const auto& resPath : resources ) {
            // 选中后立即同步背景路径与媒体类型。
            bool isSelected = (m_selectedCoverPath == resPath);
            if ( ::MMM::UI::FeedbackSelectable(resPath.c_str(), isSelected) ) {
                m_selectedCoverPath = resPath;
                m_meta.cover_type =
                    isVideoBackgroundPath(Config::utf8ToPath(resPath))
                        ? MMM::CoverType::VIDEO
                        : MMM::CoverType::IMAGE;

                // 图片背景且封面为空时自动沿用同一资源作为封面。
                auto ext =
                    Config::pathToUtf8(Config::utf8ToPath(resPath).extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if ( ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                     ext == ".bmp" ) {
                    if ( m_selectedCoverImgPath.empty() ) {
                        // 已有显式封面选择永远不被背景切换覆盖。
                        m_selectedCoverImgPath = resPath;
                    }
                }
            }
            if ( isSelected ) ImGui::SetItemDefaultFocus();
        }
        ::MMM::UI::FeedbackEndCombo();
    }

    // 最近绘制的背景 combo 接受图片或视频拖放。
    handleResourceDrop(ResourceTarget::Background);
    ImGui::SameLine();
    if ( FeedbackButton(importBackgroundLabel,
                        ImVec2(importButtonWidth, 0.0f)) )
        openResourcePicker(ResourceTarget::Background);
    // 合并矩形覆盖元数据、封面和背景，表达同一人工复核阶段。
    // 起止坐标均来自现有控件，不增加 Child 或改变滚动内容高度。
    if ( sourceManager )
        sourceManager->walkthroughSpotlight().reportTarget(
            "new-beatmap.details",
            detailsAreaMin,
            ImGui::GetItemRectMax(),
            ImGui::GetWindowViewport());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 模板模式额外要求已选择仍存活的源对象。
    const bool needsTemplateSelection =
        m_createMode == CreateMode::OpenTemplate && !m_templateBeatmap;
    // 音频是两种创建模式共同的最低提交条件。
    const bool canCreate =
        !m_selectedAudioPath.empty() && !needsTemplateSelection;

    if ( !canCreate ) {
        ImGui::BeginDisabled();
    }

    if ( ::MMM::UI::FeedbackButton(TR("ui.wizard.new_beatmap.create").data(),
                                   ImVec2(120, 0)) ) {
        if ( hasInternalNameConflict() ) {
            // 重名只进入确认弹窗，不立即丢弃当前输入。
            ::MMM::UI::FeedbackOpenPopup("NewBeatmapDuplicateNameWarning");
        } else {
            // 无冲突时直接向逻辑队列提交创建命令。
            submitCreateRequest();
        }
    }
    if ( sourceManager )
        sourceManager->walkthroughSpotlight().reportLastItem(
            "new-beatmap.create");

    if ( !canCreate ) {
        // 恢复控件状态并在按钮旁解释第一个缺失条件。
        ImGui::EndDisabled();
        ImGui::SameLine();
        const char* warningText =
            m_selectedAudioPath.empty()
                ? TR("ui.wizard.new_beatmap.audio_not_selected").data()
                : TR("ui.wizard.new_beatmap.template.not_selected").data();
        ImGui::TextColored(
            Utils::UIThemeUtils::getWarningColor(), "%s", warningText);
    }

    // 取消按钮靠右布局，关闭时统一解除 BPM 工具绑定。
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    if ( ::MMM::UI::FeedbackButton(TR("ui.wizard.new_beatmap.cancel").data(),
                                   ImVec2(120, 0)) ) {
        close();
    }

    // 子弹窗必须在父 popup 生命周期内持续调用。
    renderDuplicateNameWarningPopup(sourceManager);
    // 本帧所有三个资源控件均已获得处理机会，丢弃未命中的事件。
    m_pendingDrops.clear();
    // 内置文件选择器也作为向导帧的一部分驱动。
    renderResourcePicker();
    if ( !m_resourceImportError.empty() ) {
        // 错误保留到下一次选择开始或成功导入。
        ImGui::TextWrapped("%s: %s",
                           TR("ui.wizard.new_beatmap.import_failed").data(),
                           m_resourceImportError.c_str());
    }

    if ( !m_isOpen ) {
        // 按钮或窗口关闭状态在 EndPopup 前同步给 ImGui。
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// @brief 以全新默认状态打开新谱面向导。
///
/// m_shouldOpen 将实际 ImGui OpenPopup 延迟到下一次 update；reset
/// 在打开边界清除 上次输入、错误、模板和 BPM 绑定。
/// 调用方无需预先检查当前状态；重复 open 会按新的创建流程重新初始化，但 popup
/// 仍由具有有效 ImGui 帧上下文的 update 创建。
void NewBeatmapWizard::open(Logic::BeatmapCreateOrigin origin)
{
    // 目标状态先设为打开，拖放订阅从此开始接收事件。
    m_isOpen = true;
    // popup 打开请求由 UI 帧消费一次。
    m_shouldOpen = true;
    // 每次显式打开都是独立创建流程，入口在 reset 后写入以免被旧状态覆盖。
    reset();
    m_origin = origin;
    publishInteraction(Event::BeatmapCreateInteractionStage::WizardOpened);
}

/// @brief 发布本轮新建谱面向导已经到达的业务阶段。
/// @param stage 已实际到达的向导或创建阶段。
/// @param beatmapPath 完成阶段的新谱面项目内路径。
/// @note 未知入口仍可发布供其他业务观察，但演练服务会忽略其归因。
void NewBeatmapWizard::publishInteraction(
    Event::BeatmapCreateInteractionStage stage, std::string beatmapPath) const
{
    Event::BeatmapCreateInteractionEvent event;
    event.m_origin = m_origin;
    event.m_stage  = stage;
    // 来源在每次发布时读取，允许用户在同一弹窗内切换创建方式。
    event.m_fromTemplate = m_createMode == CreateMode::OpenTemplate;
    event.m_beatmapPath  = std::move(beatmapPath);
    Event::EventBus::instance().publish(event);
}

/// @brief 关闭向导并解除所有跨视图临时绑定。
///
/// 保留输入缓冲到下一次 open 调用 reset，当前帧先清除拖放和 BPM
/// 回调，确保关闭后 不再接收异步工具结果。
/// 关闭不直接调用 ImGui::CloseCurrentPopup；update 会在当前布局完成后依据
/// m_isOpen 对称结束 popup，避免破坏 ImGui 栈。
void NewBeatmapWizard::close()
{
    // 丢弃尚未匹配到资源控件的文件事件。
    m_pendingDrops.clear();
    // 命令提交已按值复制入口；取消关闭则必须防止旧入口泄漏到下次调用。
    m_origin = Logic::BeatmapCreateOrigin::Unknown;
    // 工具回调捕获 this，关闭边界必须主动解除。
    unbindBpmMeasurementTool();
    // 手动测量的暂停状态不能跨向导生命周期保留。
    m_manualBpmMeasurementActive   = false;
    m_manualBpmMeasurementExported = false;
    m_isOpen                       = false;
}

/// @brief 将全部创建字段恢复为软件默认值和空资源选择。
///
/// 默认作者读取软件设置，BPM 与轨道数使用安全初值；所有资源、模板、弹窗请求、
/// 测量结果和错误状态一并清理。函数不改变 m_isOpen 或 m_shouldOpen。
/// 资源导入目标枚举无需重置，因为只有新的文件选择或控件命中后才会读取。模板复制
/// 选项使用值初始化恢复类型定义的默认策略，而非沿用上一轮用户选择。
void NewBeatmapWizard::reset()
{
    // 清理瞬时输入和上一轮可见错误。
    m_pendingDrops.clear();
    m_resourceImportError.clear();
    // 使用值初始化的 BaseMapMeta 清除所有遗留元数据字段。
    m_meta = MMM::BaseMapMeta();

    // 默认制谱者来自用户设置，空配置时回退明确占位。
    const auto& defaultCreator =
        Config::AppConfig::instance().getEditorSettings().defaultCreator;

    // 新谱面提供常见四轨和 120 BPM 起点。
    m_bpm        = 120.0;
    m_trackCount = 4;
    m_measuredTimings.clear();

    // 固定缓冲统一通过安全 helper 初始化并保证 NUL 结尾。
    copyToBuffer(m_nameBuf, sizeof(m_nameBuf), "New Beatmap");
    copyToBuffer(m_titleBuf, sizeof(m_titleBuf), "");
    copyToBuffer(m_titleUnicodeBuf, sizeof(m_titleUnicodeBuf), "");
    copyToBuffer(m_artistBuf, sizeof(m_artistBuf), "");
    copyToBuffer(m_artistUnicodeBuf, sizeof(m_artistUnicodeBuf), "");
    copyToBuffer(m_authorBuf,
                 sizeof(m_authorBuf),
                 defaultCreator.empty() ? "Unknown" : defaultCreator);
    copyToBuffer(m_versionBuf, sizeof(m_versionBuf), "Easy");

    // 资源选择和探测时长全部从未绑定状态开始。
    m_selectedAudioPath.clear();
    m_selectedAudioTrackId.clear();
    m_selectedCoverPath.clear();
    m_selectedCoverImgPath.clear();
    m_audioDuration = 0.0;

    // 创建来源默认空白，模板选择与复制选项重新初始化。
    m_createMode = CreateMode::Blank;
    m_templateCameraId.clear();
    m_templateDisplayName.clear();
    m_templateBeatmap.reset();
    m_templateOptions = {};
    // 所有子弹窗延迟打开请求归零。
    m_shouldOpenTemplatePicker  = false;
    m_shouldOpenTemplateOptions = false;
    // 测量状态归零后安全解除可能仍存在的工具回调。
    m_manualBpmMeasurementActive   = false;
    m_manualBpmMeasurementExported = false;
    unbindBpmMeasurementTool();
}

/// @brief 绑定所选音频并用探测元数据预填谱面信息。
/// @param path 项目相对路径或外部绝对路径。
///
/// 音频变化首先清除旧 Timing；有当前项目时把路径规范化为项目表示，并以绝对路径
/// 调用探针。成功后更新时长、标题、艺术家及去空白内部名，探测失败则只保留选择。
/// 普通与 Unicode 标签初始填入相同文件标签，用户可以随后分别修正；内部名去除
/// 空白但不改写展示标题。没有当前项目时只保存原路径，不尝试解析相对位置。
/// @warning 低频交互路径：音频探测可能执行文件读取，只由用户选择或导入触发。
void NewBeatmapWizard::onAudioSelected(const std::filesystem::path& path)
{
    // 测量 Timing 与具体音频绑定，任何重新选择都使其失效。
    m_measuredTimings.clear();

    // 项目关闭竞态下仍记录原路径，待后续用户修正。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        m_selectedAudioPath = path;
        return;
    }

    // 项目内资源尽量使用可移植相对路径。
    m_selectedAudioPath = normalizeProjectResourcePath(*project, path);
    // 路径已规范化且满足提交条件后才记录准备阶段；探针失败不撤销有效选择。
    publishInteraction(Event::BeatmapCreateInteractionStage::AudioSelected);

    // 音频探针需要项目根解析后的绝对路径。
    auto absPath = project->m_projectRoot / m_selectedAudioPath;
    auto infoOpt = MMM::Utils::AudioInfoUtils::probeAudioInfo(absPath);
    if ( infoOpt ) {
        // 探测结果只在成功分支内读取。
        auto& info = *infoOpt;

        // 向导内部以秒保存，提交时再转为毫秒。
        m_audioDuration = info.duration;

        // 文件标签同时填入普通与 Unicode 字段，用户可继续编辑区分。
        copyToBuffer(m_titleBuf, sizeof(m_titleBuf), info.title);
        copyToBuffer(m_titleUnicodeBuf, sizeof(m_titleUnicodeBuf), info.title);
        copyToBuffer(m_artistBuf, sizeof(m_artistBuf), info.artist);
        copyToBuffer(
            m_artistUnicodeBuf, sizeof(m_artistUnicodeBuf), info.artist);

        // 内部名称默认使用去空白标题，避免生成包含空格的常见不便标识。
        std::string safeName = info.title;
        // remove_if 只修改本地副本，不改变展示标题。
        safeName.erase(
            std::remove_if(safeName.begin(), safeName.end(), ::isspace),
            safeName.end());
        if ( !safeName.empty() ) {
            // 空标签不覆盖 reset 提供的默认内部名。
            copyToBuffer(m_nameBuf, sizeof(m_nameBuf), safeName);
        }
    }
}

}  // namespace MMM::UI
