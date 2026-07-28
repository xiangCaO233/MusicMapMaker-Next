#define IMGUI_DEFINE_MATH_OPERATORS
#include "audio/AudioManager.h"
#include "audio/AudioSpeedExportService.h"
#include "common/BeatmapAudioTimelineCompatibility.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/SessionUtils.h"
#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "runtime/AppThreadPool.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
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
struct SpeedExportAudioFormatOption {
    /// @brief 下拉框显示名。
    const char* label;

    /// @brief 输出扩展名；空字符串表示跟随源音频。
    const char* extension;
};

/// @brief 获取倍速音频导出格式选项表。
/// @return 格式选项表。
const std::array<SpeedExportAudioFormatOption, 8>&
speedExportAudioFormatOptions()
{
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
    return options;
}

/// @brief 谱面倍速制作后台进度消息。
struct SpeedExportProgressPayload {
    /// @brief 0 到 1 的进度。
    float progress{ 0.0f };

    /// @brief 状态文本。
    std::string message;
};

/// @brief 谱面倍速制作后台结果消息。
struct SpeedExportResultPayload {
    /// @brief 是否成功。
    bool success{ false };

    /// @brief 结果消息。
    std::string message;

    /// @brief 新谱面绝对路径。
    std::filesystem::path mapPath;

    /// @brief 新音频绝对路径。
    std::filesystem::path audioPath;

    /// @brief 新谱面对象。
    std::shared_ptr<BeatMap> beatmap;

    /// @brief 新谱面显示名。
    std::string displayName;

    /// @brief 新音频资源沿用的项目资源类型。
    AudioTrackType audioTrackType{ AudioTrackType::Main };

    /// @brief 新音频资源沿用的资源级处理配置。
    AudioTrackConfig audioTrackConfig;
};

/// @brief 获取谱面倍速制作进度队列。
moodycamel::ConcurrentQueue<SpeedExportProgressPayload>&
speedExportProgressQueue()
{
    static moodycamel::ConcurrentQueue<SpeedExportProgressPayload> queue;
    return queue;
}

/// @brief 获取谱面倍速制作结果队列。
moodycamel::ConcurrentQueue<SpeedExportResultPayload>& speedExportResultQueue()
{
    static moodycamel::ConcurrentQueue<SpeedExportResultPayload> queue;
    return queue;
}

/// @brief 判断文件名字符是否需要替换。
/// @param c 输入字符。
/// @return 需要替换时返回 true。
bool shouldReplaceFileNameChar(char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::iscntrl(uc) || c == '/' || c == '\\' || c == ':' || c == '*' ||
           c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
}

/// @brief 生成安全文件名片段。
/// @param value 原始文本。
/// @param fallback 空结果时使用的兜底名。
/// @return 可用于文件名的文本。
std::string sanitizeFileNamePart(std::string value, const char* fallback)
{
    std::replace_if(value.begin(), value.end(), shouldReplaceFileNameChar, '_');
    while ( !value.empty() && value.front() == ' ' ) {
        value.erase(value.begin());
    }
    while ( !value.empty() && value.back() == ' ' ) {
        value.pop_back();
    }
    if ( value.empty() ) return fallback;
    return value;
}

/// @brief 生成倍速文件名片段。
/// @param speed 倍速。
/// @return 文件名片段。
std::string makeSpeedToken(double speed)
{
    char buffer[32] = { 0 };
    std::snprintf(buffer, sizeof(buffer), "%.3fx", speed);
    std::string token = buffer;
    while ( token.size() > 2 && token[token.size() - 2] == '0' ) {
        token.erase(token.size() - 2, 1);
    }
    if ( token.size() > 2 && token[token.size() - 2] == '.' ) {
        token.erase(token.size() - 2, 1);
    }
    std::replace(token.begin(), token.end(), '.', '_');
    return token;
}

/// @brief 生成倍速显示文本。
/// @param speed 倍速。
/// @return 显示文本。
std::string makeSpeedLabel(double speed)
{
    char buffer[32] = { 0 };
    std::snprintf(buffer, sizeof(buffer), "%.3fx", speed);
    std::string label = buffer;
    while ( label.size() > 2 && label[label.size() - 2] == '0' ) {
        label.erase(label.size() - 2, 1);
    }
    if ( label.size() > 2 && label[label.size() - 2] == '.' ) {
        label.erase(label.size() - 2, 1);
    }
    return label;
}

/// @brief 去除首尾空格。
/// @param value 输入文本。
/// @return 去除首尾空格后的文本。
std::string trimSpaces(std::string value)
{
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
bool isSpeedSuffixToken(std::string_view token)
{
    if ( token.size() < 2 || token.back() != 'x' ) return false;
    std::string  number(token.substr(0, token.size() - 1));
    char*        endPtr = nullptr;
    const double value  = std::strtod(number.c_str(), &endPtr);
    return endPtr == number.c_str() + number.size() && value > 0.0 &&
           std::isfinite(value);
}

/// @brief 去掉谱面名末尾已有的倍速尾缀。
/// @param value 原谱面名。
/// @return 去掉尾部倍速后的谱面名。
std::string stripTrailingSpeedSuffix(std::string value)
{
    value                = trimSpaces(std::move(value));
    const auto lastSpace = value.find_last_of(' ');
    if ( lastSpace == std::string::npos ) {
        return isSpeedSuffixToken(value) ? std::string("Speed Beatmap") : value;
    }

    const std::string_view suffix(value.data() + lastSpace + 1,
                                  value.size() - lastSpace - 1);
    if ( !isSpeedSuffixToken(suffix) ) return value;
    value.erase(lastSpace);
    value = trimSpaces(std::move(value));
    return value.empty() ? std::string("Speed Beatmap") : value;
}

/// @brief 获取谱面默认显示名。
/// @param beatmap 当前谱面。
/// @return 默认显示名。
std::string beatmapDisplayName(const BeatMap& beatmap)
{
    if ( !beatmap.m_baseMapMetadata.version.empty() ) {
        return beatmap.m_baseMapMetadata.version;
    }
    if ( !beatmap.m_baseMapMetadata.name.empty() ) {
        return beatmap.m_baseMapMetadata.name;
    }
    return "Speed Beatmap";
}

/// @brief 生成谱面倍速制作默认输出名称。
/// @param speed 当前倍率。
/// @return 默认输出名称。
std::string buildSpeedExportAutoName(double speed)
{
    std::string baseName = "Speed Beatmap";
    auto&       engine   = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( session && session->getContext().currentBeatmap ) {
        baseName = stripTrailingSpeedSuffix(
            beatmapDisplayName(*session->getContext().currentBeatmap));
    }
    return baseName + " " + makeSpeedLabel(speed);
}

/// @brief 生成不冲突的项目根目录文件路径。
/// @param projectRoot 项目根目录。
/// @param stem 文件名主体。
/// @param extension 扩展名。
/// @return 可写入的绝对路径。
std::filesystem::path makeUniqueProjectFilePath(
    const std::filesystem::path& projectRoot, const std::string& stem,
    const std::string& extension)
{
    std::filesystem::path candidate =
        projectRoot / Config::utf8ToPath(stem + extension);
    std::error_code filesystemError;
    int             suffix = 1;
    while ( std::filesystem::exists(candidate, filesystemError) &&
            !filesystemError ) {
        candidate = projectRoot /
                    Config::utf8ToPath(stem + "_" + std::to_string(suffix++) +
                                       extension);
    }
    return candidate.lexically_normal();
}

/// @brief 获取倍速导出的音频扩展名。
/// @param inputAudioPath 输入音频路径。
/// @param selectedFormatIndex 用户选择的格式索引。
/// @return 优先沿用源音频扩展名；缺失时使用 wav。
std::string getSpeedExportAudioExtension(
    const std::filesystem::path& inputAudioPath, int selectedFormatIndex)
{
    const auto& formatOptions = speedExportAudioFormatOptions();
    if ( selectedFormatIndex > 0 &&
         selectedFormatIndex < static_cast<int>(formatOptions.size()) ) {
        return formatOptions[static_cast<std::size_t>(selectedFormatIndex)]
            .extension;
    }

    std::string extension = Config::pathToUtf8(inputAudioPath.extension());
    if ( extension.empty() ) {
        return ".wav";
    }
    if ( extension.front() != '.' ) {
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
std::filesystem::path makeProjectRelativePath(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    std::error_code filesystemError;
    auto            relativePath =
        std::filesystem::relative(path, projectRoot, filesystemError);
    if ( !filesystemError && !relativePath.empty() ) {
        return relativePath.lexically_normal();
    }
    return path.filename();
}

/// @brief 解析项目音频资源绝对路径。
/// @param project 当前项目。
/// @param resource 当前音频时间线引用的项目资源。
/// @return 音频资源绝对路径。
std::filesystem::path resolveProjectAudioPath(const Project&       project,
                                              const AudioResource& resource)
{
    const auto audioPath = Config::utf8ToPath(resource.m_path);
    if ( audioPath.empty() || audioPath.is_absolute() ) {
        return audioPath.lexically_normal();
    }
    return (project.m_projectRoot / audioPath).lexically_normal();
}

/// @brief 获取单文件倍速导出无法表示当前音频时间线时的提示。
/// @param issue 单文件兼容性问题。
/// @return 面向用户的明确拒绝原因。
const char* singleAudioTimelineIssueMessage(
    Common::SingleAudioTimelineIssue issue)
{
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
    return "当前谱面的音频时间线无法由单文件倍速导出表示";
}

/// @brief 将文本复制到 ImGui 输入缓存。
/// @param buffer 输入缓存。
/// @param value 文本。
void copyToInputBuffer(std::array<char, 192>& buffer, const std::string& value)
{
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
}

/// @brief 打开谱面倍速制作窗口动作。
class OpenBeatmapSpeedExportAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在存在活跃谱面且未运行倍速任务时允许打开。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return MenuUtil::hasActiveBeatmap(true) && !m_running;
    }

    /// @brief 消费谱面倍速制作后台任务消息。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧只消费无锁队列中的已完成消息。
    void update(MainMenuContext& context) override { consumeQueues(context); }

    /// @brief 打开谱面倍速制作窗口。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
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
    void openPopup()
    {
        if ( m_running ) {
            return;
        }

        m_autoName   = buildSpeedExportAutoName(m_factor);
        m_nameEdited = false;
        copyToInputBuffer(m_nameBuffer, m_autoName);
        m_progress = 0.0f;
        m_status.clear();
        m_showPopup = true;
    }

    /// @brief 启动谱面倍速制作后台任务。
    void startExport()
    {
        if ( m_running ) {
            return;
        }

        auto& engine  = Logic::EditorEngine::instance();
        auto* project = engine.getCurrentProject();
        if ( !project ) {
            m_status = "当前没有打开的项目";
            return;
        }

        std::shared_ptr<BeatMap> outputBeatmap;
        std::filesystem::path    inputAudioPath;
        std::filesystem::path    outputAudioPath;
        std::filesystem::path    outputMapPath;
        std::string              displayName;
        AudioTrackType           sourceAudioTrackType{ AudioTrackType::Main };
        AudioTrackConfig         sourceAudioTrackConfig;
        const double             speed =
            std::clamp(static_cast<double>(m_factor), 0.1, 4.0);
        const bool preservePitch    = m_preservePitch;
        const int  audioFormatIndex = m_audioFormatIndex;

        {
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();
            if ( !session ) {
                m_status = "当前没有打开的谱面";
                return;
            }

            auto& ctx = session->getContextMutable();
            Logic::SessionUtils::syncBeatmap(ctx);
            if ( !ctx.currentBeatmap ) {
                m_status = "当前没有打开的谱面";
                return;
            }

            const auto& sourceBeatmap = *ctx.currentBeatmap;
            const auto  timelineSource =
                Common::resolveSingleZeroPointAudioTimeline(sourceBeatmap,
                                                            *project);
            if ( !timelineSource ) {
                m_status =
                    singleAudioTimelineIssueMessage(timelineSource.m_issue);
                return;
            }

            inputAudioPath =
                resolveProjectAudioPath(*project, *timelineSource.m_resource);
            sourceAudioTrackType   = timelineSource.m_resource->m_type;
            sourceAudioTrackConfig = timelineSource.m_resource->m_config;
            std::error_code filesystemError;
            if ( inputAudioPath.empty() ||
                 !std::filesystem::is_regular_file(inputAudioPath,
                                                   filesystemError) ||
                 filesystemError ) {
                m_status = "当前谱面的自动采样音频文件不存在";
                return;
            }

            displayName = trimSpaces(m_nameBuffer.data());
            if ( displayName.empty() || !m_nameEdited ) {
                displayName = buildSpeedExportAutoName(speed);
            }

            const std::string safeName =
                sanitizeFileNamePart(displayName, "SpeedBeatmap");
            const std::string speedToken = makeSpeedToken(speed);
            const std::string pitchToken =
                preservePitch ? "_keep_pitch" : "_pitch";
            outputMapPath = makeUniqueProjectFilePath(
                project->m_projectRoot, safeName + "_" + speedToken, ".mmm");

            const std::string audioStem = sanitizeFileNamePart(
                Config::pathToUtf8(inputAudioPath.stem()), safeName.c_str());
            outputAudioPath = makeUniqueProjectFilePath(
                project->m_projectRoot,
                audioStem + "_" + speedToken + pitchToken,
                getSpeedExportAudioExtension(inputAudioPath, audioFormatIndex));

            MMM::BeatmapSpeedTransformOptions transformOptions;
            transformOptions.speed = speed;
            transformOptions.mapPath =
                makeProjectRelativePath(project->m_projectRoot, outputMapPath);
            transformOptions.audioPath = makeProjectRelativePath(
                project->m_projectRoot, outputAudioPath);
            transformOptions.name    = displayName;
            transformOptions.version = displayName;

            auto transformResult =
                MMM::BeatmapSpeedTransform::createSpeedVersion(
                    sourceBeatmap, transformOptions);
            if ( !transformResult.success ) {
                m_status = transformResult.errorMessage.empty()
                               ? "谱面倍速副本生成失败"
                               : transformResult.errorMessage;
                return;
            }

            outputBeatmap =
                std::make_shared<BeatMap>(std::move(transformResult.beatmap));
            if ( outputBeatmap->m_audioSamples.size() != 1U ) {
                m_status = "倍速副本未能保留唯一自动采样";
                return;
            }
            const auto outputAudioRelativePath = makeProjectRelativePath(
                project->m_projectRoot, outputAudioPath);
            outputBeatmap->m_audioSamples.front().m_audioResourceId =
                Config::pathToUtf8(outputAudioRelativePath.filename());
            outputBeatmap->m_baseMapMetadata.song_file_hint =
                outputAudioRelativePath;
            outputBeatmap->m_baseMapMetadata.main_audio_path.clear();
        }

        m_running  = true;
        m_progress = 0.0f;
        m_status   = "正在准备倍速音频...";

        auto task = [inputAudioPath,
                     outputAudioPath,
                     outputMapPath,
                     outputBeatmap,
                     displayName,
                     sourceAudioTrackType,
                     sourceAudioTrackConfig,
                     speed,
                     preservePitch]() {
            SpeedExportResultPayload payload;
            payload.mapPath          = outputMapPath;
            payload.audioPath        = outputAudioPath;
            payload.beatmap          = outputBeatmap;
            payload.displayName      = displayName;
            payload.audioTrackType   = sourceAudioTrackType;
            payload.audioTrackConfig = sourceAudioTrackConfig;

            Audio::AudioSpeedExportOptions audioOptions;
            audioOptions.inputPath     = inputAudioPath;
            audioOptions.outputPath    = outputAudioPath;
            audioOptions.speed         = speed;
            audioOptions.preservePitch = preservePitch;
            if ( outputBeatmap ) {
                audioOptions.minimumDurationSeconds =
                    std::max(0.0,
                             BeatmapSpeedTransform::calculateContentEndTime(
                                 *outputBeatmap) /
                                 1000.0);
            }
            audioOptions.progressCallback =
                [](const Audio::AudioSpeedExportProgress& progress) {
                    speedExportProgressQueue().enqueue(
                        SpeedExportProgressPayload{ progress.progress,
                                                    progress.message });
                };

            const auto audioResult =
                Audio::AudioSpeedExportService::exportWav(audioOptions);
            if ( !audioResult.success ) {
                payload.success = false;
                payload.message = audioResult.errorMessage.empty()
                                      ? "倍速音频导出失败"
                                      : audioResult.errorMessage;
                speedExportResultQueue().enqueue(std::move(payload));
                return;
            }

            if ( outputBeatmap && audioResult.outputDurationSeconds > 0.0 &&
                 std::isfinite(audioResult.outputDurationSeconds) ) {
                const double contentEndMs =
                    BeatmapSpeedTransform::calculateContentEndTime(
                        *outputBeatmap);
                outputBeatmap->m_baseMapMetadata.map_length = std::max(
                    audioResult.outputDurationSeconds * 1000.0, contentEndMs);
            }

            speedExportProgressQueue().enqueue(
                SpeedExportProgressPayload{ 0.98f, "正在保存倍速谱面..." });
            if ( !outputBeatmap || !outputBeatmap->saveToFile(outputMapPath) ) {
                payload.success = false;
                payload.message = "倍速谱面保存失败";
                speedExportResultQueue().enqueue(std::move(payload));
                return;
            }

            payload.success = true;
            payload.message = "谱面倍速制作完成";
            speedExportResultQueue().enqueue(std::move(payload));
        };

        auto* threadPool = Runtime::AppThreadPool::instance().get();
        if ( threadPool ) {
            threadPool->enqueue_void(std::move(task));
        } else {
            task();
        }
    }

    /// @brief 消费谱面倍速制作后台任务消息。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧只消费无锁队列中的已完成消息。
    void consumeQueues(MainMenuContext& context)
    {
        SpeedExportProgressPayload progress;
        while ( speedExportProgressQueue().try_dequeue(progress) ) {
            m_progress = std::clamp(progress.progress, 0.0f, 1.0f);
            m_status   = std::move(progress.message);
        }

        SpeedExportResultPayload result;
        while ( speedExportResultQueue().try_dequeue(result) ) {
            m_running  = false;
            m_progress = result.success ? 1.0f : 0.0f;
            m_status   = result.message;

            if ( result.success && result.beatmap ) {
                auto& engine = Logic::EditorEngine::instance();
                Audio::AudioManager::instance().invalidateTrackCache(
                    Config::pathToUtf8(result.audioPath));
                engine.handleImportAudio(
                    Logic::CmdImportAudio{ Config::pathToUtf8(result.audioPath),
                                           result.audioTrackType });

                auto* project = engine.getCurrentProject();
                if ( !project ) {
                    result.message = "倍速音频已生成，但当前项目已关闭";
                    m_status       = result.message;
                    context.statusMessageSink.showStatusMessage(result.message,
                                                                4.0F);
                    continue;
                }

                const std::string relativeAudioPath =
                    Config::pathToUtf8(makeProjectRelativePath(
                        project->m_projectRoot, result.audioPath));
                const std::string outputResourceId =
                    Config::pathToUtf8(result.audioPath.filename());
                const auto resource = std::find_if(
                    project->m_audioResources.begin(),
                    project->m_audioResources.end(),
                    [&](const AudioResource& candidate) {
                        return candidate.m_path == relativeAudioPath ||
                               candidate.m_id == outputResourceId;
                    });
                if ( resource == project->m_audioResources.end() ) {
                    result.message = "倍速音频已生成，但项目音频资源登记失败";
                    m_status       = result.message;
                    context.statusMessageSink.showStatusMessage(result.message,
                                                                4.0F);
                    continue;
                }

                resource->m_config = result.audioTrackConfig;
                if ( resource->m_type == AudioTrackType::Effect ) {
                    Audio::AudioManager::instance().registerSoundEffect(
                        resource->m_id,
                        Config::pathToUtf8(result.audioPath),
                        resource->m_config);
                }
                engine.saveProject();
                engine.syncProjectWithFile(result.mapPath);
                engine.createSession(result.beatmap, result.displayName);

                context.statusMessageSink.showStatusMessage(result.message,
                                                            3.0f);
                m_showPopup = false;
            } else {
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
    void renderPopup(float dpiScale)
    {
        constexpr const char* popupId =
            "谱面倍速制作###BeatmapSpeedExportModal";
        if ( m_showPopup ) {
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showPopup = false;
        }

        const float popupWidth = std::floor(520.0f * dpiScale);
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( !modalScope.begin(popupId,
                               nullptr,
                               ImGuiWindowFlags_None,
                               ImVec2(popupWidth, 0.0f)) ) {
            return;
        }

        ImGui::BeginDisabled(m_running);
        const auto refreshAutoNameIfNeeded = [this]() {
            const std::string autoName = buildSpeedExportAutoName(m_factor);
            if ( !m_nameEdited && autoName != m_autoName ) {
                copyToInputBuffer(m_nameBuffer, autoName);
            }
            m_autoName = autoName;
        };
        refreshAutoNameIfNeeded();

        ImGui::TextUnformatted("输出名称");
        ImGui::SetNextItemWidth(-1.0f);
        if ( ImGui::InputText("##SpeedExportName",
                              m_nameBuffer.data(),
                              m_nameBuffer.size()) ) {
            m_nameEdited = trimSpaces(m_nameBuffer.data()) != m_autoName;
        }

        ImGui::TextUnformatted("倍速");
        ImGui::SetNextItemWidth(-1.0f);
        if ( ::MMM::UI::FeedbackSliderFloat(
                 "##SpeedExportFactor", &m_factor, 0.25f, 4.0f, "%.4fx") ) {
            m_factor = std::clamp(m_factor, 0.25f, 4.0f);
            refreshAutoNameIfNeeded();
        }

        const ImGuiStyle& style        = ImGui::GetStyle();
        const float       contentWidth = ImGui::GetContentRegionAvail().x;
        const float       buttonW =
            std::floor((contentWidth - style.ItemSpacing.x * 3.0f) / 4.0f);
        if ( ::MMM::UI::FeedbackButton("0.75x", ImVec2(buttonW, 0.0f)) ) {
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
            for ( std::size_t index = 0; index < formatOptions.size();
                  ++index ) {
                const bool selected =
                    m_audioFormatIndex == static_cast<int>(index);
                if ( ::MMM::UI::FeedbackSelectable(formatOptions[index].label,
                                                   selected) ) {
                    m_audioFormatIndex = static_cast<int>(index);
                }
                if ( selected ) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ::MMM::UI::FeedbackEndCombo();
        }
        ImGui::EndDisabled();

        if ( m_running || !m_status.empty() ) {
            ImGui::Spacing();
            ImGui::ProgressBar(m_progress, ImVec2(-1.0f, 0.0f), nullptr);
            ImGui::TextWrapped("%s", m_status.c_str());
        }

        ImGui::Separator();
        const float actionButtonWidth = std::floor(
            (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.0f);
        const ImVec2 actionButtonSize(actionButtonWidth, 0.0f);
        ImGui::BeginDisabled(m_running);
        if ( ::MMM::UI::FeedbackButton("开始制作", actionButtonSize) ) {
            startExport();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton("关闭", actionButtonSize) &&
             !m_running ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    /// @brief 是否显示谱面倍速制作弹窗。
    bool m_showPopup = false;

    /// @brief 谱面倍速制作后台任务是否运行中。
    bool m_running = false;

    /// @brief 谱面倍速制作倍率。
    float m_factor = 1.2f;

    /// @brief 谱面倍速音频是否保留原音高。
    bool m_preservePitch = true;

    /// @brief 谱面倍速音频输出格式索引；0 表示跟随源音频。
    int m_audioFormatIndex = 0;

    /// @brief 输出名称是否已被用户手动编辑。
    bool m_nameEdited = false;

    /// @brief 谱面倍速制作输出名称输入缓存。
    std::array<char, 192> m_nameBuffer{};

    /// @brief 当前自动生成的输出名称，用于判断是否跟随倍率刷新。
    std::string m_autoName;

    /// @brief 谱面倍速制作进度。
    float m_progress = 0.0f;

    /// @brief 谱面倍速制作状态文本。
    std::string m_status;
};
}  // namespace

/// @brief 创建打开谱面倍速制作窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenBeatmapSpeedExportAction()
{
    return std::make_unique<OpenBeatmapSpeedExportAction>();
}

}  // namespace MMM::UI
