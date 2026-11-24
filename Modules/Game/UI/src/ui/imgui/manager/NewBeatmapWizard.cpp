// 向导提交完整谱面命令，必须显式提供命令载荷和编辑器配置定义。
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "common/AudioInfoUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/logic/BeatmapCreateInteractionEvent.h"
#include "event/project/ProjectEvents.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectCommandService.h"
#include "logic/ProjectController.h"
#include "logic/SessionTypes.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/project/Project.h"
#include "mmm/timing/BpmNormalization.h"
#include "runtime/AppThreadPool.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/utils/MobileFileDialog.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/ProjectMediaTask.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fmt/format.h>
#include <ice/thread/ThreadPool.hpp>
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
/// @param root 工程根目录。
/// @param path 资源路径。
/// @return 可用于谱面元数据持久化的项目相对路径。
///
/// 相对输入优先修复重复项目文件夹前缀，并且只在修复后文件真实存在时采用；否则
/// 保留原相对值。绝对输入尝试转换到项目根相对路径，根外路径保持绝对表示。
/// 相对输入未强制检查存在性，因为模板元数据可能引用尚待导入或暂时缺失的资源；
/// 只有自动剥离前缀属于推测性修复，因此要求目标真实存在才采用。
/// 本函数不复制文件，导入动作由 importProjectResource 的独立流程负责。
std::filesystem::path normalizeProjectResourcePath(
    const std::filesystem::path& root, const std::filesystem::path& path)
{
    // 空选择继续以空路径表示未绑定资源。
    if ( path.empty() ) return {};

    // 词法规整项目根，避免后续组合出现多余点号分量。
    const auto projectRoot = root.lexically_normal();
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

/// @brief 从同一轮递归遍历构建两类下拉框使用的视觉候选。
/// @param root 当前工程的绝对根目录。
/// @param generation 工程实例的代次，同一路径重新打开也会变化。
/// @param revision 本次读取前观察到的目录失效序号。
/// @return 带有原请求身份的图片视频列表，空工程也返回有效结果。
/// @details 封面选择只读取图片子集，背景选择可读取两种资源。无论哪个下拉
/// 先展开，本轮遍历产物可服务两个控件。
/// 文件任务不持有向导和工程对象，即使页面关闭也不访问悬空引用。
/// @details 相对路径保留子目录层级，便于工程迁移及同名素材区分。
/// 仅检查扩展名和常规文件类型，实际媒体内容由原解码流程验证。
/// 路径大小写按文件系统保留。只对扩展名执行 ASCII 小写转换。
/// 背景和封面复用同一次扫描的结果，布尔类型用于选择时决定 cover_type。
/// 遍历使用 error_code。无权访问的目录按选项跳过，其余迭代错误结束扫描；
/// 返回此前收集的候选，异常条目不会传播到 UI。
/// @warning 独立文件线程入口。只访问请求内路径，不读取向导或会话状态。
NewBeatmapWizard::VisualResources NewBeatmapWizard::scanVisualResources(
    std::filesystem::path root, std::uint64_t generation,
    std::uint64_t revision)
{
    VisualResources result{ generation, revision, {} };
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    // 不开启 follow_directory_symlink，避免目录别名导致重复递归或成环。
    const std::filesystem::recursive_directory_iterator end;
    // 迭代错误会终止标准迭代器。逐项状态错误单独处理，保留可访问结果。
    for ( ; !error && it != end; it.increment(error) ) {
        std::error_code fileError;
        if ( !it->is_regular_file(fileError) ) continue;
        // 视频沿用设置背景类型的公共判定，避免扩展名接受规则分叉。
        const bool video = isVideoBackgroundPath(it->path());
        if ( !video ) {
            auto extension = Config::pathToUtf8(it->path().extension());
            std::ranges::transform(
                extension, extension.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            // 仅接受现有封面和背景控件支持的图片类型。
            if ( extension != ".png" && extension != ".jpg" &&
                 extension != ".jpeg" && extension != ".bmp" )
                continue;
        }

        // relative 可能解析符号链接并访问文件系统，必须留在工作线程。
        // 界面继续显示和提交的是项目相对字符串，无需重做转换。
        auto relative = std::filesystem::relative(it->path(), root, fileError);
        // 相对路径不能逃离工程根，即便目录中存在指向外部的符号链接。
        if ( !fileError && !relative.empty() && *relative.begin() != ".." )
            result.entries.push_back({ Config::pathToUtf8(relative), video });
    }
    return result;
}

/// @brief 取出已完成目录快照；按工程代次和目录失效序号验证。
/// @param project 由绘制帧持有的工程快照。
/// @return 当前目录快照，或扫描期间的空值。
/// @details 工程代次区分同一路径关闭重开。目录序号区分同一工程打开期间
/// 的资源改动。只有两者都匹配才能继续使用列表。
/// 结果对象由向导持有，同一帧在两个下拉中按引用读取。
/// @details 同一工程和失效序号最多启动一次扫描，封面和背景共用这份列表。
/// 新目录事件或同路径重开后，仍运行的旧任务不会阻塞当帧绘制；下一轮收到
/// 结果时重新匹配请求身份，旧数据被丢弃。单独跟踪未完成 future 能限制积压，
/// 即使文件服务器无响应也不会在每帧递交新线程任务。
/// @note 只有展开下拉框时会调用。其余画面不轮询文件或提交后台任务。
/// 返回值由向导独占，只在下一次到达此轮询点之前借用。
/// @warning 每个展开帧调用；拒绝阻塞式等待、文件探测与会话锁。
const NewBeatmapWizard::VisualResources* NewBeatmapWizard::readVisualResources(
    const Logic::ProjectUiState& project)
{
    // 事件线程只递增失效序号；文件任务自身只接收本次提交的副本。
    const auto revision =
        m_visualResourceRevision->load(std::memory_order_relaxed);
    const auto isCurrent = [&](const VisualResources& result) {
        return result.projectGeneration == project.m_generation &&
               result.revision == revision;
    };
    // packaged_task future 的析构不会 join。只有明确就绪才允许取结果。
    // 工作线程可能在视图关闭期间完成，按请求身份核验后才能替换当前缓存。
    // 读取已完成的 future 会清空任务槽，使更新后的工程可以开始下一轮扫描。
    if ( m_visualResourcesJob.valid() &&
         m_visualResourcesJob.wait_for(std::chrono::seconds(0)) ==
             std::future_status::ready ) {
        auto result = m_visualResourcesJob.get();
        if ( isCurrent(result) ) m_visualResources = std::move(result);
    }
    // 未变更的静置帧不分配 vector，也不重复复制已解析的相对路径。
    if ( m_visualResources && isCurrent(*m_visualResources) )
        return &*m_visualResources;
    // 目录变化后旧列表立即失效；等待期间禁止选择已被删除的项目路径。
    m_visualResources.reset();
    // 新任务只在没有在途工作时提交，以免项目连续切换占满文件线程池。
    // 文件池按值接收根和序号，不复用 UI 中的 project 引用。
    if ( !m_visualResourcesJob.valid() ) {
        // 移动项只捕获路径和数值，关闭向导后工作线程也能安全收尾。
        auto* const pool =
            Runtime::AppThreadPool::instance().getFileThreadPool();
        if ( pool )
            m_visualResourcesJob = pool->enqueue(scanVisualResources,
                                                 project.m_projectRoot,
                                                 project.m_generation,
                                                 revision);
        // 停机或启动失败时不在后续静置帧反复尝试提交。
        if ( !m_visualResourcesJob.valid() ) {
            m_visualResources.emplace(
                VisualResources{ project.m_generation, revision, {} });
            return &*m_visualResources;
        }
    }
    return nullptr;
}

/// @brief 创建新谱面向导并订阅文件拖放事件。
///
/// 初始状态通过 reset 建立；只在向导打开且未进入手动 BPM 工具时缓存拖放事件，
/// 避免后台窗口截获属于其他视图的文件。
NewBeatmapWizard::NewBeatmapWizard() : IUIView("NewBeatmapWizard")
{
    // 统一初始化输入缓冲、资源选择和模板状态。
    reset();
    // EventBus 可以在取消订阅后执行已复制的回调，故此处只捕获独立序号。
    // 目录刷新涵盖外部写入，项目保存涵盖程序内导入和改名。
    // 多次通知只推进序号，文件轮询合并为下一次展开时的一份工作。
    const auto invalidateDirectory =
        [revision = m_visualResourceRevision](const auto&) {
            revision->fetch_add(1, std::memory_order_relaxed);
        };
    m_visualDirectorySubscription =
        Event::EventBus::instance()
            .subscribe<Event::ProjectDirectoryRefreshedEvent>(
                invalidateDirectory);
    m_visualSaveSubscription =
        Event::EventBus::instance().subscribe<Event::ProjectSavedEvent>(
            invalidateDirectory);
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
    // 文件线程仅拥有路径，关闭时不等待正在进行的扫描。
    Event::EventBus::instance()
        .unsubscribe<Event::ProjectDirectoryRefreshedEvent>(
            m_visualDirectorySubscription);
    Event::EventBus::instance().unsubscribe<Event::ProjectSavedEvent>(
        m_visualSaveSubscription);
    // EventBus 不拥有向导，必须移除捕获成员地址的回调。
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(
        m_dropSubscription);
}

/// @brief 根据稳定相机及载图实例选择模板，并在锁内复制预填字段。
/// @param option 来自发布列表的候选，真实会话可能已关闭或更换谱面。
/// @return 同一载图实例仍存在时为 true。
/// @details 列表可以比实时会话稍旧。显式点击时重找相机，并核对该相机是否
/// 仍绑定列表里的谱面实例。比较所有权控制块可区分同路径打开的两个版本。
/// 复制后立即释放会话锁，资源字段填入草稿及发送演练事件在锁外完成。
/// 已选择项仍以弱引用标识源，关闭源窗口时不会延长整个谱面的生命周期。
/// @warning 只在明确点击时使用会话锁。普通绘制不查询实时 SessionContext。
bool NewBeatmapWizard::selectTemplate(
    const Logic::SessionTemplateOption& option)
{
    MMM::BaseMapMeta meta;
    {
        auto&           engine = Logic::EditorEngine::instance();
        std::lock_guard lock(engine.getSessionMutex());
        const auto      entries = engine.getSessionEntries();
        const auto      source  = std::ranges::find(
            entries, option.cameraId, &Logic::SessionEntry::cameraId);
        if ( source == entries.end() || source->isLogoPlaceholder ||
             !source->session )
            return false;
        const auto& chart = source->session->getContext().currentBeatmap;
        // 相同相机可重新加载其他谱面，所有权身份比地址或文件名更可靠。
        if ( !chart || option.beatmap.owner_before(chart) ||
             chart.owner_before(option.beatmap) )
            return false;
        // 普通选择只读取资源和显示字段，不同步 ECS、不复制谱面对象。
        meta              = chart->m_baseMapMetadata;
        m_templateBeatmap = chart;
    }
    m_createMode          = CreateMode::OpenTemplate;
    m_templateCameraId    = option.cameraId;
    m_templateDisplayName = option.displayName;
    publishInteraction(Event::BeatmapCreateInteractionStage::TemplateSelected);
    // 预填仅读取复制出来的值，不把 mutable BeatMap 借给渲染线程。
    applyTemplateResourceDefaults(meta);
    if ( !m_selectedAudioPath.empty() )
        publishInteraction(Event::BeatmapCreateInteractionStage::AudioSelected);
    return true;
}

/// @brief 从模板谱面提取适合作为新谱面初始值的资源和偏好。
/// @param meta 在选择时复制的源谱面元数据。
///
/// 音频、背景、封面和数值偏好可直接继承；标题与艺术家只填入仍为空的输入框，
/// 避免用户先填写的内容在切换模板时丢失。音频变化会使旧 BPM 测量结果失效。
/// map_length 从元数据毫秒转换为内部秒值；非法或零值不覆盖已有探测时长。轨道数
/// 与 BPM 也只接受正值，避免损坏模板把向导带入不可创建状态。
/// author、version 与内部 name
/// 不从模板预填，继续沿用当前向导用户输入和软件默认值。
void NewBeatmapWizard::applyTemplateResourceDefaults(
    const MMM::BaseMapMeta& meta)
{
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
    if ( m_titleInput.empty() && !meta.title.empty() ) {
        m_titleInput = meta.title;
    }
    if ( m_titleUnicodeInput.empty() && !meta.title_unicode.empty() ) {
        m_titleUnicodeInput = meta.title_unicode;
    }
    if ( m_artistInput.empty() && !meta.artist.empty() ) {
        m_artistInput = meta.artist;
    }
    if ( m_artistUnicodeInput.empty() && !meta.artist_unicode.empty() ) {
        m_artistUnicodeInput = meta.artist_unicode;
    }
    if ( m_albumInput.empty() && !meta.album.empty() ) {
        // 模板专辑只填空栏，保留用户已输入或音频标签给出的值。
        m_albumInput = meta.album;
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
    auto project = Logic::ProjectController::instance().uiSnapshot();
    if ( !project || project->m_generation != m_projectGeneration ||
         path.empty() )
        return {};

    for ( const auto& resource : project->m_audioResources ) {
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

/// @brief 提交前从当前源会话同步并提升模板引用。
/// @return 仍与选择时身份一致的谱面对象，失效时返回空。
/// @details 提交方只在创建操作上提升源谱面的弱引用，确认同一个实例后
/// 刷新逻辑对象的领域数据，与命令发布之间保持会话锁。
/// 常态绘制只借用标题和路径，详情改变不会延长源谱面生命周期。
/// @pre 由创建提交持有递归会话锁，直到同步创建结束，由此封闭两次操作之间的
/// 更新窗口。定位失败不会退回可写但已经离开会话的对象。
/// @warning 低频确认可能同步已修改领域，禁止用于普通模板绘制。
///
/// 向导打开期间模板会话仍可继续编辑；通过 cameraId 在会话锁内重新定位并调用
/// SessionUtils::syncBeatmap，确保创建命令复制的是最新逻辑状态。
std::shared_ptr<MMM::BeatMap> NewBeatmapWizard::syncSelectedTemplateBeatmap()
{
    const auto entries = Logic::EditorEngine::instance().getSessionEntries();
    const auto source  = std::ranges::find(
        entries, m_templateCameraId, &Logic::SessionEntry::cameraId);
    if ( source == entries.end() || source->isLogoPlaceholder ||
         !source->session )
        return {};
    auto& context = source->session->getContextMutable();
    // 关闭或同画布重载后禁止拿仍存活的旧谱面对象提交。
    const auto selected = m_templateBeatmap.lock();
    if ( !selected || context.currentBeatmap != selected ) return {};
    // 写回发生在模板的原会话内；目标创建使用同步完毕的同一个对象。
    Logic::SessionUtils::syncBeatmap(context);
    return selected;
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
    // 提交使用完整字符串，输入框只负责显示和更新草稿。
    m_meta.name           = m_nameInput;
    m_meta.title          = m_titleInput;
    m_meta.title_unicode  = m_titleUnicodeInput;
    m_meta.artist         = m_artistInput;
    m_meta.artist_unicode = m_artistUnicodeInput;
    m_meta.album          = m_albumInput;
    m_meta.author         = m_authorInput;
    m_meta.version        = m_versionInput;
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
bool NewBeatmapWizard::applyMeasuredTimingsFromTool(
    const std::string& audioTrackId, const std::vector<::MMM::Timing>& timings)
{
    if ( !m_isOpen || timings.empty() ) {
        // 关闭后的异步结果和空结果不改变下一次向导状态。
        return false;
    }
    if ( !audioTrackId.empty() && !m_selectedAudioTrackId.empty() &&
         audioTrackId != m_selectedAudioTrackId ) {
        // 用户已切换音频时拒绝旧轨道的迟到测量结果。
        return false;
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
        if ( !std::isfinite(timing.m_timestamp) || !std::isfinite(bpm) ||
             bpm <= 0.0 ) {
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
        return false;
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
    if ( m_boundBpmToolManager )
        // 回调成功即直接推进突出层，不等待下一帧事件归约后再隐藏旧目标。
        m_boundBpmToolManager->walkthroughSpotlight().completeTarget(
            "new-beatmap.timing.auto");
    if ( m_manualBpmMeasurementActive ) {
        // 手动流程观察此标志后恢复被暂时关闭的模态向导。
        m_manualBpmMeasurementExported = true;
    }
    return true;
}

/// @brief 开始手动 BPM 测量并暂时收起向导模态弹窗。
/// @param tool 即将显示在前层的 BPM 测量工具。
///
/// ImGui 不适合叠加两个需要交互的模态层，因此手动测量期间关闭当前 popup，但保留
/// m_isOpen 和所有输入状态；工具结束后由 shouldWaitForManualBpmMeasurement
/// 重开。
/// 引导目标只在手动测量生命周期内委托给工具，不能让普通工具窗口永久取得
/// 向导遮罩的输入权限。委托使用当前阶段原有的语义 ID，使步骤进度保持不变。
/// 若用户在自动测量阶段主动改用手测，仍须等到有效 Timing 导出后才推进。
void NewBeatmapWizard::beginManualBpmMeasurement(BpmMeasurementToolView& tool)
{
    // active 区分临时收起与用户主动关闭向导。
    m_manualBpmMeasurementActive   = true;
    m_manualBpmMeasurementExported = false;
    // 手测期间向导目标不可见；把当前目标交给工具窗口维持遮罩亮区。
    // 自动测偏、结果复核和信息总览都可能打开手测，关闭后交还向导。
    tool.setWalkthroughTarget({});
    if ( m_boundBpmToolManager ) {
        const auto& spotlight = m_boundBpmToolManager->walkthroughSpotlight();
        if ( spotlight.awaitingTarget("new-beatmap.timing.auto") )
            tool.setWalkthroughTarget("new-beatmap.timing.auto");
        else if ( spotlight.awaitingTarget("new-beatmap.timing.bpm") )
            tool.setWalkthroughTarget("new-beatmap.timing.bpm");
        else if ( spotlight.awaitingTarget("new-beatmap.details") )
            tool.setWalkthroughTarget("new-beatmap.details");
        else
            tool.setWalkthroughTarget({});
    }
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
/// 手动测量可能因关闭窗口、导出、重开向导或项目切换而结束；每条路径均通过此处
/// 归还 Spotlight 目标，否则独立打开的旧工具会继续留有教程亮区。
void NewBeatmapWizard::unbindBpmMeasurementTool()
{
    if ( m_boundBpmToolManager ) {
        // UIManager 拥有工具，观察指针本身不能证明对象仍存活。
        auto* tool = m_boundBpmToolManager->getView<BpmMeasurementToolView>(
            BPM_MEASUREMENT_TOOL_VIEW_NAME);
        if ( tool && tool == m_boundBpmToolView ) {
            // 清除捕获 this 的函数对象后，工具可独立继续或销毁。
            tool->setMeasurementExportCallback({});
            tool->setWalkthroughTarget({});
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
    auto project = Logic::ProjectController::instance().uiSnapshot();
    if ( !project || project->m_generation != m_projectGeneration )
        return false;

    // 空内部名由其他创建校验处理，这里不制造重复提示。
    const auto& name = m_nameInput;
    if ( name.empty() ) return false;

    // 使用精确大小写比较，与项目 m_name 身份规则保持一致。
    return std::ranges::find(project->m_beatmapNames, name) !=
           project->m_beatmapNames.end();
}

/// @brief 汇总向导状态并经编辑器命令入口创建谱面。
/// @details 空白模式只携带基础元数据与测量 Timing；模板模式在同一会话锁内
/// 同步源会话并同步派发创建，防止命令观察到重载或后续编辑写入的模板数据。
/// 普通确认与重名确认复用同一入口，模板失效时保留草稿以便重新选择。
/// @warning 仅显式确认时调用。创建命令可能保存文件，禁止从绘制读路径触发。
void NewBeatmapWizard::submitCreateRequest()
{
    // 最后一次从控件缓冲同步，保证失焦前输入也进入命令。
    syncMetaFromInputs();

    // 命令使用值语义携带新谱面初始状态。
    Logic::CmdCreateBeatmap cmd;
    cmd.origin = m_origin;
    // 命令可能等待正在更新的会话；项目代次在真正创建前再次由引擎检查。
    cmd.projectGeneration = m_projectGeneration;
    cmd.baseMeta          = m_meta;
    cmd.initialTimings    = m_measuredTimings;
    // 创建指令由当前引擎入口同步消费，模板比空白模式多一次源身份校验。
    // 这里只在明确提交时才取得会话锁，锁的作用域不延伸到弹窗清理。
    auto& engine = Logic::EditorEngine::instance();
    if ( m_createMode == CreateMode::OpenTemplate ) {
        // 源同步及同步创建共用作用域，之间不能让会话替换或修改源。
        const std::lock_guard lock(engine.getSessionMutex());
        cmd.templateBeatmap = syncSelectedTemplateBeatmap();
        if ( !cmd.templateBeatmap ) return;
        cmd.templateOptions = m_templateOptions;
        engine.pushCommand(std::move(cmd));
    } else {
        engine.pushCommand(std::move(cmd));
    }
    // 清理事件绑定和临时交互状态，保留下一次 open 可重新 reset。
    close();
}

/// @brief 绘制当前打开谱面列表并处理模板选择。
/// @param templateOptions 本帧借用的不可变模板候选。
/// @param sourceManager 提供模板选择成功后的演练状态更新。
///
/// 弹窗使用稳定内部名称，列表项以 cameraId 构造唯一 ID。选择后先继承默认资源，
/// 再关闭选择器并请求打开复制选项弹窗。
void NewBeatmapWizard::renderTemplatePickerPopup(
    const std::vector<Logic::SessionTemplateOption>& templateOptions,
    UIManager*                                       sourceManager)
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
                if ( ::MMM::UI::FeedbackSelectable(label.c_str(), selected) &&
                     selectTemplate(option) ) {
                    // 选择成功后才推进到复制范围。
                    m_shouldOpenTemplateOptions = true;
                    ImGui::CloseCurrentPopup();
                    if ( sourceManager )
                        sourceManager->walkthroughSpotlight().completeTarget(
                            "new-beatmap.template.pick");
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
        // 布局完成后读取实际弹窗范围，包含可滚动候选和取消按钮。
        if ( sourceManager )
            sourceManager->walkthroughSpotlight().reportCurrentPopup(
                "new-beatmap.template.pick");
        ImGui::EndPopup();
    }
}

/// @brief 绘制模板内容复制范围选项弹窗。
/// @param sourceManager 提供选项确认后的演练状态更新。
///
/// 三个布尔项分别控制元数据、时间线和物件复制；确认与取消都只关闭弹窗，因为
/// checkbox 已直接写入向导状态，最终是否使用由创建命令决定。
/// 弹窗本身不要求模板仍存在；若源会话同时关闭，主向导下一帧会清理陈旧选择并禁用
/// 创建。所有控件使用反馈包装以保持全局悬停和点击音效一致。
void NewBeatmapWizard::renderTemplateOptionsPopup(UIManager* sourceManager)
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
            if ( sourceManager )
                sourceManager->walkthroughSpotlight().completeTarget(
                    "new-beatmap.template.options");
        }
        ImGui::SameLine();
        // 取消沿用当前 checkbox 值，与即时编辑控件语义一致。
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.wizard.new_beatmap.cancel").data(),
                 ImVec2(120.0f, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        // 复制选项弹窗接管目标边界，允许逐项勾选后确认。
        if ( sourceManager )
            sourceManager->walkthroughSpotlight().reportCurrentPopup(
                "new-beatmap.template.options");
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
        const auto& name = m_nameInput;
        ImGui::TextWrapped(
            "%s",
            TR_FMT("ui.wizard.new_beatmap.duplicate_name.message", name)
                .c_str());

        ImGui::Spacing();
        // 继续按钮绕过本轮重复检查并直接构造命令。
        const bool continueClicked = ::MMM::UI::FeedbackButton(
            TR("ui.wizard.new_beatmap.duplicate_name.continue").data(),
            ImVec2(140.0f, 0.0f));
        if ( continueClicked ) {
            ImGui::CloseCurrentPopup();
            submitCreateRequest();
        }
        // 重名确认只在警告弹窗出现时可见，作为创建按钮之后的引导阶段。
        if ( sourceManager )
            sourceManager->walkthroughSpotlight().reportLastItem(
                "new-beatmap.duplicate.continue");
        if ( continueClicked && sourceManager )
            // 创建请求已经提交后才完成重名确认目标。
            sourceManager->walkthroughSpotlight().completeTarget(
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
/// @warning 每帧调用且不应获取会话锁；只有点击候选才进入显式选择事务。
/// 每帧先校验持久化选择仍在候选中；没有候选时强制回退空白模式。弹窗打开请求
/// 使用一次性布尔位，确保 OpenPopup 与对应 BeginPopup 在同一 UI 调用链中完成。
void NewBeatmapWizard::renderTemplateSourceControls(
    const std::vector<Logic::SessionTemplateOption>& templateOptions,
    UIManager*                                       sourceManager)
{
    if ( templateOptions.empty() && m_createMode == CreateMode::OpenTemplate ) {
        // 最后一个源会话关闭后，模板模式已不可提交。
        m_createMode = CreateMode::Blank;
        // 清除所有与失效源对象关联的身份和共享所有权。
        m_templateCameraId.clear();
        m_templateDisplayName.clear();
        m_templateBeatmap.reset();
    }

    if ( !m_templateCameraId.empty() ) {
        const auto selected =
            std::ranges::find(templateOptions,
                              m_templateCameraId,
                              &Logic::SessionTemplateOption::cameraId);
        // 只比较所有权身份。常态不提升弱引用，也不复制来源谱面引用。
        if ( selected == templateOptions.end() ||
             selected->beatmap.owner_before(m_templateBeatmap) ||
             m_templateBeatmap.owner_before(selected->beatmap) ) {
            m_templateCameraId.clear();
            m_templateDisplayName.clear();
            m_templateBeatmap.reset();
        }
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
    if ( sourceManager &&
         (chooseTemplate || m_createMode == CreateMode::OpenTemplate) )
        // 当前模式是稳定业务状态，可在每帧直接推进而不依赖点击推测。
        // 重放引导时若已处于模板模式，也应立即等待真正的模板选择目标。
        sourceManager->walkthroughSpotlight().completeTarget(
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
            !m_templateCameraId.empty()
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
        if ( sourceManager && !m_shouldOpenTemplatePicker &&
             !ImGui::IsPopupOpen("NewBeatmapTemplatePicker") )
            sourceManager->walkthroughSpotlight().reportLastItem(
                "new-beatmap.template.pick");
        ImGui::SameLine();
        if ( m_templateCameraId.empty() ) {
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
        if ( sourceManager && !m_shouldOpenTemplateOptions &&
             !ImGui::IsPopupOpen("NewBeatmapTemplateOptions") )
            sourceManager->walkthroughSpotlight().reportLastItem(
                "new-beatmap.template.options");
        if ( m_templateCameraId.empty() ) {
            ImGui::EndDisabled();
        }
    }

    if ( m_shouldOpenTemplatePicker ) {
        // 消费一次性请求，避免每帧重置弹窗打开状态。
        ::MMM::UI::FeedbackOpenPopup("NewBeatmapTemplatePicker");
        m_shouldOpenTemplatePicker = false;
    }
    // BeginPopupModal 自行判断当前是否真正打开。
    renderTemplatePickerPopup(templateOptions, sourceManager);

    if ( m_shouldOpenTemplateOptions ) {
        // 选择模板后或用户点击选项按钮时打开。
        ::MMM::UI::FeedbackOpenPopup("NewBeatmapTemplateOptions");
        m_shouldOpenTemplateOptions = false;
    }
    renderTemplateOptionsPopup(sourceManager);
}

/// @brief 按目标资源类型打开原生或 ImGui 文件选择器。
/// @param target 音频、封面图或背景资源目标。
///
/// 原生选择器通过统一异步入口返回路径，桌面仍在调用期间完成，Android
/// 等待系统授权。 内置选择器继续由向导模态内部推进，避免被父窗口挡住。
/// 三类资源共用导入校验，取消不修改选择或产生复制；错误只显示本次请求状态。
/// @warning 用户触发的低频入口；Android 不阻塞 SDL 生命周期。
void NewBeatmapWizard::openResourcePicker(ResourceTarget target)
{
    // 系统已有在途请求时不能替换用途或覆盖原回调。
    // 选择器恢复 Activity 后仍由原请求消费，重复点击不能创建第二份资源。
    if ( m_resourcePickerLifetime ) return;
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
    if ( NativeFileDialog::requiresSystemPicker() ||
         settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 平台能力检查优先于桌面样式偏好；Android 未授权目录访问时必须走 SAF。
        // 不能调用同步 NFD 后再降级，因为该入口没有可等待的 Android 结果槽。
        // 捕获正式项目身份，系统选择期间切换项目不能把资源导入另一个项目。
        const auto project = Logic::ProjectController::instance().uiSnapshot();
        if ( !project || project->m_generation != m_projectGeneration ) return;
        const auto projectRoot = project->m_projectRoot;
        // 令牌只在用户点击时分配，UI 线程回调与销毁不会并发。
        m_resourcePickerLifetime = std::make_shared<const bool>(true);
        // 原生窗口打开前播放统一弹窗反馈。
        PlayPopupOpenFeedback();
        const nfdu8filteritem_t filter{
            title,
            audio ? "mp3,ogg,wav,flac,opus,aac,m4a"
            : target == ResourceTarget::Background
                ? "png,jpg,jpeg,bmp,mp4,avi,mkv,webm,mov,flv,m4v"
                : "png,jpg,jpeg,bmp"
        };
        NativeFileDialog::openFileAsync(
            &filter,
            1,
            settings.lastFilePickerPath.c_str(),
            [this,
             target,
             projectRoot,
             lifetime = std::weak_ptr<const bool>(m_resourcePickerLifetime)](
                nfdresult_t result, const std::string& selectedPath) {
                // 必须先验证弱令牌再访问 this；旧向导和旧请求不能复活。
                if ( lifetime.expired() ) return;
                // 先释放等待状态，再进行导入；失败后允许用户立即从同一入口重试。
                m_resourcePickerLifetime.reset();
                if ( result == NFD_OKAY && !selectedPath.empty() ) {
                    completeResourceSelection(
                        Config::utf8ToPath(selectedPath), projectRoot, target);
                } else if ( result == NFD_ERROR ) {
                    // Android 不读取同步 NFD 的能力错误；取消保持已有草稿。
                    m_resourceImportError =
                        TR("ui.wizard.new_beatmap.import_failed").toString();
                }
            });
        // 桌面同步完成、Android 继续事件循环，均不再重复打开内置对话框。
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

/// @brief 在异步返回边界复核向导和项目身份，再使用原有资源导入事务。
/// @param path 系统完成授权及工作副本准备后提供的普通路径。
/// @param projectRoot 请求发起时的项目根目录。
/// @param target 请求发起时的资源用途，不读取后来点击的控件状态。
/// @warning 低频完成回调；入队前读取不可变工程快照，不等待会话。
void NewBeatmapWizard::completeResourceSelection(
    const std::filesystem::path& path, const std::filesystem::path& projectRoot,
    ResourceTarget target)
{
    // 关闭向导是明确的取消边界，即使返回路径有效也不能提交草稿。
    if ( !m_isOpen ) return;
    // 系统选择器可在切换工程后返回。这里仅查询不可变工程身份。
    const auto project = Logic::ProjectController::instance().uiSnapshot();
    if ( !project || project->m_generation != m_projectGeneration ||
         project->m_projectRoot != projectRoot )
        return;
    // 用途取自请求快照，避免后台返回的封面被当成之后点击的音轨导入。
    // 导入继续复用扩展名校验、音频解码和项目相对路径生成，不另写复制流程。
    m_resourceTarget = target;
    importResource(path);
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
    // 文件名编辑与浏览共用键盘安全区，不能以固定最小高度覆盖 IME 避让。
    const auto size = mobileFileDialogSize(
        Config::AppConfig::instance().getWindowContentScale());
    Utils::prepareCenteredModalWindow(size);
    if ( dialog->Display(
             "NewBeatmapResourcePicker", ImGuiWindowFlags_NoCollapse, size) ) {
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
/// 立即检查媒体类别，文件池独立完成探测和复制。输入可以来自系统选择器、
/// 内置对话框或拖放，三条入口均采用相同的用途与工程校验。
/// 没有工作线程时向导保留现有选择及可见诊断，避免 UI 同步回退到读盘。
/// 向导关闭、重开或工程切换后，
/// 文件结果不会绑定到新的草稿；已经复制完成的文件仍属于原工程。
/// @warning 只由明确的选择或拖放动作入队，不通过轮询启动重复复制。
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
    // 请求只由本轮草稿捕获当前可写工程，文件池不会读取保存中的可变工程。
    // 单一在途任务也避免同一向导的重复拖入交错改写字段。
    const auto project = Logic::ProjectController::instance().uiSnapshot();
    if ( !project || project->m_generation != m_projectGeneration ||
         project->m_isTemporaryProject || m_pendingImport.valid() )
        return;
    // 独占弱令牌在关闭时失效，尚未开始的文件操作可安全跳过。
    m_pendingImportLifetime = std::make_shared<const bool>(true);
    m_pendingImport         = Utils::queueProjectMediaImport(
        project->m_projectRoot,
        path,
        m_resourceTarget == ResourceTarget::Audio,
        project->m_generation,
        m_pendingImportLifetime);
    if ( !m_pendingImport.valid() )
        m_resourceImportError =
            TR("ui.wizard.new_beatmap.import_failed").toString();
    else {
        m_pendingImportTarget = m_resourceTarget;
        m_pendingImportPreviousPath =
            m_resourceTarget == ResourceTarget::Audio
                ? m_selectedAudioPath
                : (m_resourceTarget == ResourceTarget::Cover
                       ? m_selectedCoverImgPath
                       : m_selectedCoverPath);
    }
}

/// @brief 在结果就绪时提交资源选择。项目切换或关闭不会绑定旧结果。
/// @details 检查草稿的可见状态、绑定工程代次和原选中路径。三者任一变化，
/// 后台结果只作为已完成的原工程文件留下，不回写当前页面。
/// 音频使用探测快照与正式资源 ID，同一次完成不会重复解码；图片和视频直接
/// 更新各自路径，缺少封面时仍允许背景图作为封面。
/// @warning 每帧只做零超时检查；磁盘复制和音频探测已经在文件池完成。
void NewBeatmapWizard::pollResourceImport()
{
    if ( !m_pendingImport.valid() ||
         m_pendingImport.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready )
        return;
    auto&            engine = Logic::EditorEngine::instance();
    std::unique_lock lock(engine.getSessionMutex(), std::try_to_lock);
    // 更新循环持锁时等下一帧，结果保持有效，不会在 UI 线程等待更新结束。
    if ( !lock ) return;
    const auto operation = m_pendingImport.get();
    // 只标记当前任务已结束。关闭或重开向导会丢弃自己的结果槽，
    // 即使工作者尚未完成，也不能把旧关联写入新一轮草稿。
    m_pendingImportLifetime.reset();
    const auto identity = Logic::ProjectController::instance().uiSnapshot();
    auto*      project  = engine.getCurrentProject();
    if ( !m_isOpen || !identity ||
         identity->m_generation != m_projectGeneration ||
         identity->m_isTemporaryProject || !project )
        return;
    const auto& selection =
        m_pendingImportTarget == ResourceTarget::Audio
            ? m_selectedAudioPath
            : (m_pendingImportTarget == ResourceTarget::Cover
                   ? m_selectedCoverImgPath
                   : m_selectedCoverPath);
    // 用户可能在复制另一首歌期间改选已存在的配乐，主动选择仍优先。
    // 校验精确目标，封面和背景共用文件时也不能随意切换媒体用途。
    // 只在身份确认后解释工作结果，旧请求不会留下新的错误提示。
    // 这里消费已探测的标签，避免对同一音频发起重复的解码查询。
    if ( selection != m_pendingImportPreviousPath ) return;
    if ( operation->m_error ) {
        m_resourceImportError = operation->m_error.message();
        return;
    }
    const auto imported = &operation->m_path;
    if ( m_pendingImportTarget == ResourceTarget::Audio ) {
        // 先让逻辑引擎登记复制后的绝对路径为主音轨。
        const auto audioId =
            Logic::ProjectCommandService::registerPreparedMainAudio(*project,
                                                                    *imported);
        if ( audioId.empty() ) {
            m_resourceImportError =
                TR("ui.wizard.new_beatmap.no_main_audio").toString();
            return;
        }
        // 媒体及配套文件已准备完成；这里仅保存更新后的工程清单。
        engine.saveProject();
        // 选择 ID 后探测元数据并填充标题、艺术家和时长。
        m_selectedAudioTrackId = audioId;
        m_selectedAudioPath    = *imported;
        m_measuredTimings.clear();
        publishInteraction(Event::BeatmapCreateInteractionStage::AudioSelected);
        applyAudioInfo(*operation->m_audio);
    } else if ( m_pendingImportTarget == ResourceTarget::Cover ) {
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
/// 用户点击项目元数据导入后，音频探测只更新时长和内部名，不覆盖明确选择的文本。
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
    // 仅消费一次就绪结果；普通绘制从不启动新的文件任务。
    pollResourceImport();
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

    // ImGui 输入只借用本次提供的地址，编辑导致扩容时由回调交回新地址。
    // 草稿始终持有完整 UTF-8，未编辑时不会因显示范围或旧缓冲上限被截断。
    // 容量包含可写字符数，另给终止符一个位置。此处不预分配每帧缓冲。
    // 回调只迁移当前控件对应的字符串，不修改其它字段或谱面对象。
    const auto drawInput = [](const char* label, std::string& value) {
        ImGui::InputText(
            label,
            value.data(),
            value.capacity() + 1,
            ImGuiInputTextFlags_CallbackResize,
            [](ImGuiInputTextCallbackData* data) {
                auto& text = *static_cast<std::string*>(data->UserData);
                // ImGui 随后按 BufTextLen
                // 写入，调整逻辑长度与容量必须先于拷贝。
                text.resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = text.data();
                return 0;
            },
            &value);
    };

    // 基础信息到视觉资源共同组成教程的“补充谱面信息”目标区域。
    const ImVec2 detailsAreaMin = ImGui::GetCursorScreenPos();
    // 基础信息字段直接编辑向导缓冲，提交时统一同步到 m_meta。
    ImGui::SeparatorText(TR("ui.settings.beatmap.info").data());
    // 仅当前项目能提供权威信息；没有项目时继续显示下方已有的错误说明。
    auto project = Logic::ProjectController::instance().uiSnapshot();
    // 向导绑定打开时的工程，切换后不允许重新绑定旧草稿中的相对路径。
    if ( project && project->m_generation != m_projectGeneration )
        project.reset();
    if ( project &&
         FeedbackButton(
             TR("ui.wizard.new_beatmap.fill_from_project").data()) ) {
        // 项目标题和艺术家没有独立 Unicode 字段，复制到两种谱面展示文本。
        m_titleInput         = project->m_title;
        m_titleUnicodeInput  = project->m_title;
        m_artistInput        = project->m_artist;
        m_artistUnicodeInput = project->m_artist;
        m_authorInput        = project->m_mapper;
        // 本次按钮操作覆盖展示字段；随后重新选音频则由新音频标签更新这些字段。
        // 难度、BPM、资源和轨道数属于单张谱面，不从项目展示元数据推导。
    }
    drawInput(TR("ui.settings.beatmap.name").data(), m_nameInput);
    drawInput(TR("ui.settings.beatmap.title").data(), m_titleInput);
    drawInput(TR("ui.settings.beatmap.title_unicode").data(),
              m_titleUnicodeInput);
    drawInput(TR("ui.settings.beatmap.artist").data(), m_artistInput);
    drawInput(TR("ui.settings.beatmap.artist_unicode").data(),
              m_artistUnicodeInput);
    drawInput(TR("ui.settings.beatmap.album").data(), m_albumInput);
    drawInput(TR("ui.settings.beatmap.mapper").data(), m_authorInput);
    drawInput(TR("ui.settings.beatmap.version").data(), m_versionInput);

    // 偏好区保存双精度 BPM，但 ImGui 控件临时使用 float。
    ImGui::SeparatorText(TR("ui.settings.beatmap.preference").data());
    float      bpm        = (float)m_bpm;
    const bool bpmChanged = ::MMM::UI::FeedbackDragFloat(
        TR("ui.settings.beatmap.bpm").data(),
        &bpm,
        0.1f,
        static_cast<float>(::MMM::MIN_NORMALIZED_BPM),
        static_cast<float>(::MMM::MAX_NORMALIZED_BPM),
        "%.2f");
    if ( bpmChanged ) {
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
    if ( bpmChanged && sourceManager )
        // 数值实际写回后才视为修正动作完成；只点击控件不会推进。
        sourceManager->walkthroughSpotlight().completeTarget(
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
    if ( !project ) {
        // 无项目时给出错误并保持向导打开，等待生命周期更新。
        m_pendingDrops.clear();
        ImGui::TextColored(Utils::UIThemeUtils::getDangerColor(),
                           "%s",
                           TR("ui.wizard.new_beatmap.no_project").data());
        ImGui::EndPopup();
        return;
    }

    // 候选跟随已发布会话列表；绘制只借用同一份不可变结果。
    // 单个读句柄保证关谱发布新列表时，本帧打开的模板弹层仍能安全完成。
    // 列表中是弱身份，静置帧不会复制各源谱的拥有权或遍历可变会话。
    const auto sessions =
        Logic::EditorEngine::instance().getSessionUiSnapshot();
    renderTemplateSourceControls(sessions->templateOptions, sourceManager);

    ImGui::SeparatorText(TR("ui.settings.beatmap.resource").data());

    // 音频预览显示项目相对路径，未选择时显示本地化占位。
    std::string audioPreview =
        m_selectedAudioPath.empty()
            ? TR("ui.wizard.new_beatmap.select_audio").data()
            : Config::pathToUtf8(m_selectedAudioPath);
    // 发布模型已过滤音效资源，静置绘制只读容器大小。
    const bool hasSelectableMainAudio = !project->m_audioResources.empty();

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
    const float autoBpmWidth = ImGui::CalcTextSize(autoBpmLabel).x +
                               ImGui::GetStyle().FramePadding.x * 2.0f;
    const auto  renameLabel = TR("ui.file_manager.context.rename").toString();
    const float renameButtonWidth = ImGui::CalcTextSize(renameLabel.c_str()).x +
                                    ImGui::GetStyle().FramePadding.x * 2.0F;
    // 三行同时预留导入和改名按钮，不能覆盖下拉框箭头或截断长路径。
    const float comboWidth = std::max(
        1.0F,
        ImGui::GetContentRegionAvail().x - importButtonWidth -
            renameButtonWidth - ImGui::GetStyle().ItemSpacing.x * 2.0F);
    // 改名与导入并排，资源选择继续使用原有拖放命中区域。
    // 用途随按钮传递，不依赖导入选择器保存的目标类型。
    const auto drawRename = [&](ResourceTarget               target,
                                const std::filesystem::path& selected) {
        ImGui::SameLine();
        // 三种资源共享标签，但用途 ID 保证按钮状态互不串线。
        ImGui::PushID(static_cast<int>(target));
        // 未绑定资源或临时只读项目不能重命名；音频仍受外层自动分析禁用约束。
        ImGui::BeginDisabled(selected.empty() || project->m_isTemporaryProject);
        if ( FeedbackButton(renameLabel.c_str(), { renameButtonWidth, 0.0F }) )
            requestResourceRename(target);
        ImGui::EndDisabled();
        ImGui::PopID();
    };

    // 音频下拉、导入和拖放作为一个整体受自动分析状态约束。
    if ( backgroundAutomaticMeasurementActive ) {
        ImGui::BeginDisabled();
    }
    ImGui::SetNextItemWidth(comboWidth);
    // 音频行以一个复合语义区域覆盖下拉框与导入按钮，布局变化时仍可整体跟随。
    const ImVec2 audioRowMin = ImGui::GetCursorScreenPos();
    if ( ::MMM::UI::FeedbackBeginCombo("##NewBeatmapAudioSelect",
                                       audioPreview.c_str()) ) {
        // 候选直接借用本帧项目快照，不在组合框中读取可变资源表。
        for ( const auto& res : project->m_audioResources ) {
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
        // 下拉选项可能延伸到音频行下方，随展开窗口同步更新遮罩。
        if ( sourceManager )
            sourceManager->walkthroughSpotlight().reportCurrentPopup(
                "new-beatmap.audio");
        ::MMM::UI::FeedbackEndCombo();
    }
    ImGui::SameLine();
    // 自动分析未运行时，刚绘制的 combo 区域可接受音频拖放。
    if ( !backgroundAutomaticMeasurementActive )
        handleResourceDrop(ResourceTarget::Audio);
    if ( FeedbackButton(importAudioLabel, ImVec2(importButtonWidth, 0.0f)) )
        openResourcePicker(ResourceTarget::Audio);
    // 音频改名与导入一起服从自动分析期间的禁用约束。
    drawRename(ResourceTarget::Audio, m_selectedAudioPath);
    if ( sourceManager )
        sourceManager->walkthroughSpotlight().reportTarget(
            "new-beatmap.audio",
            audioRowMin,
            ImGui::GetItemRectMax(),
            ImGui::GetWindowViewport());
    if ( sourceManager && !m_selectedAudioPath.empty() )
        // 已绑定有效音频是每帧可验证的完成状态，避免选择后旧目标反弹。
        // 该判断覆盖下拉选择、拖放和两种文件选择器的统一结果。
        sourceManager->walkthroughSpotlight().completeTarget(
            "new-beatmap.audio");
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
    if ( sourceManager ) {
        // 手测是自动测量与结果复核两阶段的可选入口，单独留出按钮亮区。
        // BPM 数值框位于向导上方，自动按钮位于同一行；辅助区域避免将
        // 两者之间的表单字段一并解锁，也不改变教程步骤的完成条件。
        const ImVec2 minimum   = ImGui::GetItemRectMin();
        const ImVec2 maximum   = ImGui::GetItemRectMax();
        auto&        spotlight = sourceManager->walkthroughSpotlight();
        // BPM 数值框滚出可视范围时，手测按钮仍可作为该阶段的备用锚点。
        // 主控件可见时 fallback 不会覆盖原本的提示位置。
        // 备用锚点按每帧实际按钮位置上报，窗口滚动后不沿用历史矩形。
        // 用户从该入口返回时，向导复核阶段仍保持原有目标水位。
        spotlight.reportTarget("new-beatmap.timing.bpm",
                               minimum,
                               maximum,
                               ImGui::GetWindowViewport(),
                               true,
                               true);
        spotlight.reportCompanionRegion("new-beatmap.timing.auto",
                                        minimum,
                                        maximum,
                                        ImGui::GetWindowViewport());
        spotlight.reportCompanionRegion("new-beatmap.timing.bpm",
                                        minimum,
                                        maximum,
                                        ImGui::GetWindowViewport());
    }
    ImGui::SameLine();
    if ( ::MMM::UI::FeedbackButton(autoBpmLabel, ImVec2(autoBpmWidth, 0.0f)) ) {
        openBpmTool(true);
    }
    // 自动测偏按钮是推荐入口；手动工具仍沿用原有并列布局与行为。
    if ( sourceManager )
        sourceManager->walkthroughSpotlight().reportLastItem(
            "new-beatmap.timing.auto");
    if ( sourceManager && !m_measuredTimings.empty() )
        // 只有有效 Timing 已回填才完成自动测量目标，点击启动按钮并不等价。
        // 已有结果重放时同样直接退出旧高亮，交由下一复核步骤处理。
        sourceManager->walkthroughSpotlight().completeTarget(
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
        // 第一次展开时启动或复用后台任务。就绪后绘制使用同一列表，
        // 收起不会等待或取消扫描，随后展开另一个列表可直接接续结果。
        if ( const auto* resources = readVisualResources(*project) ) {
            for ( const auto& resource : resources->entries ) {
                if ( resource.video ) continue;
                // 图片用于缩略图，视频只会出现在独立的背景列表。
                const auto& path     = resource.path;
                const bool  selected = (m_selectedCoverImgPath == path);
                if ( ::MMM::UI::FeedbackSelectable(path.c_str(), selected) )
                    m_selectedCoverImgPath = path;
                if ( selected ) ImGui::SetItemDefaultFocus();
            }
        } else {
            ImGui::TextDisabled(
                "%s",
                TR("ui.status.project_loading.scanning_directory").data());
        }
        // “补充信息”阶段包含封面选择，展开后必须照亮完整资源列表。
        // 文件数变化会改变弹窗高度，不能沿用打开瞬间的旧坐标。
        if ( sourceManager )
            sourceManager->walkthroughSpotlight().reportCurrentPopup(
                "new-beatmap.details");
        ::MMM::UI::FeedbackEndCombo();
    }

    // 最近绘制的封面 combo 接收单图片文件拖放。
    handleResourceDrop(ResourceTarget::Cover);
    ImGui::SameLine();
    if ( FeedbackButton(importCoverLabel, ImVec2(importButtonWidth, 0.0f)) )
        openResourcePicker(ResourceTarget::Cover);
    // 封面入口传独立用途，不能把背景字段误当作目标。
    drawRename(ResourceTarget::Cover, m_selectedCoverImgPath);

    // 背景允许图片或视频，预览同样显示项目相对路径。
    std::string coverPreview =
        m_selectedCoverPath.empty()
            ? TR("ui.wizard.new_beatmap.select_cover").data()
            : Config::pathToUtf8(m_selectedCoverPath);

    ImGui::SetNextItemWidth(comboWidth);
    if ( ::MMM::UI::FeedbackBeginCombo("##NewBeatmapBackgroundSelect",
                                       coverPreview.c_str()) ) {
        if ( const auto* resources = readVisualResources(*project) ) {
            for ( const auto& resource : resources->entries ) {
                const auto& path     = resource.path;
                const bool  selected = (m_selectedCoverPath == path);
                if ( ::MMM::UI::FeedbackSelectable(path.c_str(), selected) ) {
                    m_selectedCoverPath = path;
                    m_meta.cover_type =
                        resource.video ? CoverType::VIDEO : CoverType::IMAGE;
                    // 保留用户明确填写的封面；只有未设置时才从图片背景继承。
                    if ( !resource.video && m_selectedCoverImgPath.empty() )
                        m_selectedCoverImgPath = path;
                }
                if ( selected ) ImGui::SetItemDefaultFocus();
            }
        } else {
            ImGui::TextDisabled(
                "%s",
                TR("ui.status.project_loading.scanning_directory").data());
        }
        // 背景列表与封面列表遵守同一帧范围契约，收起后自动恢复。
        if ( sourceManager )
            sourceManager->walkthroughSpotlight().reportCurrentPopup(
                "new-beatmap.details");
        ::MMM::UI::FeedbackEndCombo();
    }

    // 最近绘制的背景 combo 接受图片或视频拖放。
    handleResourceDrop(ResourceTarget::Background);
    ImGui::SameLine();
    if ( FeedbackButton(importBackgroundLabel,
                        ImVec2(importButtonWidth, 0.0f)) )
        openResourcePicker(ResourceTarget::Background);
    // 背景既可为图片也可为视频，改名不改变媒体类型。
    drawRename(ResourceTarget::Background, m_selectedCoverPath);
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
        m_createMode == CreateMode::OpenTemplate && m_templateCameraId.empty();
    // 音频是两种创建模式共同的最低提交条件。
    const bool canCreate =
        !m_selectedAudioPath.empty() && !needsTemplateSelection;

    if ( !canCreate ) {
        ImGui::BeginDisabled();
    }

    const bool createClicked = ::MMM::UI::FeedbackButton(
        TR("ui.wizard.new_beatmap.create").data(), ImVec2(120, 0));
    if ( createClicked ) {
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
    if ( createClicked && sourceManager )
        // 校验已通过并进入提交或重名确认分支，当前创建目标不应再次显示。
        sourceManager->walkthroughSpotlight().completeTarget(
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
    // 改名弹窗必须与资源选择器一样处于父向导的模态生命周期内。
    renderResourceRename(dpiScale);
    if ( sourceManager && !m_selectedAudioPath.empty() )
        // 选择器在资源区之后提交，成功导入仍须在同一帧完成音频目标。
        sourceManager->walkthroughSpotlight().completeTarget(
            "new-beatmap.audio");
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
    m_origin           = origin;
    const auto project = Logic::ProjectController::instance().uiSnapshot();
    // 无工程时保存哨兵代次；随后打开另一个工程也不沿用这次草稿。
    m_projectGeneration = project ? project->m_generation : 0;
    publishInteraction(Event::BeatmapCreateInteractionStage::WizardOpened);
}

/// @brief 用 BPM 工具的主音轨启动新建流程，最终仍由用户确认创建。
/// @param audioTrackId 当前项目内所测量的主音轨 ID。
/// @param timings 测量工具转换后的毫秒 Timing，仍需经过向导常规验证。
/// @details 先选音轨再回填段落，因为向导会在切换音频时清除已有测量。
/// 只匹配创建向导可选的 Main 音轨，避免把试听效果音当作主音乐。
/// 已有向导草稿优先，保留用户填写的元数据和手动测量回填。
/// 本入口只建立草稿；创建、重名处理和命令入队仍由向导统一完成。
/// @return 成功打开预填草稿返回 true。
bool NewBeatmapWizard::openWithMeasuredTimings(
    const std::string& audioTrackId, const std::vector<::MMM::Timing>& timings)
{
    const auto project = Logic::ProjectController::instance().uiSnapshot();
    if ( m_isOpen || !project || audioTrackId.empty() || timings.empty() )
        return false;

    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_id != audioTrackId || resource.m_path.empty() )
            continue;

        open();
        if ( m_projectGeneration != project->m_generation ) {
            // 工程可在工具结果与真正打开向导之间更换，不能复用旧轨道的测量值。
            close();
            return false;
        }
        m_selectedAudioTrackId = resource.m_id;
        onAudioSelected(Config::utf8ToPath(resource.m_path));
        if ( applyMeasuredTimingsFromTool(audioTrackId, timings) ) return true;
        // 无效数据不能留下一个看似已接收测量结果的空向导。
        close();
        return false;
    }
    return false;
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
    // 系统授权可能晚于关闭返回，此时回调只能释放结果，不能访问草稿。
    m_resourcePickerLifetime.reset();
    // 文件任务只能用弱令牌，释放 future 不需要等待正在执行的系统复制。
    m_pendingImportLifetime.reset();
    m_pendingImport = {};
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
    // 忽略上一次草稿的文件结果。保留未完成 future，避免反复开关积压扫描。
    m_visualResourceRevision->fetch_add(1, std::memory_order_relaxed);
    m_visualResources.reset();
    // 再次打开是新草稿，旧系统选择结果不得绑定到这一轮。
    m_pendingImport = {};
    m_pendingImportLifetime.reset();
    m_resourcePickerLifetime.reset();
    // 清理瞬时输入和上一轮可见错误。
    m_pendingDrops.clear();
    m_resourceImportError.clear();
    m_resourceRenameError.clear();
    m_openRenamePopup  = false;
    m_focusRenameInput = false;
    m_renameProjectRoot.clear();
    m_renameSourcePath.clear();
    // 音轨 ID 与路径分别清空，下一次改名重新捕获当前选择。
    m_renameAudioId.clear();
    // 使用值初始化的 BaseMapMeta 清除所有遗留元数据字段。
    m_meta = MMM::BaseMapMeta();

    // 默认制谱者来自用户设置，空配置时回退明确占位。
    const auto& defaultCreator =
        Config::AppConfig::instance().getEditorSettings().defaultCreator;

    // 新谱面提供常见四轨和 120 BPM 起点。
    m_bpm        = 120.0;
    m_trackCount = 4;
    m_measuredTimings.clear();

    // 新一轮创建从清空的草稿开始，导入的文本长度不影响下一次初始化。
    m_nameInput          = "New Beatmap";
    m_titleInput         = "";
    m_titleUnicodeInput  = "";
    m_artistInput        = "";
    m_artistUnicodeInput = "";
    m_albumInput         = "";
    m_authorInput        = defaultCreator.empty() ? "Unknown" : defaultCreator;
    m_versionInput       = "Easy";
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
    auto project = Logic::ProjectController::instance().uiSnapshot();
    if ( !project || project->m_generation != m_projectGeneration ) {
        m_selectedAudioPath = path;
        return;
    }

    // 项目内资源尽量使用可移植相对路径。
    m_selectedAudioPath =
        normalizeProjectResourcePath(project->m_projectRoot, path);
    // 路径已规范化且满足提交条件后才记录准备阶段；探针失败不撤销有效选择。
    publishInteraction(Event::BeatmapCreateInteractionStage::AudioSelected);

    // 音频探针需要项目根解析后的绝对路径。
    auto absPath = project->m_projectRoot / m_selectedAudioPath;
    auto infoOpt = MMM::Utils::AudioInfoUtils::probeAudioInfo(absPath);
    if ( infoOpt ) {
        applyAudioInfo(*infoOpt);
    }
}

/// @brief 复用已有标签处理，不从磁盘重新探测素材。
/// @param info 由音频选择或后台导入得到的独立探测结果。
/// @details 同一份标签填充普通和 Unicode 字段，用户仍可独立修改它们。
/// 专辑以音频为准，新选择不会沿用上一首歌的专辑。
/// 内部名称去掉 ASCII 空白以保持命名习惯，展示字段保留原文本。
/// @details 元数据与文件名的限制不同。只为较短标题生成内部名，
/// 否则保留草稿原名，用户仍可直接编辑文件名来源。
/// 未知时长仍由提交端使用原有的有限值检查，不影响非 ASCII 标签。
void NewBeatmapWizard::applyAudioInfo(const ::MMM::Utils::AudioInfo& info)
{
    // 向导内部以秒保存，提交时再转为毫秒。
    m_audioDuration = info.duration;

    // 选择音频是新的用户操作，标签应替换旧项目值或前一音频的文本。
    // 普通与 Unicode 栏先用同一标签，仍允许随后分别手动调整。
    m_titleInput         = info.title;
    m_titleUnicodeInput  = info.title;
    m_artistInput        = info.artist;
    m_artistUnicodeInput = info.artist;
    // 项目没有专辑字段，因此音频标签始终作为专辑初始值。
    // 重新选择音频时同步刷新专辑，避免显示前一个资源的标签。
    m_albumInput = info.album;

    // 项目一键导入只覆盖展示字段，内部名称仍沿用音频标题的原有推导。
    // 去掉空白可避免生成包含空格的常见不便标识。
    std::string safeName = info.title;
    // remove_if 只修改本地副本，不改变展示标题。
    safeName.erase(
        std::remove_if(safeName.begin(),
                       safeName.end(),
                       [](char ch) {
                           return std::string_view(" \t\r\n\v\f").find(ch) !=
                                  std::string_view::npos;
                       }),
        safeName.end());
    // 过长音频标签完整保存在标题栏；内部名仍需为 .mmm 文件名保留空间。
    if ( !safeName.empty() && safeName.size() <= 251 )
        m_nameInput = std::move(safeName);
}

}  // namespace MMM::UI
