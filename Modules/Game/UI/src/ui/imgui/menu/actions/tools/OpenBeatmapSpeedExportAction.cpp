#define IMGUI_DEFINE_MATH_OPERATORS
#include "audio/AudioManager.h"
#include "audio/AudioSpeedExportService.h"
#include "common/BeatmapAudioTimelineCompatibility.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/SessionUtils.h"
#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "runtime/AppThreadPool.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <concurrentqueue.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <ice/thread/ThreadPool.hpp>
#include <imgui.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 倍速音频导出格式选项。
/// @details 将用户可见名称与传递给导出服务的扩展名保持在同一只读表中。
struct SpeedExportAudioFormatOption {
    /// @brief 下拉框显示名。
    const char* label;

    /// @brief 输出扩展名；空字符串表示跟随源音频。
    const char* extension;
};

/// @brief 获取倍速音频导出格式选项表。
/// @return 格式选项表。
/// @note 返回静态只读对象，选项顺序同时是弹窗持久状态使用的索引协议。
const std::array<SpeedExportAudioFormatOption, 8>&
speedExportAudioFormatOptions()
{
    // 首项使用空扩展名表达沿用源格式，其余项显式选择编码容器。
    static constexpr std::array<SpeedExportAudioFormatOption, 8> options{ {
        { "跟随源音频", "" },
        { "WAV (.wav)", ".wav" },
        { "MP3 (.mp3)", ".mp3" },
        { "FLAC (.flac)", ".flac" },
        { "OGG/Vorbis (.ogg)", ".ogg" },
        { "M4A/AAC (.m4a)", ".m4a" },
        { "Opus (.opus)", ".opus" },
        { "AAC ADTS (.aac)", ".aac" },
    } };
    // 引用生命周期覆盖整个进程，不产生每帧选项表复制。
    return options;
}

/// @brief 谱面倍速制作后台进度消息。
/// @note 通过无锁队列从工作线程传递到 UI 线程。
struct SpeedExportProgressPayload {
    /// @brief 0 到 1 的进度。
    /// @note UI 消费时仍会钳制，防御底层服务的越界报告。
    float progress{ 0.0f };

    /// @brief 状态文本。
    /// @note 消费后移动到弹窗状态，避免重复分配长消息。
    std::string message;
};

/// @brief 谱面倍速制作后台结果消息。
/// @details 携带接入当前项目所需的完整数据，避免工作线程访问 UI 会话。
struct SpeedExportResultPayload {
    /// @brief 是否成功。
    /// @note 失败时其余路径和谱面字段仅用于诊断，不进入项目。
    bool success{ false };

    /// @brief 结果消息。
    /// @note 既用于弹窗文本，也用于全局状态提示。
    std::string message;

    /// @brief 新谱面绝对路径。
    /// @note 后台保存完成后由 UI 线程据此同步项目文件。
    std::filesystem::path mapPath;

    /// @brief 新音频绝对路径。
    /// @note 用于失效音频缓存并登记项目资源。
    std::filesystem::path audioPath;

    /// @brief 新谱面对象。
    /// @note 跨线程共享所有权保证队列消费前对象保持存活。
    std::shared_ptr<BeatMap> beatmap;

    /// @brief 新谱面显示名。
    /// @note 创建编辑会话时沿用用户在弹窗中确认的名称。
    std::string displayName;

    /// @brief 新音频资源沿用的项目资源类型。
    /// @note 主音轨与音效轨的注册流程不同，不能只复制路径。
    AudioTrackType audioTrackType{ AudioTrackType::Main };

    /// @brief 新音频资源沿用的资源级处理配置。
    /// @note 保留源音频增益等项目侧设置。
    AudioTrackConfig audioTrackConfig;
};

/// @brief 获取谱面倍速制作进度队列。
/// @return 进程内所有倍速导出任务共用的多生产者队列。
/// @note 当前动作限制单任务运行，静态队列仍便于后台回调脱离对象生命周期。
moodycamel::ConcurrentQueue<SpeedExportProgressPayload>&
speedExportProgressQueue()
{
    // 函数静态对象避免跨翻译单元初始化顺序依赖。
    static moodycamel::ConcurrentQueue<SpeedExportProgressPayload> queue;
    return queue;
}

/// @brief 获取谱面倍速制作结果队列。
/// @return 进程内倍速导出结果队列。
/// @note 结果只由主菜单 update 消费，项目修改始终留在 UI 线程。
moodycamel::ConcurrentQueue<SpeedExportResultPayload>& speedExportResultQueue()
{
    // 队列独立于动作实例存活，后台任务完成时无需捕获 this。
    static moodycamel::ConcurrentQueue<SpeedExportResultPayload> queue;
    return queue;
}

/// @brief 判断文件名字符是否需要替换。
/// @param c 输入字符。
/// @return 需要替换时返回 true。
/// @note 同时覆盖控制字符和 Windows 禁止的文件名字符，便于跨平台打包。
bool shouldReplaceFileNameChar(char c)
{
    // 转为 unsigned char 后再调用 cctype，避免负 char 导致未定义行为。
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::iscntrl(uc) || c == '/' || c == '\\' || c == ':' || c == '*' ||
           c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
}

/// @brief 生成安全文件名片段。
/// @param value 原始文本。
/// @param fallback 空结果时使用的兜底名。
/// @return 可用于文件名的文本。
/// @note 仅处理文件名片段，不负责路径穿越或目录规范化。
std::string sanitizeFileNamePart(std::string value, const char* fallback)
{
    // 不合法字符逐个替换，保留其余 Unicode UTF-8 字节。
    std::replace_if(value.begin(), value.end(), shouldReplaceFileNameChar, '_');
    // 去除首部 ASCII 空格，避免生成难以辨认的文件名。
    while ( !value.empty() && value.front() == ' ' ) {
        value.erase(value.begin());
    }
    // 尾部空格在部分文件系统上会被折叠，必须主动移除。
    while ( !value.empty() && value.back() == ' ' ) {
        value.pop_back();
    }
    // 全空或全被清理时使用调用方提供的稳定兜底名。
    if ( value.empty() ) return fallback;
    return value;
}

/// @brief 生成倍速文件名片段。
/// @param speed 倍速。
/// @return 文件名片段。
/// @note 小数点替换为下划线，避免与最终文件扩展名混淆。
std::string makeSpeedToken(double speed)
{
    // 三位小数覆盖 UI 提供的精度，并限制缓冲区大小。
    char buffer[32] = { 0 };
    std::snprintf(buffer, sizeof(buffer), "%.3fx", speed);
    std::string token = buffer;
    // 从 x 前向左删除无意义的末尾零。
    while ( token.size() > 2 && token[token.size() - 2] == '0' ) {
        token.erase(token.size() - 2, 1);
    }
    if ( token.size() > 2 && token[token.size() - 2] == '.' ) {
        // 整数倍率移除残留小数点。
        token.erase(token.size() - 2, 1);
    }
    std::replace(token.begin(), token.end(), '.', '_');
    return token;
}

/// @brief 生成倍速显示文本。
/// @param speed 倍速。
/// @return 显示文本。
/// @note 与文件名 token 使用相同精度，但保留小数点供用户阅读。
std::string makeSpeedLabel(double speed)
{
    // 固定三位后再裁剪可避免依赖区域设置的流格式化状态。
    char buffer[32] = { 0 };
    std::snprintf(buffer, sizeof(buffer), "%.3fx", speed);
    std::string label = buffer;
    // 仅裁剪单位 x 之前的小数尾零。
    while ( label.size() > 2 && label[label.size() - 2] == '0' ) {
        label.erase(label.size() - 2, 1);
    }
    if ( label.size() > 2 && label[label.size() - 2] == '.' ) {
        // 整数倍率不显示悬空的小数点。
        label.erase(label.size() - 2, 1);
    }
    return label;
}

/// @brief 去除首尾空格。
/// @param value 输入文本。
/// @return 去除首尾空格后的文本。
/// @note 只处理 ASCII 空格，与 ImGui 单行名称输入的约束保持一致。
std::string trimSpaces(std::string value)
{
    // 逐次删除可保持中间空格和 UTF-8 内容不变。
    while ( !value.empty() && value.front() == ' ' ) {
        value.erase(value.begin());
    }
    while ( !value.empty() && value.back() == ' ' ) {
        value.pop_back();
    }
    return value;
}

/// @brief 判断文本片段是否为 1.2x 这类倍速尾缀。
/// @param token 文本片段。
/// @return 是倍速尾缀时返回 true。
/// @note 数值必须完整消费、有限且为正，避免误删普通名称尾词。
bool isSpeedSuffixToken(std::string_view token)
{
    // 最短形式为数字加 x；单位字符必须位于末尾。
    if ( token.size() < 2 || token.back() != 'x' ) return false;
    std::string  number(token.substr(0, token.size() - 1));
    char*        endPtr = nullptr;
    const double value  = std::strtod(number.c_str(), &endPtr);
    // strtod 的结束指针用于拒绝部分可解析文本。
    return endPtr == number.c_str() + number.size() && value > 0.0 &&
           std::isfinite(value);
}

/// @brief 去掉谱面名末尾已有的倍速尾缀。
/// @param value 原谱面名。
/// @return 去掉尾部倍速后的谱面名。
/// @note 仅检查最后一个空格分隔片段，不会修改名称中间的倍率文本。
std::string stripTrailingSpeedSuffix(std::string value)
{
    // 先规整边缘空格，使最后片段边界稳定。
    value                = trimSpaces(std::move(value));
    const auto lastSpace = value.find_last_of(' ');
    if ( lastSpace == std::string::npos ) {
        // 名称仅由倍率构成时回退到可识别的默认谱面名。
        return isSpeedSuffixToken(value) ? std::string("Speed Beatmap") : value;
    }

    const std::string_view suffix(value.data() + lastSpace + 1,
                                  value.size() - lastSpace - 1);
    if ( !isSpeedSuffixToken(suffix) ) return value;
    // 删除空格及其后的旧倍率，再次清理新的末尾空格。
    value.erase(lastSpace);
    value = trimSpaces(std::move(value));
    return value.empty() ? std::string("Speed Beatmap") : value;
}

/// @brief 获取谱面默认显示名。
/// @param beatmap 当前谱面。
/// @return 默认显示名。
/// @note 难度版本优先于歌曲名，保持导出副本在项目列表中的辨识度。
std::string beatmapDisplayName(const BeatMap& beatmap)
{
    // version 通常对应当前谱面难度，是最精确的副本基名。
    if ( !beatmap.m_baseMapMetadata.version.empty() ) {
        return beatmap.m_baseMapMetadata.version;
    }
    if ( !beatmap.m_baseMapMetadata.name.empty() ) {
        // 缺少难度名时退回歌曲或谱面名称。
        return beatmap.m_baseMapMetadata.name;
    }
    return "Speed Beatmap";
}

/// @brief 生成谱面倍速制作默认输出名称。
/// @param speed 当前倍率。
/// @return 默认输出名称。
/// @warning UI 低频路径：会获取会话递归互斥锁读取当前谱面。
/// @note 名称会先移除已有倍率尾缀，避免反复打开弹窗叠加多个倍率。
std::string buildSpeedExportAutoName(double speed)
{
    // 无活跃谱面时仍提供稳定默认名称，允许调用方安全展示。
    std::string baseName = "Speed Beatmap";
    auto&       engine   = Logic::EditorEngine::instance();
    // 在锁内同时读取会话与谱面元数据，防止切换谱面时悬空访问。
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( session && session->getContext().currentBeatmap ) {
        // 只复制名称数据，不把会话对象带出锁保护范围。
        baseName = stripTrailingSpeedSuffix(
            beatmapDisplayName(*session->getContext().currentBeatmap));
    }
    // 空格分隔使下一次 stripTrailingSpeedSuffix 能精确识别尾缀。
    return baseName + " " + makeSpeedLabel(speed);
}

/// @brief 生成不冲突的项目根目录文件路径。
/// @param projectRoot 项目根目录。
/// @param stem 文件名主体。
/// @param extension 扩展名。
/// @return 可写入的绝对路径。
/// @note 冲突时追加递增数字，不覆盖项目内已有谱面或音频。
std::filesystem::path makeUniqueProjectFilePath(
    const std::filesystem::path& projectRoot, const std::string& stem,
    const std::string& extension)
{
    // 初始候选直接位于项目根目录，stem 已由调用方清理。
    std::filesystem::path candidate =
        projectRoot / Config::utf8ToPath(stem + extension);
    std::error_code filesystemError;
    int             suffix = 1;
    // 文件存在且查询无错误时持续尝试后缀；查询错误时停止猜测。
    while ( std::filesystem::exists(candidate, filesystemError) &&
            !filesystemError ) {
        candidate = projectRoot /
                    Config::utf8ToPath(stem + "_" + std::to_string(suffix++) +
                                       extension);
    }
    // 词法规范化不访问文件系统，也不会解析符号链接。
    return candidate.lexically_normal();
}

/// @brief 获取倍速导出的音频扩展名。
/// @param inputAudioPath 输入音频路径。
/// @param selectedFormatIndex 用户选择的格式索引。
/// @return 优先沿用源音频扩展名；缺失时使用 wav。
/// @note 显式格式选择优先于源文件，索引异常按跟随源格式处理。
std::string getSpeedExportAudioExtension(
    const std::filesystem::path& inputAudioPath, int selectedFormatIndex)
{
    // 只有非零且在表内的索引代表显式编码格式。
    const auto& formatOptions = speedExportAudioFormatOptions();
    if ( selectedFormatIndex > 0 &&
         selectedFormatIndex < static_cast<int>(formatOptions.size()) ) {
        return formatOptions[static_cast<std::size_t>(selectedFormatIndex)]
            .extension;
    }

    // 跟随源格式时从路径提取扩展名并统一为小写。
    std::string extension = Config::pathToUtf8(inputAudioPath.extension());
    if ( extension.empty() ) {
        // 无扩展名的源文件使用导出服务最稳妥的 WAV 默认值。
        return ".wav";
    }
    if ( extension.front() != '.' ) {
        // 防御非标准 path 实现或人工构造值，确保扩展名包含点号。
        extension.insert(extension.begin(), '.');
    }
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

/// @brief 将绝对路径转换为项目相对路径。
/// @param projectRoot 项目根目录。
/// @param path 绝对路径。
/// @return 项目相对路径；失败时退回文件名。
/// @note 返回路径仅用于项目数据引用，不承担目标路径安全检查。
std::filesystem::path makeProjectRelativePath(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    std::error_code filesystemError;
    // 使用 error_code 避免文件系统异常跨越禁异常代码边界。
    auto relativePath =
        std::filesystem::relative(path, projectRoot, filesystemError);
    if ( !filesystemError && !relativePath.empty() ) {
        // 规范化点段，保证序列化结果稳定。
        return relativePath.lexically_normal();
    }
    // 不同根或查询失败时至少保留文件名，避免写入绝对路径。
    return path.filename();
}

/// @brief 解析项目音频资源绝对路径。
/// @param project 当前项目。
/// @param resource 当前音频时间线引用的项目资源。
/// @return 音频资源绝对路径。
/// @note 已是绝对路径时保持其根，项目相对路径则以项目根目录解析。
std::filesystem::path resolveProjectAudioPath(const Project&       project,
                                              const AudioResource& resource)
{
    // 资源路径按 UTF-8 项目协议转换为本机路径。
    const auto audioPath = Config::utf8ToPath(resource.m_path);
    if ( audioPath.empty() || audioPath.is_absolute() ) {
        // 空路径留给上层统一报告缺失资源。
        return audioPath.lexically_normal();
    }
    return (project.m_projectRoot / audioPath).lexically_normal();
}

/// @brief 获取单文件倍速导出无法表示当前音频时间线时的提示。
/// @param issue 单文件兼容性问题。
/// @return 面向用户的明确拒绝原因。
/// @note 每个枚举分支说明为何单个倍速音频无法保持原时间线语义。
const char* singleAudioTimelineIssueMessage(
    Common::SingleAudioTimelineIssue issue)
{
    // 文案与兼容性分析结果一一对应，避免笼统提示掩盖数据条件。
    switch ( issue ) {
    case Common::SingleAudioTimelineIssue::MissingSample:
        return "当前谱面没有可导出的自动采样";
    case Common::SingleAudioTimelineIssue::CompositeTimeline:
        return "当前谱面包含多个自动采样，倍速导出暂不支持复合音频时间线";
    case Common::SingleAudioTimelineIssue::NonZeroStart:
        return "当前谱面的唯一自动采样并非零点起播，倍速导出暂不支持该音频时间"
               "线";
    case Common::SingleAudioTimelineIssue::MissingResource:
        return "当前谱面的自动采样引用了缺失的项目音频资源";
    case Common::SingleAudioTimelineIssue::None: break;
    }
    // 防御未来新增枚举未同步文案时的统一回退。
    return "当前谱面的音频时间线无法由单文件倍速导出表示";
}

/// @brief 将文本复制到 ImGui 输入缓存。
/// @param buffer 输入缓存。
/// @param value 文本。
/// @note 始终先清零，确保过长文本截断后仍以 NUL 结尾。
void copyToInputBuffer(std::array<char, 192>& buffer, const std::string& value)
{
    // 固定容量与 ImGui InputText 的可写缓冲区契约一致。
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
}

/// @brief 打开谱面倍速制作窗口动作。
/// @details 负责采集导出参数、创建谱面副本、调度后台音频处理，并在
/// UI 线程把完成结果登记回当前项目。后台任务不直接访问编辑器会话。
/// @warning 动作实例及成员仅归 UI 线程使用，跨线程通信必须经过静态队列。
/// @note 单实例只允许一个导出任务，避免进度和最终结果缺少任务标识时串扰。
class OpenBeatmapSpeedExportAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在存在活跃谱面且未运行倍速任务时允许打开。
    /// @param context 单帧主菜单上下文。
    /// @return 满足谱面与任务状态条件时返回 true。
    /// @warning UI 热路径：每帧查询，不执行文件系统或后台工作。
    bool isEnabled(const MainMenuContext& context) const override
    {
        // 启用条件来自编辑器全局状态，不需要菜单上下文字段。
        (void)context;
        return MenuUtil::hasActiveBeatmap(true) && !m_running;
    }

    /// @brief 消费谱面倍速制作后台任务消息。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧只消费无锁队列中的已完成消息。
    void update(MainMenuContext& context) override { consumeQueues(context); }

    /// @brief 打开谱面倍速制作窗口。
    /// @param context 单帧主菜单上下文。
    /// @param activation 菜单激活来源，本动作不区分来源。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        // 弹窗参数从活动会话读取，激活事件本身无附加语义。
        (void)context;
        (void)activation;
        openPopup();
    }

    /// @brief 渲染谱面倍速制作弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；只在弹窗打开时绘制控件。
    void renderDeferred(MainMenuContext& context) override
    {
        renderPopup(context.dpiScale);
    }

private:
    /// @brief 打开谱面倍速制作弹窗并重置本次交互状态。
    /// @note 正在运行的任务保留当前弹窗状态，不能被重复初始化。
    void openPopup()
    {
        // 任务期间禁止覆盖名称和进度，确保完成消息仍对应当前请求。
        if ( m_running ) {
            return;
        }

        // 新一轮交互从当前谱面和倍率重新生成默认名称。
        m_autoName   = buildSpeedExportAutoName(m_factor);
        m_nameEdited = false;
        copyToInputBuffer(m_nameBuffer, m_autoName);
        // 清除上次结果，使弹窗初始状态只展示本次参数。
        m_progress = 0.0f;
        m_status.clear();
        m_showPopup = true;
    }

    /// @brief 启动谱面倍速制作后台任务。
    /// @warning UI 低频路径：启动阶段会锁定会话、检查文件并创建谱面副本。
    /// @note 文件编码和谱面保存转移到后台任务，项目登记留在 UI 线程。
    void startExport()
    {
        // 单实例只允许一个未完成任务，避免静态消息队列结果串扰。
        if ( m_running ) {
            return;
        }

        // 项目必须在任务准备前存在，后台任务不会延长 Project 生命周期。
        auto& engine  = Logic::EditorEngine::instance();
        auto* project = engine.getCurrentProject();
        if ( !project ) {
            // 保持弹窗打开，让用户看到当前状态而非静默失败。
            m_status = "当前没有打开的项目";
            return;
        }

        // 以下值均在锁内构造，随后按值捕获给后台任务。
        std::shared_ptr<BeatMap> outputBeatmap;
        std::filesystem::path    inputAudioPath;
        std::filesystem::path    outputAudioPath;
        std::filesystem::path    outputMapPath;
        std::string              displayName;
        AudioTrackType           sourceAudioTrackType{ AudioTrackType::Main };
        AudioTrackConfig         sourceAudioTrackConfig;
        // 即使 UI 状态被外部恢复为异常值，也在任务边界再次钳制倍率。
        const double speed =
            std::clamp(static_cast<double>(m_factor), 0.1, 4.0);
        const bool preservePitch    = m_preservePitch;
        const int  audioFormatIndex = m_audioFormatIndex;

        {
            // 同步 ECS 与读取当前谱面必须共享同一会话锁。
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();
            if ( !session ) {
                // 项目存在不保证已经打开某张谱面。
                m_status = "当前没有打开的谱面";
                return;
            }

            // 导出前把编辑中的 ECS 状态写回谱面对象，确保副本包含最新修改。
            auto& ctx = session->getContextMutable();
            Logic::SessionUtils::syncBeatmap(ctx);
            if ( !ctx.currentBeatmap ) {
                // 会话上下文缺少谱面对象时不能构造转换输入。
                m_status = "当前没有打开的谱面";
                return;
            }

            // 单文件导出只能表示零点开始的一条自动采样时间线。
            const auto& sourceBeatmap = *ctx.currentBeatmap;
            const auto  timelineSource =
                Common::resolveSingleZeroPointAudioTimeline(sourceBeatmap,
                                                            *project);
            if ( !timelineSource ) {
                // 将结构化兼容性问题映射为精确用户提示。
                m_status =
                    singleAudioTimelineIssueMessage(timelineSource.m_issue);
                return;
            }

            // 路径解析仍在项目有效期间完成，后台只接收独立 path 值。
            inputAudioPath =
                resolveProjectAudioPath(*project, *timelineSource.m_resource);
            sourceAudioTrackType   = timelineSource.m_resource->m_type;
            sourceAudioTrackConfig = timelineSource.m_resource->m_config;
            // 启动前验证输入是普通文件，避免把编码失败延迟到后台。
            std::error_code filesystemError;
            if ( inputAudioPath.empty() ||
                 !std::filesystem::is_regular_file(inputAudioPath,
                                                   filesystemError) ||
                 filesystemError ) {
                // error_code 同样视为不可读取，避免误报为可用资源。
                m_status = "当前谱面的自动采样音频文件不存在";
                return;
            }

            // 未手动修改时按最终钳制倍率刷新自动名称。
            displayName = trimSpaces(m_nameBuffer.data());
            if ( displayName.empty() || !m_nameEdited ) {
                // 空手动输入同样回退自动名，禁止生成空元数据。
                displayName = buildSpeedExportAutoName(speed);
            }

            // 显示名保留原文本，文件名使用跨平台安全副本。
            const std::string safeName =
                sanitizeFileNamePart(displayName, "SpeedBeatmap");
            const std::string speedToken = makeSpeedToken(speed);
            // 音高策略进入文件名，避免相同倍率的两类结果难以区分。
            const std::string pitchToken =
                preservePitch ? "_keep_pitch" : "_pitch";
            // 谱面和音频分别查重，任何已有文件都不会被覆盖。
            outputMapPath = makeUniqueProjectFilePath(
                project->m_projectRoot, safeName + "_" + speedToken, ".mmm");

            // 音频名优先沿用源 stem，以便在项目资源列表中追溯来源。
            const std::string audioStem = sanitizeFileNamePart(
                Config::pathToUtf8(inputAudioPath.stem()), safeName.c_str());
            outputAudioPath = makeUniqueProjectFilePath(
                project->m_projectRoot,
                audioStem + "_" + speedToken + pitchToken,
                getSpeedExportAudioExtension(inputAudioPath, audioFormatIndex));

            // 转换器接收项目相对路径，使生成谱面可以随项目目录移动。
            MMM::BeatmapSpeedTransformOptions transformOptions;
            transformOptions.speed = speed;
            transformOptions.mapPath =
                makeProjectRelativePath(project->m_projectRoot, outputMapPath);
            transformOptions.audioPath = makeProjectRelativePath(
                project->m_projectRoot, outputAudioPath);
            transformOptions.name    = displayName;
            transformOptions.version = displayName;

            // 谱面时间轴转换在锁内同步完成，失败时不启动音频任务。
            auto transformResult =
                MMM::BeatmapSpeedTransform::createSpeedVersion(
                    sourceBeatmap, transformOptions);
            if ( !transformResult.success ) {
                // 优先展示转换器提供的具体数据错误。
                m_status = transformResult.errorMessage.empty()
                               ? "谱面倍速副本生成失败"
                               : transformResult.errorMessage;
                return;
            }

            // 共享所有权跨越后台保存与 UI 消费两个阶段。
            outputBeatmap =
                std::make_shared<BeatMap>(std::move(transformResult.beatmap));
            if ( outputBeatmap->m_audioSamples.size() != 1U ) {
                // 单文件兼容性承诺要求转换后仍严格只有一个自动采样。
                m_status = "倍速副本未能保留唯一自动采样";
                return;
            }
            // 重写新谱面的采样引用，使其指向即将生成的音频资源。
            const auto outputAudioRelativePath = makeProjectRelativePath(
                project->m_projectRoot, outputAudioPath);
            outputBeatmap->m_audioSamples.front().m_audioResourceId =
                Config::pathToUtf8(outputAudioRelativePath.filename());
            outputBeatmap->m_baseMapMetadata.song_file_hint =
                outputAudioRelativePath;
            // 清除旧版主音频字段，避免与统一资源时间线产生双重引用。
            outputBeatmap->m_baseMapMetadata.main_audio_path.clear();
        }

        // 准备成功后才切换运行状态，前置失败仍允许用户修改参数重试。
        m_running  = true;
        m_progress = 0.0f;
        m_status   = "正在准备倍速音频...";

        // 后台闭包只捕获值和共享谱面，不捕获动作或项目裸指针。
        auto task = [inputAudioPath,
                     outputAudioPath,
                     outputMapPath,
                     outputBeatmap,
                     displayName,
                     sourceAudioTrackType,
                     sourceAudioTrackConfig,
                     speed,
                     preservePitch]() {
            // 结果载荷预先保存接入项目所需上下文，所有出口共用同一结构。
            SpeedExportResultPayload payload;
            payload.mapPath          = outputMapPath;
            payload.audioPath        = outputAudioPath;
            payload.beatmap          = outputBeatmap;
            payload.displayName      = displayName;
            payload.audioTrackType   = sourceAudioTrackType;
            payload.audioTrackConfig = sourceAudioTrackConfig;

            // 音频服务负责解码、变速、可选音高保持和目标格式编码。
            Audio::AudioSpeedExportOptions audioOptions;
            audioOptions.inputPath     = inputAudioPath;
            audioOptions.outputPath    = outputAudioPath;
            audioOptions.speed         = speed;
            audioOptions.preservePitch = preservePitch;
            if ( outputBeatmap ) {
                // 音频不能短于变速后最后一个谱面物件，防止尾部被截断。
                audioOptions.minimumDurationSeconds =
                    std::max(0.0,
                             BeatmapSpeedTransform::calculateContentEndTime(
                                 *outputBeatmap) /
                                 1000.0);
            }
            // 进度回调只入队，不从工作线程触碰 ImGui 或动作成员。
            audioOptions.progressCallback =
                [](const Audio::AudioSpeedExportProgress& progress) {
                    speedExportProgressQueue().enqueue(
                        SpeedExportProgressPayload{ progress.progress,
                                                    progress.message });
                };

            // 输出格式由目标扩展名决定，服务入口名称保留历史兼容命名。
            const auto audioResult =
                Audio::AudioSpeedExportService::exportWav(audioOptions);
            if ( !audioResult.success ) {
                // 音频失败时不保存谱面，避免产生引用缺失文件的副本。
                payload.success = false;
                payload.message = audioResult.errorMessage.empty()
                                      ? "倍速音频导出失败"
                                      : audioResult.errorMessage;
                // 移交最终结果后立即结束后台任务。
                speedExportResultQueue().enqueue(std::move(payload));
                return;
            }

            if ( outputBeatmap && audioResult.outputDurationSeconds > 0.0 &&
                 std::isfinite(audioResult.outputDurationSeconds) ) {
                // 元数据长度至少覆盖实际音频和谱面内容两者中的较长者。
                const double contentEndMs =
                    BeatmapSpeedTransform::calculateContentEndTime(
                        *outputBeatmap);
                outputBeatmap->m_baseMapMetadata.map_length = std::max(
                    audioResult.outputDurationSeconds * 1000.0, contentEndMs);
            }

            // 音频完成后预留末段进度给谱面序列化。
            speedExportProgressQueue().enqueue(
                SpeedExportProgressPayload{ 0.98f, "正在保存倍速谱面..." });
            if ( !outputBeatmap || !outputBeatmap->saveToFile(outputMapPath) ) {
                // 谱面保存失败时音频可能已生成，消息明确任务整体未完成。
                payload.success = false;
                payload.message = "倍速谱面保存失败";
                speedExportResultQueue().enqueue(std::move(payload));
                return;
            }

            // 只有音频与谱面都落盘后才报告成功。
            payload.success = true;
            payload.message = "谱面倍速制作完成";
            speedExportResultQueue().enqueue(std::move(payload));
        };

        // 优先使用应用线程池，保持 UI 响应并复用统一后台生命周期。
        auto* threadPool = Runtime::AppThreadPool::instance().get();
        if ( threadPool ) {
            threadPool->enqueue_void(std::move(task));
        } else {
            // 无线程池仅作为启动期或测试环境兜底，同步执行保持功能可用。
            task();
        }
    }

    /// @brief 消费谱面倍速制作后台任务消息。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧只消费无锁队列中的已完成消息。
    /// @note 项目、音频管理器和编辑会话的修改全部在此主线程入口完成。
    void consumeQueues(MainMenuContext& context)
    {
        // 排空进度队列，只保留最新值，避免低帧率时积压旧状态。
        SpeedExportProgressPayload progress;
        while ( speedExportProgressQueue().try_dequeue(progress) ) {
            // 后台服务值在 UI 边界钳制，状态文本以移动方式接管。
            m_progress = std::clamp(progress.progress, 0.0f, 1.0f);
            m_status   = std::move(progress.message);
        }

        // 结果队列通常只有一项，循环可清理异常残留并恢复动作状态。
        SpeedExportResultPayload result;
        while ( speedExportResultQueue().try_dequeue(result) ) {
            // 任一最终结果都结束运行态；失败将进度复位以免误示完成。
            m_running  = false;
            m_progress = result.success ? 1.0f : 0.0f;
            m_status   = result.message;

            if ( result.success && result.beatmap ) {
                // 新文件已由后台落盘，先失效旧缓存再登记项目资源。
                auto& engine = Logic::EditorEngine::instance();
                Audio::AudioManager::instance().invalidateTrackCache(
                    Config::pathToUtf8(result.audioPath));
                // 统一走编辑器导入入口，复用资源 ID 与相对路径生成规则。
                engine.handleImportAudio(
                    Logic::CmdImportAudio{ Config::pathToUtf8(result.audioPath),
                                           result.audioTrackType });

                // 后台期间用户可能关闭项目，消费时必须重新验证生命周期。
                auto* project = engine.getCurrentProject();
                if ( !project ) {
                    // 已生成文件保留在磁盘，仅跳过无法完成的项目接入步骤。
                    result.message = "倍速音频已生成，但当前项目已关闭";
                    m_status       = result.message;
                    context.statusMessageSink.showStatusMessage(result.message,
                                                                4.0F);
                    // 继续消费可能存在的后续消息，不关闭当前弹窗。
                    continue;
                }

                // 导入入口完成后按相对路径或文件名 ID 找回新增资源。
                const std::string relativeAudioPath =
                    Config::pathToUtf8(makeProjectRelativePath(
                        project->m_projectRoot, result.audioPath));
                const std::string outputResourceId =
                    Config::pathToUtf8(result.audioPath.filename());
                const auto resource = std::find_if(
                    project->m_audioResources.begin(),
                    project->m_audioResources.end(),
                    [&](const AudioResource& candidate) {
                        // 同时兼容路径键和历史文件名资源 ID。
                        return candidate.m_path == relativeAudioPath ||
                               candidate.m_id == outputResourceId;
                    });
                if ( resource == project->m_audioResources.end() ) {
                    // 文件成功但资源登记缺失时不创建引用不完整的新会话。
                    result.message = "倍速音频已生成，但项目音频资源登记失败";
                    m_status       = result.message;
                    context.statusMessageSink.showStatusMessage(result.message,
                                                                4.0F);
                    continue;
                }

                // 导入只建立基础资源，随后恢复源资源的处理配置。
                resource->m_config = result.audioTrackConfig;
                if ( resource->m_type == AudioTrackType::Effect ) {
                    // 音效轨需要额外登记到即时播放缓存，主音轨由会话加载。
                    Audio::AudioManager::instance().registerSoundEffect(
                        resource->m_id,
                        Config::pathToUtf8(result.audioPath),
                        resource->m_config);
                }
                // 先持久化资源清单，再同步新谱面并创建活动编辑会话。
                engine.saveProject();
                engine.syncProjectWithFile(result.mapPath);
                engine.createSession(result.beatmap, result.displayName);

                // 全局状态条提供弹窗关闭后的完成反馈。
                context.statusMessageSink.showStatusMessage(result.message,
                                                            3.0f);
                m_showPopup = false;
            } else {
                // 失败既写日志供诊断，也保留面向用户的状态提示。
                XERROR("Beatmap speed export failed: {}", result.message);
                context.statusMessageSink.showStatusMessage(
                    result.message.empty() ? "谱面倍速制作失败"
                                           : result.message,
                    4.0f);
            }
        }
    }

    /// @brief 渲染谱面倍速制作弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：每帧执行；只在弹窗打开时绘制控件。
    /// @note 任务运行期间参数区和启动按钮禁用，但进度与关闭条件持续刷新。
    void renderPopup(float dpiScale)
    {
        // ### 后缀固定 ImGui 内部 ID，中文标题变化不会破坏弹窗状态。
        constexpr const char* popupId =
            "谱面倍速制作###BeatmapSpeedExportModal";
        if ( m_showPopup ) {
            // 标志只触发一次 OpenPopup，后续可见性由 ImGui 弹窗栈维护。
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showPopup = false;
        }

        // 宽度按 DPI 缩放并取整，避免亚像素边缘模糊。
        const float popupWidth = std::floor(520.0f * dpiScale);
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( !modalScope.begin(popupId,
                               nullptr,
                               ImGuiWindowFlags_None,
                               ImVec2(popupWidth, 0.0f)) ) {
            // 弹窗未打开时不执行任何控件状态更新。
            return;
        }

        // 后台任务期间冻结所有会改变捕获参数的控件。
        ImGui::BeginDisabled(m_running);
        const auto refreshAutoNameIfNeeded = [this]() {
            // 仅未手动编辑的名称跟随倍率自动变化。
            const std::string autoName = buildSpeedExportAutoName(m_factor);
            if ( !m_nameEdited && autoName != m_autoName ) {
                copyToInputBuffer(m_nameBuffer, autoName);
            }
            // 无论是否覆盖输入，都更新比较基准供下一帧判断。
            m_autoName = autoName;
        };
        refreshAutoNameIfNeeded();

        ImGui::TextUnformatted("输出名称");
        ImGui::SetNextItemWidth(-1.0f);
        if ( ImGui::InputText("##SpeedExportName",
                              m_nameBuffer.data(),
                              m_nameBuffer.size()) ) {
            // 回到自动名称时恢复跟随行为，不永久标记为手动值。
            m_nameEdited = trimSpaces(m_nameBuffer.data()) != m_autoName;
        }

        ImGui::TextUnformatted("倍速");
        ImGui::SetNextItemWidth(-1.0f);
        if ( ::MMM::UI::FeedbackSliderFloat(
                 "##SpeedExportFactor", &m_factor, 0.25f, 4.0f, "%.4fx") ) {
            // 控件边界之外再次钳制，防御键盘输入产生越界值。
            m_factor = std::clamp(m_factor, 0.25f, 4.0f);
            refreshAutoNameIfNeeded();
        }

        // 四个快捷倍率均分可用宽度，并扣除三个控件间距。
        const ImGuiStyle& style        = ImGui::GetStyle();
        const float       contentWidth = ImGui::GetContentRegionAvail().x;
        const float       buttonW =
            std::floor((contentWidth - style.ItemSpacing.x * 3.0f) / 4.0f);
        if ( ::MMM::UI::FeedbackButton("0.75x", ImVec2(buttonW, 0.0f)) ) {
            // 每个快捷按钮都复用自动名称刷新规则。
            m_factor = 0.75f;
            refreshAutoNameIfNeeded();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("1.2x", ImVec2(buttonW, 0.0f)) ) {
            m_factor = 1.2f;
            refreshAutoNameIfNeeded();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("1.5x", ImVec2(buttonW, 0.0f)) ) {
            m_factor = 1.5f;
            refreshAutoNameIfNeeded();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("2x", ImVec2(buttonW, 0.0f)) ) {
            m_factor = 2.0f;
            refreshAutoNameIfNeeded();
        }

        ImGui::TextUnformatted("音频音高");
        if ( ::MMM::UI::FeedbackRadioButton("保留原音高", m_preservePitch) ) {
            m_preservePitch = true;
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackRadioButton("随倍速变调", !m_preservePitch) ) {
            m_preservePitch = false;
        }

        // 格式索引在绘制前校验，防止未来选项表调整导致越界。
        const auto& formatOptions = speedExportAudioFormatOptions();
        if ( m_audioFormatIndex < 0 ||
             m_audioFormatIndex >= static_cast<int>(formatOptions.size()) ) {
            m_audioFormatIndex = 0;
        }
        ImGui::TextUnformatted("音频格式");
        ImGui::SetNextItemWidth(-1.0f);
        if ( ::MMM::UI::FeedbackBeginCombo(
                 "##SpeedExportAudioFormat",
                 formatOptions[static_cast<std::size_t>(m_audioFormatIndex)]
                     .label) ) {
            // 使用表顺序绘制所有编码格式，并保持单选状态。
            for ( std::size_t index = 0; index < formatOptions.size();
                  ++index ) {
                const bool selected =
                    m_audioFormatIndex == static_cast<int>(index);
                if ( ::MMM::UI::FeedbackSelectable(formatOptions[index].label,
                                                   selected) ) {
                    m_audioFormatIndex = static_cast<int>(index);
                }
                if ( selected ) {
                    // 弹窗打开组合框时键盘焦点落在当前格式。
                    ImGui::SetItemDefaultFocus();
                }
            }
            ::MMM::UI::FeedbackEndCombo();
        }
        ImGui::EndDisabled();

        // 初始空闲状态隐藏进度区域，任务开始后持续保留最后消息。
        if ( m_running || !m_status.empty() ) {
            ImGui::Spacing();
            ImGui::ProgressBar(m_progress, ImVec2(-1.0f, 0.0f), nullptr);
            ImGui::TextWrapped("%s", m_status.c_str());
        }

        ImGui::Separator();
        // 底部两个动作按钮等宽排列。
        const float actionButtonWidth = std::floor(
            (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.0f);
        const ImVec2 actionButtonSize(actionButtonWidth, 0.0f);
        ImGui::BeginDisabled(m_running);
        if ( ::MMM::UI::FeedbackButton("开始制作", actionButtonSize) ) {
            // startExport 自身还会复核任务和项目状态。
            startExport();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("关闭", actionButtonSize) &&
             !m_running ) {
            // 任务进行中禁止关闭，保证用户始终能观察完成或失败反馈。
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    /// @brief 是否显示谱面倍速制作弹窗。
    /// @note 该标志只请求下一帧打开弹窗，调用后立即复位。
    /// @warning 仅由 UI 线程读写，后台任务不得捕获其地址。
    bool m_showPopup = false;

    /// @brief 谱面倍速制作后台任务是否运行中。
    /// @note 从任务入队前保持为 true，直到 UI 线程消费最终结果。
    /// @warning 仅由 UI 线程读写，跨线程完成信号通过结果队列传递。
    bool m_running = false;

    /// @brief 谱面倍速制作倍率。
    /// @note UI 范围为 0.25 到 4.0，任务边界另行钳制到服务安全范围。
    /// @warning 启动任务时复制到闭包，后台不读取此成员。
    float m_factor = 1.2f;

    /// @brief 谱面倍速音频是否保留原音高。
    /// @note false 表示音高随播放速度同比变化。
    bool m_preservePitch = true;

    /// @brief 谱面倍速音频输出格式索引；0 表示跟随源音频。
    /// @note 索引对应 speedExportAudioFormatOptions 的稳定顺序。
    int m_audioFormatIndex = 0;

    /// @brief 输出名称是否已被用户手动编辑。
    /// @note false 时倍率变化会自动重建名称缓存。
    bool m_nameEdited = false;

    /// @brief 谱面倍速制作输出名称输入缓存。
    /// @note 固定容量避免 UI 热路径分配，并与 ImGui 可写缓冲协议匹配。
    /// @warning 只在 UI 线程传给 InputText，后台任务捕获转换后的字符串。
    std::array<char, 192> m_nameBuffer{};

    /// @brief 当前自动生成的输出名称，用于判断是否跟随倍率刷新。
    /// @note 与去除边缘空格后的输入缓存比较。
    /// @warning UI 热路径内可能在倍率变化时重新分配，但不跨线程共享。
    std::string m_autoName;

    /// @brief 谱面倍速制作进度。
    /// @note 消费后台消息时钳制到 0 到 1。
    float m_progress = 0.0f;

    /// @brief 谱面倍速制作状态文本。
    /// @note 空文本隐藏进度区域，非空文本保留最后一次任务反馈。
    /// @warning 后台线程只写队列载荷，本成员仅由 UI 消费端修改。
    std::string m_status;
};
}  // namespace

/// @brief 创建打开谱面倍速制作窗口动作处理器。
/// @return 由主菜单注册表独占持有的动作实例。
/// @note 每个实例维护一组弹窗状态，工厂不复用全局 UI 对象。
std::unique_ptr<IMainMenuItemActionHandler> createOpenBeatmapSpeedExportAction()
{
    // unique_ptr 与主菜单动作处理器的生命周期协议一致。
    return std::make_unique<OpenBeatmapSpeedExportAction>();
}

}  // namespace MMM::UI
