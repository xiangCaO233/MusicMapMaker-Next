/// @file Basic2DCanvas_Rendering.cpp
/// @brief 实现主画布背景媒体、纹理图集、字体栅格化与 Vulkan 命令录制。
///
/// 资源重载路径允许文件系统访问和 GPU 同步，
/// 但只能由路径、皮肤、字体或 DPI 变化触发。
/// 每帧录制路径只读取当前不可变渲染快照、描述符和上传修订号，
/// 不执行磁盘读取、字体栅格化或完整项目遍历。
///
/// 背景视频解码在 BackgroundVideoPlayer 工作线程完成，
/// UI 阶段只请求目标时间并接收最新 RGBA 帧，
/// RenderPass 开始前再把待处理像素写入可复用 staging 槽。
/// requestGeneration 隔离跳转前后的解码结果，
/// uploadRevision 隔离 CPU 已接收与 GPU 已录制的帧状态。
#include "canvas/Basic2DCanvas.h"

#include "canvas/BackgroundVideoTiming.h"
#include "common/UnicodeFontData.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "font/AsciiFontRasterizer.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "mmm/project/Project.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

namespace MMM::Canvas
{
namespace
{

/// @brief 解析软件设置当前选中的 ASCII 字体路径。
/// @param skin 当前皮肤管理器。
/// @return 首选字体、外部字体或皮肤默认字体路径。
/// @details 非 Default 偏好先按皮肤枚举字体名称匹配，
/// 未命中时再将偏好解释为 UTF-8 外部文件路径。
/// 外部路径必须是可访问的普通文件；任何查询错误均回退到皮肤默认字体。
/// @warning 低频字体图集重载路径：包含线性查找和文件系统查询。
std::filesystem::path resolveCanvasAsciiFontPath(Config::SkinManager& skin)
{
    const auto& preference =
        Config::AppConfig::instance().getEditorSettings().preferredAsciiFont;
    // 空值和 Default 都明确表示使用皮肤默认项，不访问外部路径。
    if ( !preference.empty() && preference != "Default" ) {
        const auto& fonts = skin.getAsciiFonts();
        // 枚举项 first 是设置页持久化的显示标识，second 是已解析路径。
        auto it =
            std::find_if(fonts.begin(), fonts.end(), [&](const auto& entry) {
                return entry.first == preference;
            });
        if ( it != fonts.end() ) {
            // 命名字体由 SkinManager 验证来源，直接复用其规范路径。
            return it->second;
        }

        const auto      externalPath = Config::utf8ToPath(preference);
        std::error_code pathError;
        // 使用 error_code 重载保持项目无异常边界；损坏路径与不存在
        // 采用相同回退语义，不中断整个画布纹理重建。
        if ( std::filesystem::is_regular_file(externalPath, pathError) &&
             !pathError ) {
            return externalPath;
        }
    }
    // 皮肤默认字体是所有偏好失败后的稳定兜底。
    return skin.getFontPath("ascii");
}

/// @brief 解析软件设置当前选中的 CJK 字体路径。
/// @param skin 当前皮肤管理器。
/// @return 首选字体、外部字体或皮肤默认字体路径。
/// @details 解析顺序与 ASCII 字体一致，但使用独立 CJK 枚举和默认键，
/// 避免大量 Unicode 字形意外落到仅覆盖 ASCII 的字体文件。
/// @warning 低频字体图集重载路径：包含线性查找和文件系统查询。
std::filesystem::path resolveCanvasCjkFontPath(Config::SkinManager& skin)
{
    const auto& preference =
        Config::AppConfig::instance().getEditorSettings().preferredCjkFont;
    // Default 语义由设置层保证稳定，不将该字面值当作相对文件路径。
    if ( !preference.empty() && preference != "Default" ) {
        const auto& fonts = skin.getCjkFonts();
        const auto  iterator =
            std::find_if(fonts.begin(), fonts.end(), [&](const auto& entry) {
                return entry.first == preference;
            });
        if ( iterator != fonts.end() ) {
            // 枚举命中优先于同名外部路径，保持设置页所见即所得。
            return iterator->second;
        }

        const auto      externalPath = Config::utf8ToPath(preference);
        std::error_code pathError;
        // 外部字体只接受普通文件，目录、断链和权限错误全部安全回退。
        if ( std::filesystem::is_regular_file(externalPath, pathError) &&
             !pathError ) {
            return externalPath;
        }
    }
    return skin.getFontPath("cjk");
}

/// @brief 收集当前项目音频资源标签实际需要的非 ASCII 码点。
/// @return 已排序去重的 Unicode 码点。
/// @details 音频资源 ID 会绘制在画布标签中，需在重载时预先栅格化；
/// ASCII 字符由固定层级图集覆盖，因此这里只追加非 ASCII 码点。
/// 排序去重为字体栅格化器提供稳定输入，也让相同项目得到稳定纹理 ID。
/// @warning 低频字体图集重载路径：会遍历项目音频资源，禁止在每帧调用。
std::vector<std::uint32_t> collectProjectAudioLabelCodepoints()
{
    std::vector<std::uint32_t> result;
    const auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        return result;
    }

    // 两个码点只是减少常见短 ID 的扩容启发值，不构成资源长度上限。
    result.reserve(project->m_audioResources.size() * 2U);
    for ( const auto& resource : project->m_audioResources ) {
        // helper 按 UTF-8 解码并忽略 ASCII，非法序列遵循公共字体数据策略。
        Common::appendNonAsciiCodepoints(result, resource.m_id);
    }
    // 先排序再 unique，避免为同一码点建立重复纹理并稳定图集装入顺序。
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

}  // namespace

/// @brief 当快照背景路径或类型变化时加载或清理背景资源。
/// @details 先记录新路径与媒体类型，再等待仍可能引用旧 descriptor 的
/// 在途 GPU 工作完成，然后统一清空静态纹理及视频请求/上传状态。
/// 视频背景只设置异步解码来源并请求零时刻首帧；
/// 静态图片在 Vulkan 设备资源齐备且路径有效时同步建立 VKTexture。
/// 缺失、非法或加载失败的背景都收敛为空资源，不保留上一项目画面。
/// @warning 低频阻塞路径：可能访问文件系统、创建 Vulkan 纹理并等待
/// GPU；调用方必须保证不在每帧无条件执行。
void Basic2DCanvas::updateBackgroundTexture()
{
    // 外层比较是进入所有阻塞操作的唯一门槛；相同路径和类型的快照
    // 每帧到达时不会重复查询文件系统或重建 descriptor。
    if ( m_currentSnapshot &&
         (m_currentSnapshot->backgroundPath != m_loadedBgPath ||
          m_currentSnapshot->backgroundIsVideo != m_loadedBackgroundIsVideo) ) {
        m_loadedBgPath            = m_currentSnapshot->backgroundPath;
        m_loadedBackgroundIsVideo = m_currentSnapshot->backgroundIsVideo;

        // 替换背景纹理前等待全部在途帧，旧 DescriptorSet 可能仍被
        // 上一帧 CommandBuffer 引用；提前释放会违反
        // VUID-vkFreeDescriptorSets-pDescriptorSets-00309。
        // 该阻塞仅由背景代际变化触发，不允许移入常规视频帧更新路径。
        if ( m_logicalDevice && m_bgTexture ) {
            (void)m_logicalDevice.waitIdle();
        }

        std::error_code textureExistsError;
        const bool      texturePathExists =
            !m_loadedBgPath.empty() &&
            std::filesystem::is_regular_file(Config::utf8ToPath(m_loadedBgPath),
                                             textureExistsError) &&
            !textureExistsError;
        // 无论新路径是否合法都先丢弃旧资源和解码状态，避免项目切换后
        // 因新资源失败而继续显示上一项目的背景。
        m_bgTexture.reset();
        // pending 像素和两个 revision 属于旧纹理尺寸/来源，必须一起
        // 归零，不能让新来源误认为旧上传已经录制。
        m_pendingVideoPixels.clear();
        m_pendingVideoUploadRevision  = 0;
        m_recordedVideoUploadRevision = 0;
        m_videoFrameAvailable         = false;
        m_videoSourceAvailable        = false;
        // available 表示 GPU 中存在帧，visible 还受开始时间控制；
        // 两者分别清理以维持状态语义。
        m_videoFrameVisible             = false;
        m_videoShouldBeVisibleThisFrame = false;
        m_hasRequestedVideoFrame        = false;
        m_lastVideoFrameRequestSysTime  = 0.0;
        // 请求时间、重试次数和 uploaded timestamp 都不能跨来源继承，
        // 否则首帧可能被错误去重或延后。
        m_pendingVideoSeekRetryCount = 0;
        m_hasUploadedVideoTimestamp  = false;
        m_videoDiscontinuityPending  = false;
        m_videoReachedEnd            = false;
        // pending 元数据只有在像素缓冲有效时才可提交，来源变化时
        // 显式作废，避免保留旧 generation 的 reachedEnd。
        m_pendingVideoStateValid = false;

        if ( m_loadedBackgroundIsVideo ) {
            // 非法视频路径以空 source 传给播放器，播放器负责取消旧代际；
            // 返回 generation 仍作为后续迟到帧的最低接受边界。
            const std::filesystem::path videoPath =
                texturePathExists ? Config::utf8ToPath(m_loadedBgPath)
                                  : std::filesystem::path{};
            m_requiredVideoRequestGeneration =
                m_backgroundVideoPlayer->setSource(videoPath);
            if ( !videoPath.empty() ) {
                // 首帧按零时刻 discontinuity 请求，使解码器执行 seek 并
                // 从新来源建立稳定时间基线。
                m_videoSourceAvailable         = true;
                m_hasRequestedVideoFrame       = true;
                m_lastRequestedVideoTime       = 0.0;
                m_videoDiscontinuityPending    = true;
                m_videoDiscontinuityTargetTime = 0.0;
                m_lastVideoFrameRequestSysTime = currentSteadySeconds();
            }
        } else if ( m_physicalDevice && m_logicalDevice && m_cmdPool &&
                    m_queue && texturePathExists ) {
            // 切换到静态图片时先取消视频来源，再同步创建采样纹理；
            // 设备句柄不完整时不会尝试部分构造。
            (void)m_backgroundVideoPlayer->setSource({});
            m_bgTexture = std::make_unique<Graphic::VKTexture>(
                Config::utf8ToPath(m_loadedBgPath),
                m_physicalDevice,
                m_logicalDevice,
                m_cmdPool,
                m_queue);
            if ( m_bgTexture->isValid() ) {
                XINFO("Loaded background texture: {}", m_loadedBgPath);
            } else {
                // 无效 VKTexture 不得进入命令录制的 descriptor 获取路径。
                XERROR("Failed to load background texture: {}", m_loadedBgPath);
                m_bgTexture.reset();
            }
        } else {
            // 无背景、路径无效或设备尚未就绪都需要显式取消旧视频解码。
            (void)m_backgroundVideoPlayer->setSource({});
        }
    }
}

/// @brief 提交已完成 GPU 纹理更新的视频帧状态。
/// @param timestamp 已上传帧在视频时间轴中的时间。
/// @param requestGeneration 产生该帧的解码请求代际。
/// @param reachedEnd 解码器是否已到达当前视频末尾。
/// @details 只有不早于 required generation 的帧可成为可见状态，
/// 成功提交会结束 discontinuity 等待、清零暂停重试并更新末尾标志。
/// 像素数据和 upload revision 由调用方在录制成功后分别清理。
/// @warning UI 阶段或离屏录制阶段调用；两阶段由渲染器串行。
void Basic2DCanvas::commitUploadedVideoFrame(double        timestamp,
                                             std::uint64_t requestGeneration,
                                             bool          reachedEnd)
{
    // 跳转前的异步解码结果可能晚到；代际检查阻止旧时间画面覆盖
    // 新 Seek 正在等待的目标帧。
    if ( requestGeneration < m_requiredVideoRequestGeneration ) {
        return;
    }

    // timestamp 只在 GPU 资源已经包含该帧时更新，用于去除解码器
    // 重复交付以及判断视频画面是否可显示。
    m_hasUploadedVideoTimestamp = true;
    m_uploadedVideoTimestamp    = timestamp;
    m_videoFrameAvailable       = true;
    m_videoDiscontinuityPending = false;
    m_videoReachedEnd           = reachedEnd;
    // reachedEnd 只描述当前已上传帧所属解码状态，不代表纹理无效。
    m_pendingVideoSeekRetryCount = 0;
    // 成功帧建立新的稳定基线，后续暂停定位不再重试旧目标。
}

/// @brief 按谱面播放时钟更新背景视频帧。
/// @details 将快照播放时钟扣除视频开始偏移得到目标视频时间。
/// 播放期间按 120 Hz 上限请求连续帧；明显跳转、倒退或暂停定位
/// 开启新 request generation，使旧解码结果无法覆盖新目标。
/// 暂停 Seek 在有限次数内按 100 ms 重试，等待精确定位帧，
/// 但函数本身不 sleep、不等待解码线程，也不阻塞本地画布交互。
/// 解码线程交付的最新 RGBA 帧在尺寸变化时重建纹理，
/// 尺寸不变时保存为 pending 像素，留待 RenderPass 前录制上传。
/// @warning UI 热路径：解码在专用线程执行；只有首帧或分辨率变更
/// 时会在此低频分支等待 GPU 并重建纹理。
void Basic2DCanvas::updateBackgroundVideoFrame()
{
    // 背景类型、路径或来源任一无效时立即隐藏视频；旧 GPU 纹理可能
    // 尚待资源重建清理，但绝不能继续参与本帧绘制。
    if ( !m_currentSnapshot || !m_currentSnapshot->backgroundIsVideo ||
         m_loadedBgPath.empty() || !m_videoSourceAvailable ) {
        m_videoShouldBeVisibleThisFrame = false;
        m_videoFrameVisible             = false;
        return;
    }

    const double currentSysTime = currentSteadySeconds();
    // resolvePlaybackTimeAt 使用快照中的播放锚点外推当前时间，
    // 再扣除背景开始偏移得到视频自身从零开始的时间轴。
    const double targetTime = calculateBackgroundVideoTime(
        m_currentSnapshot->resolvePlaybackTimeAt(currentSysTime),
        m_currentSnapshot->backgroundVideoStartTime);
    if ( !std::isfinite(targetTime) ) {
        // 非有限时间不发送给解码器，也不沿用旧帧作为可见背景。
        m_videoShouldBeVisibleThisFrame = false;
        m_videoFrameVisible             = false;
        return;
    }
    const bool isBeforeVideoStart = targetTime < 0.0;
    // 视频开始前可以保留已解码纹理供随后立即显示，但当前帧必须隐藏。
    m_videoShouldBeVisibleThisFrame = !isBeforeVideoStart;

    if ( !isBeforeVideoStart ) {
        // 请求间隔限制连续播放频率；跳转阈值区分普通帧推进和需要
        // 解码器 Seek 的不连续变化；暂停重试只用于静止目标。
        constexpr double        REQUEST_INTERVAL_SECONDS   = 1.0 / 120.0;
        constexpr double        SEEK_DISCONTINUITY_SECONDS = 0.25;
        constexpr double        PAUSED_SEEK_RETRY_SECONDS  = 0.1;
        constexpr std::uint32_t MAX_PAUSED_SEEK_RETRIES    = 3;
        // 三个阈值分别约束连续请求频率、不连续 Seek 判定和暂停重试，
        // 不能共用一个数值：播放推进与暂停精确定位的容忍度不同。
        const bool movedBackward =
            m_hasRequestedVideoFrame &&
            targetTime + REQUEST_INTERVAL_SECONDS < m_lastRequestedVideoTime;
        // 首帧、倒退或超过 250 ms 的跳跃都属于显著不连续。
        const bool requestJumped =
            !m_hasRequestedVideoFrame || movedBackward ||
            std::abs(targetTime - m_lastRequestedVideoTime) >
                SEEK_DISCONTINUITY_SECONDS;
        const bool isPausedSeek =
            // 暂停时即使变化小于通用跳转阈值，也需要定位新目标帧。
            !m_currentSnapshot->isPlaying && m_hasRequestedVideoFrame &&
            std::abs(targetTime - m_lastRequestedVideoTime) >=
                REQUEST_INTERVAL_SECONDS;
        // 暂停状态下时间变化来自用户定位而非自然播放，哪怕只跨一帧
        // 也应当被视为需要解码器重新定位的明确目标。
        const double generationAdvanceThreshold =
            // 播放中避免每个普通帧开启代际；暂停拖动则按单帧请求
            // 间隔识别新的用户目标。
            m_currentSnapshot->isPlaying ? SEEK_DISCONTINUITY_SECONDS
                                         : REQUEST_INTERVAL_SECONDS;
        const bool startsNewGeneration =
            (requestJumped || isPausedSeek) &&
            (!m_videoDiscontinuityPending ||
             std::abs(targetTime - m_videoDiscontinuityTargetTime) >=
                 generationAdvanceThreshold);
        // 已有 discontinuity 等待期间，仅当用户目标继续移动超过当前
        // 模式阈值才推进代际，避免拖动每帧都使解码结果过期。
        // 到达末尾后普通前进不再请求；只有目标倒退才恢复解码。
        const bool resumesFromEnd = m_videoReachedEnd && movedBackward;
        const bool retriesPendingPausedSeek =
            // 解码器尚未交付当前暂停 Seek 时进行有限重试；系统时间
            // 只用于非阻塞节流，不在 UI 线程等待固定窗口。
            !m_currentSnapshot->isPlaying && m_videoDiscontinuityPending &&
            m_hasRequestedVideoFrame &&
            m_pendingVideoSeekRetryCount < MAX_PAUSED_SEEK_RETRIES &&
            currentSysTime - m_lastVideoFrameRequestSysTime >=
                PAUSED_SEEK_RETRY_SECONDS;
        // 重试上限避免损坏媒体或无法精确 seek 的解码器永久产生请求；
        // 新目标会清零计数并重新获得自己的有限重试机会。
        const bool shouldRequest =
            (!m_videoReachedEnd || resumesFromEnd) &&
            (!m_hasRequestedVideoFrame ||
             std::abs(targetTime - m_lastRequestedVideoTime) >=
                 REQUEST_INTERVAL_SECONDS ||
             retriesPendingPausedSeek);
        // shouldRequest 同时执行末尾抑制和频率限制；即使 targetTime
        // 每帧变化，也不会超过约 120 次每秒写入解码请求队列。
        if ( shouldRequest ) {
            // requestFrame 只写入后台请求队列并立即返回其 generation，
            // 解码和 seek 均在播放器工作线程完成。
            const std::uint64_t requestGeneration =
                m_backgroundVideoPlayer->requestFrame(targetTime,
                                                      startsNewGeneration);
            if ( startsNewGeneration ) {
                // 新代际立即作废旧 pending 像素和状态；即使旧帧随后
                // 到达，也会在接收条件中因 generation 过小而被丢弃。
                m_requiredVideoRequestGeneration = requestGeneration;
                m_videoReachedEnd                = false;
                m_videoDiscontinuityPending      = true;
                m_videoDiscontinuityTargetTime   = targetTime;
                m_pendingVideoSeekRetryCount     = 0;
                m_pendingVideoPixels.clear();
                m_pendingVideoStateValid = false;
                // 对齐 recorded revision 可阻止已作废像素进入下一次上传。
                m_recordedVideoUploadRevision = m_pendingVideoUploadRevision;
            }
            m_hasRequestedVideoFrame = true;
            m_lastRequestedVideoTime = targetTime;
            // 缓存实际请求目标而不是快照原始播放时间，后续跳转检测
            // 与视频开始偏移处于相同坐标系。
            m_lastVideoFrameRequestSysTime = currentSysTime;
            if ( retriesPendingPausedSeek ) {
                // 只统计实际发出的重试，普通新目标会在新代际分支清零。
                ++m_pendingVideoSeekRetryCount;
            }
        }
    }

    BackgroundVideoFrame decodedFrame;
    // 只取播放器最新完成帧；尺寸与 RGBA 字节数必须严格匹配，
    // 防止损坏缓冲进入纹理构造或 staging 复制。
    if ( m_backgroundVideoPlayer->tryTakeLatestFrame(decodedFrame) &&
         decodedFrame.requestGeneration >= m_requiredVideoRequestGeneration &&
         decodedFrame.frame.width > 0 && decodedFrame.frame.height > 0 &&
         decodedFrame.frame.rgba.size() ==
             static_cast<std::size_t>(decodedFrame.frame.width) *
                 decodedFrame.frame.height * 4U ) {
        // 条件链先拒绝旧 generation，再验证正尺寸和精确 RGBA 大小；
        // 乘积转换到 size_t 后与 vector 大小比较，避免部分帧上传。
        const bool mustRecreateTexture =
            // 首帧或分辨率改变需要重建 image；同尺寸帧可复用 streaming
            // staging 槽和现有 descriptor。
            !m_bgTexture || m_bgTexture->width() != decodedFrame.frame.width ||
            m_bgTexture->height() != decodedFrame.frame.height;
        const bool isAlreadyUploadedFrame =
            // 解码器可能重复返回同一 timestamp；已有同尺寸 GPU 帧时
            // 不增加 revision，也不重复录制上传。
            !mustRecreateTexture && m_hasUploadedVideoTimestamp &&
            std::abs(decodedFrame.frame.timestamp - m_uploadedVideoTimestamp) <=
                1e-6;
        if ( !isAlreadyUploadedFrame ) {
            // revision 表示 CPU 侧出现新的待上传内容，单调递增用于
            // RenderPass 前判断本帧是否仍需录制传输。
            ++m_pendingVideoUploadRevision;
        }
        if ( mustRecreateTexture ) {
            // 旧 image 和 descriptor 可能仍被在途命令引用；尺寸变化
            // 属于低频事件，允许在销毁前等待设备空闲。
            if ( m_logicalDevice && m_bgTexture ) {
                (void)m_logicalDevice.waitIdle();
            }
            if ( m_physicalDevice && m_logicalDevice && m_cmdPool && m_queue ) {
                // 构造函数上传首帧并建立新尺寸 image，随后准备双 staging
                // 槽供相邻帧复用，避免每帧重新分配上传资源。
                m_bgTexture = std::make_unique<Graphic::VKTexture>(
                    decodedFrame.frame.rgba.data(),
                    decodedFrame.frame.width,
                    decodedFrame.frame.height,
                    m_physicalDevice,
                    m_logicalDevice,
                    m_cmdPool,
                    m_queue);
                if ( m_bgTexture->isValid() &&
                     m_bgTexture->prepareStreamingUpload(m_physicalDevice,
                                                         2U) ) {
                    // 首帧已由构造路径进入 GPU，无需再次放入 pending；
                    // recorded revision 与当前 revision 对齐后提交状态。
                    m_recordedVideoUploadRevision =
                        m_pendingVideoUploadRevision;
                    m_pendingVideoPixels.clear();
                    m_pendingVideoStateValid = false;
                    commitUploadedVideoFrame(decodedFrame.frame.timestamp,
                                             decodedFrame.requestGeneration,
                                             decodedFrame.reachedEnd);
                } else {
                    // streaming 准备失败时丢弃整个纹理，不能让后续命令
                    // 误用仅部分初始化的上传资源。
                    XERROR("Failed to create streaming video texture: {}",
                           m_loadedBgPath);
                    m_bgTexture.reset();
                }
            }
        } else if ( !isAlreadyUploadedFrame ) {
            // 同尺寸新帧转移像素所有权到 pending 槽，并把 timestamp、
            // generation 与末尾状态绑定为同一提交单元。
            m_pendingVideoPixels     = std::move(decodedFrame.frame.rgba);
            m_pendingVideoStateValid = true;
            m_pendingVideoTimestamp  = decodedFrame.frame.timestamp;
            m_pendingVideoRequestGeneration = decodedFrame.requestGeneration;
            m_pendingVideoReachedEnd        = decodedFrame.reachedEnd;
        } else {
            // 重复帧无需 GPU 操作，但 reachedEnd 等解码状态仍需提交。
            commitUploadedVideoFrame(decodedFrame.frame.timestamp,
                                     decodedFrame.requestGeneration,
                                     decodedFrame.reachedEnd);
        }
        if ( !m_bgTexture || !m_bgTexture->isValid() ) {
            // 纹理建立失败后允许后续帧重新触发请求和构造，且清除
            // discontinuity 等待，避免暂停重试状态永久悬挂。
            m_videoFrameAvailable       = false;
            m_hasRequestedVideoFrame    = false;
            m_videoDiscontinuityPending = false;
        }
    }

    // 可见性同时要求已到视频开始时间且至少有一帧成功上传；
    // pending CPU 像素本身不能作为可绘制帧。
    m_videoFrameVisible =
        m_videoShouldBeVisibleThisFrame && m_videoFrameAvailable;
}

/// @brief 在离屏 RenderPass 开始前上传最新视频帧。
/// @param cmdBuf 当前帧用于资源传输的命令缓冲。
/// @param frameIndex 帧循环槽位，用于选择不会与在途帧冲突的 staging 区。
/// @details 只有背景仍为视频、纹理和 pending 状态有效且 revision 尚未录制时
/// 才调用 VKTexture 的 streaming 上传接口。
/// 上传命令成功录制后立即提交 timestamp/generation 状态并释放 CPU 像素；
/// 命令录制失败则保留 pending 数据，允许下一帧重试。
/// @warning 渲染命令录制热路径：没有新帧时仅比较修订号；有新帧时只
/// 复制到已映射 staging 槽并录制图像传输命令。
void Basic2DCanvas::onRecordResourceUploads(vk::CommandBuffer& cmdBuf,
                                            uint32_t           frameIndex)
{
    // 快速拒绝覆盖静态背景、未完成解码、无效纹理和已录制 revision；
    // 常规无新视频帧路径不会访问像素缓冲。
    if ( !m_loadedBackgroundIsVideo || !m_bgTexture ||
         m_pendingVideoPixels.empty() || !m_pendingVideoStateValid ||
         m_recordedVideoUploadRevision == m_pendingVideoUploadRevision ) {
        return;
    }

    // recordStreamingUpload 负责 staging 写入、image layout 转换与复制命令；
    // 只有其确认成功后才能把该帧声明为 GPU 可见。
    if ( m_bgTexture->recordStreamingUpload(cmdBuf,
                                            frameIndex,
                                            m_pendingVideoPixels.data(),
                                            m_pendingVideoPixels.size()) ) {
        m_recordedVideoUploadRevision = m_pendingVideoUploadRevision;
        commitUploadedVideoFrame(m_pendingVideoTimestamp,
                                 m_pendingVideoRequestGeneration,
                                 m_pendingVideoReachedEnd);
        m_videoFrameVisible =
            m_videoShouldBeVisibleThisFrame && m_videoFrameAvailable;
        // 清空像素只释放本次已录制内容，revision 保留为去重水位。
        m_pendingVideoPixels.clear();
        m_pendingVideoStateValid = false;
    }
}

/// @brief 取得当前主画布快照的顶点数组。
/// @return 快照顶点的只读引用；无快照时返回进程期空数组。
/// @warning Vulkan 上传热路径：不得复制容器或触发快照准备。
const std::vector<Graphic::Vertex::VKBasicVertex>&
Basic2DCanvas::getVertices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->vertices;
    }
    static std::vector<Graphic::Vertex::VKBasicVertex> empty;
    return empty;
}

/// @brief 取得当前主画布快照的索引数组。
/// @return 快照索引的只读引用；无快照时返回进程期空数组。
/// @warning Vulkan 上传热路径：返回引用只在当前快照代际内有效。
const std::vector<uint32_t>& Basic2DCanvas::getIndices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->indices;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

/// @brief 录制主画布离屏绘制命令。
/// @param cmdBuf 当前离屏 RenderPass 的命令缓冲。
/// @param pipelineLayout 主画布 pipeline layout。
/// @param setLayout 纹理 descriptor set layout。
/// @param defaultDescriptor 无专用纹理时使用的默认 descriptor。
/// @param frameIndex 当前帧槽位；主绘制不直接使用。
/// @details DrawCmd 依次选择图集、独立背景或默认纹理，
/// 并缓存最近 descriptor、scissor 与混合模式以减少重复 Vulkan 状态命令。
/// 视频背景只有在本帧状态确认可见时才录制，避免开始时间前或上传前
/// 采样旧纹理内容。循环结束后恢复普通混合 pipeline。
/// @warning 热路径：每帧命令录制时执行；只遍历快照命令列表并复用 descriptor。
void Basic2DCanvas::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                                     vk::PipelineLayout      pipelineLayout,
                                     vk::DescriptorSetLayout setLayout,
                                     vk::DescriptorSet       defaultDescriptor,
                                     uint32_t                frameIndex)
{
    if ( !m_currentSnapshot ) {
        return;
    }

    // descriptor 从渲染器共享池取得；Atlas 与背景纹理各自缓存符合
    // 当前 setLayout 的原生 descriptor。
    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        // 图集内所有 TextureID 共享一个 descriptor，具体子区域由顶点 UV 指定。
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }
    vk::DescriptorSet backgroundDescriptor = VK_NULL_HANDLE;
    if ( m_bgTexture ) {
        // 背景纹理尺寸和更新频率独立，因此不并入静态图集。
        backgroundDescriptor =
            m_bgTexture->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet             lastBoundTexture = VK_NULL_HANDLE;
    Common::Render::CanvasScissor lastScissor;
    // 缓存只在本次命令列表内有效，不能跨帧保留 Vulkan 状态假设；
    // 每个 RenderPass 开始时都从显式初始状态重新推导。
    // 默认构造的 lastScissor 作为首条命令比较基线；若恰好相等，
    // RenderContext 已在 pass 开始时设置完整默认裁剪。

    // 默认从普通透明混合开始，只在 DrawCmd 明确要求时切到 additive。
    bool additiveBlend = false;
    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        // 图集纹理可共用描述符，但混合模式改变时必须切换兼容管线。
        if ( additiveBlend != cmd.additiveBlend ) {
            bindMainBlendPipeline(cmdBuf, cmd.additiveBlend);
            additiveBlend = cmd.additiveBlend;
        }
        vk::DescriptorSet actualTexture{};

        const bool isBackground =
            cmd.customTextureId ==
            static_cast<uint32_t>(Common::Render::TextureID::Background);
        if ( isBackground &&
             (!m_bgTexture || (m_currentSnapshot->backgroundIsVideo &&
                               !m_videoFrameVisible)) ) {
            // 静态背景只要求有效纹理；视频还要求目标时刻已开始且
            // 至少一帧完成 GPU 上传，否则跳过该背景 DrawCmd。
            continue;
        }

        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            // atlasUVs 同时充当纹理 ID 是否已装入图集的快速索引。
            actualTexture = atlasDescriptor;
        } else if ( isBackground ) {
            // Background 不进入图集，使用独立静态或流式纹理 descriptor。
            actualTexture = backgroundDescriptor;
        }

        if ( actualTexture == VK_NULL_HANDLE ) {
            // 缺失可选皮肤纹理回退默认 descriptor，保持命令可录制。
            actualTexture = defaultDescriptor;
        }

        if ( actualTexture != lastBoundTexture ) {
            // 相邻同图集命令复用一次 descriptor 绑定，减少驱动状态切换。
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &actualTexture,
                                      0,
                                      nullptr);
            lastBoundTexture = actualTexture;
        }

        if ( cmd.scissor != lastScissor ) {
            // 快照 scissor 使用逻辑坐标，录制前按当前 framebuffer 比例
            // 转成物理像素；相邻相同裁剪区不重复设置。
            vk::Rect2D physicalScissor = getPhysicalScissor(
                vk::Rect2D{ { cmd.scissor.x, cmd.scissor.y },
                            { cmd.scissor.width, cmd.scissor.height } });
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
        // DrawCmd 的 indexOffset 与 vertexOffset 已由逻辑批处理器预计算，
        // 录制层不再扫描或重排几何，从而保持命令顺序和混合语义。
    }
    // 恢复普通绘制，避免后续发光或覆盖层继承 additive pipeline。
    if ( additiveBlend ) {
        bindMainBlendPipeline(cmdBuf, false);
    }
}

/// @brief 录制主画布发光层离屏绘制命令。
/// @details 发光层使用独立目标和既定发光 pipeline，仍按纹理与 scissor
/// 缓存状态；命令顺序由逻辑快照保证，与主层顶点/索引缓冲共享偏移。
/// @warning 热路径：启用发光时每帧执行；只遍历 glow 命令列表。
void Basic2DCanvas::onRecordGlowCmds(vk::CommandBuffer&      cmdBuf,
                                     vk::PipelineLayout      pipelineLayout,
                                     vk::DescriptorSetLayout setLayout,
                                     vk::DescriptorSet       defaultDescriptor,
                                     uint32_t                frameIndex)
{
    if ( !m_currentSnapshot ) {
        return;
    }

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        // 发光几何复用主图集，避免为相同物件纹理建立第二份资源。
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }
    vk::DescriptorSet backgroundDescriptor = VK_NULL_HANDLE;
    if ( m_bgTexture ) {
        // 协议允许背景 ID，但是否生成背景发光命令由逻辑快照决定。
        backgroundDescriptor =
            m_bgTexture->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet             lastBoundTexture = VK_NULL_HANDLE;
    Common::Render::CanvasScissor lastScissor;
    // 发光 pass 独立初始化缓存，不能沿用主 pass 最后绑定状态，
    // 因为 RenderPass/pipeline 切换不保证 descriptor 状态可复用。

    for ( const auto& cmd : m_currentSnapshot->glowCmds ) {
        // 发光命令通常来自图集物件；背景分支保留协议完整性，
        // 缺失专用 descriptor 时统一回退默认纹理。
        vk::DescriptorSet actualTexture{};

        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( cmd.customTextureId ==
                    static_cast<uint32_t>(
                        Common::Render::TextureID::Background) ) {
            actualTexture = backgroundDescriptor;
        }

        if ( actualTexture == VK_NULL_HANDLE ) {
            // 缺失图集项保持 draw 范围和索引顺序，只替换为默认采样。
            actualTexture = defaultDescriptor;
        }

        if ( actualTexture != lastBoundTexture ) {
            // 仅纹理来源变化时重新绑定 set 0。
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &actualTexture,
                                      0,
                                      nullptr);
            lastBoundTexture = actualTexture;
        }

        if ( cmd.scissor != lastScissor ) {
            // 发光 pass 使用与主层相同逻辑裁剪约定。
            vk::Rect2D physicalScissor = getPhysicalScissor(
                vk::Rect2D{ { cmd.scissor.x, cmd.scissor.y },
                            { cmd.scissor.width, cmd.scissor.height } });
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
        // glowCmds 与主层共享几何缓冲，只记录各自筛选后的索引区间。
    }
}

/// @brief 记录主画布最终覆盖层离屏绘制命令。
/// @details 覆盖层在主层与发光合成之后录制，用于需要保持清晰的标记；
/// 它复用相同纹理选择和裁剪缓存，但不切换主层 additive 状态。
/// @warning 热路径：每帧命令录制末尾执行；仅遍历 overlay 命令并复用已有描述符。
void Basic2DCanvas::onRecordOverlayCmds(vk::CommandBuffer&      cmdBuf,
                                        vk::PipelineLayout      pipelineLayout,
                                        vk::DescriptorSetLayout setLayout,
                                        vk::DescriptorSet defaultDescriptor,
                                        uint32_t          frameIndex)
{
    if ( !m_currentSnapshot ) {
        return;
    }

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        // 覆盖层文字与标记仍来自同一图集，不复制 descriptor 所有权。
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }
    vk::DescriptorSet backgroundDescriptor = VK_NULL_HANDLE;
    if ( m_bgTexture ) {
        // 独立背景 descriptor 只在命令明确使用 Background ID 时选择。
        backgroundDescriptor =
            m_bgTexture->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet             lastBoundTexture = VK_NULL_HANDLE;
    Common::Render::CanvasScissor lastScissor;
    // 覆盖层也从空缓存开始，明确录制其所需 descriptor 和 scissor，
    // 不依赖此前发光合成留下的隐式驱动状态。

    for ( const auto& cmd : m_currentSnapshot->overlayCmds ) {
        // Overlay DrawCmd 与主层共享纹理 ID 协议，图集外的背景使用
        // 独立 descriptor，其余缺失资源回退默认纹理。
        vk::DescriptorSet actualTexture{};

        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( cmd.customTextureId ==
                    static_cast<uint32_t>(
                        Common::Render::TextureID::Background) ) {
            actualTexture = backgroundDescriptor;
        }

        if ( actualTexture == VK_NULL_HANDLE ) {
            // defaultDescriptor 确保缺失可选纹理不会产生空句柄绑定。
            actualTexture = defaultDescriptor;
        }

        if ( actualTexture != lastBoundTexture ) {
            // 相邻命令状态合并避免覆盖层标签产生大量重复绑定。
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &actualTexture,
                                      0,
                                      nullptr);
            lastBoundTexture = actualTexture;
        }

        if ( cmd.scissor != lastScissor ) {
            // 每个覆盖层分组保留逻辑 scissor，防止标签越出轨道区域。
            vk::Rect2D physicalScissor = getPhysicalScissor(
                vk::Rect2D{ { cmd.scissor.x, cmd.scissor.y },
                            { cmd.scissor.width, cmd.scissor.height } });
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
        // 保持快照命令原序可确保半透明标签按逻辑定义的层级叠放。
    }
}

/// @brief 判断当前快照是否包含发光绘制命令。
/// @return 当前快照存在发光命令时返回 true。
/// @details 渲染器据此决定是否创建和执行发光 pass，空列表时跳过
/// 额外目标切换与合成，不遍历命令内容。
/// @warning 渲染热路径：每帧离屏命令录制前执行，只读取快照命令数量。
bool Basic2DCanvas::hasGlowDrawCmds() const
{
    return m_currentSnapshot && !m_currentSnapshot->glowCmds.empty();
}

/// @brief 判断当前快照是否包含最终覆盖层绘制命令。
/// @return 当前快照存在覆盖层命令时返回 true。
/// @details 覆盖层为空时渲染器可跳过对应录制回调，避免无效 pass 状态设置。
/// @warning 渲染热路径：每帧离屏命令录制前执行，只读取快照命令数量。
bool Basic2DCanvas::hasOverlayDrawCmds() const
{
    return m_currentSnapshot && !m_currentSnapshot->overlayCmds.empty();
}

/// @brief 读取皮肤为当前主画布配置的 SPIR-V shader 模块。
/// @param shader_name Canvas 配置中的 shader 模块键。
/// @return 按顶点、可选几何、片段阶段排列的二进制内容；失败时为空。
/// @details 首次读取检查模块目录并加载必需的顶点/片段文件，
/// GeometryShader.spv 存在时插入中间阶段；成功结果缓存到实例。
/// 文件系统查询使用 error_code，不以异常表达皮肤资源缺失。
/// @warning 低频 pipeline 创建路径：包含磁盘读取，禁止从命令录制调用。
std::vector<std::string> Basic2DCanvas::getShaderSources(
    const std::string& shader_name)
{
    // 缓存命中避免同一 pipeline 重建过程重复读取 SPIR-V 文件。
    if ( m_shaderSourceCache.count(shader_name) )
        return m_shaderSourceCache[shader_name];

    Config::SkinData::CanvasConfig canvas_config =
        Config::SkinManager::instance().getCanvasConfig(m_canvasName);
    if ( canvas_config.canvas_name == "" ) {
        // 缺失画布配置无法解析模块目录，返回空交给上层停止创建。
        XERROR("无法获取画布{}的配置", m_canvasName);
        return {};
    }

    if ( auto shaderModuleIt =
             canvas_config.canvas_shader_modules.find(shader_name);
         shaderModuleIt != canvas_config.canvas_shader_modules.end() ) {
        auto            shader_spv_path = shaderModuleIt->second;
        std::error_code shaderPathError;
        // 模块路径必须存在且查询无错误；权限与断链同样视为不可用。
        if ( !std::filesystem::exists(shader_spv_path, shaderPathError) ||
             shaderPathError ) {
            XWARN("Shader module {} not defiend.", shader_name);
            return {};
        }

        std::string vertexShaderSource = Graphic::VKShader::readFile(
            Config::pathToUtf8(shader_spv_path / "VertexShader.spv"));
        std::string fragmentShaderSource = Graphic::VKShader::readFile(
            Config::pathToUtf8(shader_spv_path / "FragmentShader.spv"));

        std::vector<std::string> result;

        // 几何阶段可选，但阶段顺序必须与 pipeline 装配接口约定一致。
        if ( auto geometryShaderPath = (shader_spv_path / "GeometryShader.spv");
             std::filesystem::exists(geometryShaderPath, shaderPathError) &&
             !shaderPathError ) {
            result = { vertexShaderSource,
                       Graphic::VKShader::readFile(
                           Config::pathToUtf8(geometryShaderPath)),
                       fragmentShaderSource };
        } else {
            result = { vertexShaderSource, fragmentShaderSource };
        }

        // 只缓存成功解析的已知键，不建立负缓存，皮肤热重载后可补齐。
        m_shaderSourceCache[shader_name] = result;
        return result;
    } else {
        XERROR("无法获取画布{}的{}着色器配置", "Basic2DCanvas", shader_name);
        return {};
    }
}

/// @brief 生成按画布实例隔离的 shader pipeline 缓存名。
/// @param shader_module_name 皮肤中的模块名称。
/// @return canvasName 与模块名组合的稳定键。
/// @details 不同皮肤画布可能使用同名模块，前缀防止错误复用 pipeline。
std::string Basic2DCanvas::getShaderName(const std::string& shader_module_name)
{
    return m_canvasName + ":" + shader_module_name;
}

/// @brief 清空缓存的 shader 源码。
/// @details 皮肤切换后旧 SPIR-V 字节不能继续参与 pipeline 重建，
/// 后续查询会从新皮肤路径重新读取。
/// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
void Basic2DCanvas::invalidateShaderSourceCache()
{
    m_shaderSourceCache.clear();
}

/// @brief 使用当前皮肤和字体偏好重建主画布纹理图集。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param cmdPool 上传纹理使用的命令池。
/// @param queue 执行纹理上传的队列。
/// @details 固定物件纹理、ASCII 多级字体、按需 Unicode 字形和效果序列帧
/// 被装入同一图集；Background 保持为独立纹理，不参与 atlas UV。
/// ASCII 首选字体失败时回退皮肤默认字体，Unicode CJK 字体使用相同策略。
/// 图集完成后一次性向当前 cameraId 发布 UV 与字体度量，
/// 逻辑线程随后生成与该资源代际一致的文字和物件顶点。
/// @warning 低频资源重载路径：包含项目资源遍历、字体栅格化、磁盘读取
/// 与 GPU 上传，只能由渲染器的安全重载阶段调用。
void Basic2DCanvas::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                   vk::Device&         logicalDevice,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
    // 保存借用的设备句柄供背景资源更新使用，其所有权仍属于渲染器。
    m_physicalDevice = physicalDevice;
    m_logicalDevice  = logicalDevice;
    m_cmdPool        = cmdPool;
    m_queue          = queue;

    auto& skin = Config::SkinManager::instance();
    // 本次函数读取同一个 SkinManager 数据代际，固定资源、字体与
    // 效果序列不会混合来自不同皮肤切换阶段的路径。

    m_textureAtlas = std::make_unique<Graphic::VKTextureAtlas>(
        physicalDevice, logicalDevice, cmdPool, queue);
    // 新 atlas 在局部重建流程中完全填充后替换旧实例；所有 UV 都会
    // 随后重新生成，不允许继续引用旧 atlas 的归一化坐标。

    // 纯白 4x4 纹理服务无贴图几何，顶点色可在 shader 中直接调制。
    unsigned char white[] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255 };
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::None), white, 4, 4);
    // None ID 始终存在，任何缺失可选资源最终都能绑定有效采样图像，
    // 防止 descriptor 为 null 的 DrawCmd 破坏后续批次。

    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::Note),
        skin.getAssetPath("note.note"));
    // 固定物件纹理 ID 是 RenderSnapshot 协议的一部分，加载顺序可变，
    // 但每个资源必须继续绑定对应枚举值。
    // 长条头为可选资源；旧皮肤不增加重复图集项，渲染时回退到 Note。
    if ( const auto it = skin.getData().assetPaths.find("note.holdhead");
         it != skin.getData().assetPaths.end() && !it->second.empty() ) {
        // 可选 HoldHead 只在皮肤显式提供时占用独立图集区域；
        // 旧皮肤由逻辑 UV 映射缺失规则回退到普通 Note。
        m_textureAtlas->addTexture(
            static_cast<uint32_t>(Common::Render::TextureID::HoldHead),
            it->second);
    }
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::Node),
        skin.getAssetPath("note.node"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::HoldEnd),
        skin.getAssetPath("note.holdend"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::HoldBodyVertical),
        skin.getAssetPath("note.holdbodyvertical"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::HoldBodyHorizontal),
        skin.getAssetPath("note.holdbodyhorizontal"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::FlickArrowLeft),
        skin.getAssetPath("note.arrowleft"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::FlickArrowRight),
        skin.getAssetPath("note.arrowright"));

    // 轨道、判定区和 Logo 与物件共用图集，批次跨越这些元素时
    // 不需要切换 descriptor，只需使用各自 UV。
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::Track),
        skin.getAssetPath("panel.track.background"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::JudgeArea),
        skin.getAssetPath("panel.track.judgearea"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::Logo),
        skin.getAssetPath("logo"));

    const float fontRasterScale = currentFontRasterScale();
    // 每次重载从空度量开始，避免失败的新字体继续暴露旧字体 glyph。
    m_asciiFontAtlasMetrics             = {};
    m_asciiFontAtlasMetrics.rasterScale = fontRasterScale;
    m_unicodeFontMetrics                = {};
    const auto preferredFontPath        = resolveCanvasAsciiFontPath(skin);
    std::array<std::optional<Graphic::RasterizedAsciiFont>,
               Common::ASCII_FONT_RASTER_TIER_COUNT>
        rasterizedFonts;
    // 所有字号层级使用同一字体来源；任一层级成功即保留结果，
    // 单层失败不会阻止其它可用层级进入图集。
    auto rasterizeFontTiers = [&](const std::filesystem::path& fontPath) {
        bool loadedAnyTier = false;
        for ( std::size_t tierIndex = 0U;
              tierIndex < Common::ASCII_FONT_RASTER_TIER_COUNT;
              ++tierIndex ) {
            // 高度表与 tierIndex 同步，逻辑文字布局按同一索引选择
            // 最接近当前显示大小的预栅格层级。
            rasterizedFonts[tierIndex] =
                Graphic::AsciiFontRasterizer::rasterize(
                    fontPath, Common::ASCII_FONT_RASTER_HEIGHTS[tierIndex]);
            loadedAnyTier =
                rasterizedFonts[tierIndex].has_value() || loadedAnyTier;
        }
        // 允许部分层级成功，缺失层级由逻辑选择其它有效字体层级。
        return loadedAnyTier;
    };
    bool       loadedFont      = rasterizeFontTiers(preferredFontPath);
    const auto defaultFontPath = skin.getFontPath("ascii");
    // 首选路径可能与默认路径相同，比较后避免对同一损坏字体重复栅格化。
    if ( !loadedFont && preferredFontPath != defaultFontPath ) {
        // 只有首选路径整体失败且确实不同于默认路径时执行一次回退。
        XWARN("Failed to rasterize preferred ASCII font, using skin default");
        loadedFont = rasterizeFontTiers(defaultFontPath);
    }
    if ( loadedFont ) {
        // 至少一个层级成功后逐层发布；全部失败则保持 metrics invalid，
        // 逻辑文字渲染会按既有缺字策略跳过位图命令。
        for ( std::size_t tierIndex = 0U;
              tierIndex < Common::ASCII_FONT_RASTER_TIER_COUNT;
              ++tierIndex ) {
            const auto& rasterizedFont = rasterizedFonts[tierIndex];
            if ( !rasterizedFont ) continue;

            // 先发布该层排版度量，再将实际有位图的 glyph 装入图集；
            // 空白字符可保留 advance 而无需占用纹理空间。
            m_asciiFontAtlasMetrics.tiers[tierIndex] = rasterizedFont->metrics;
            m_asciiFontAtlasMetrics.valid            = true;
            for ( std::uint32_t code = Common::ASCII_GLYPH_FIRST;
                  code <= Common::ASCII_GLYPH_LAST;
                  ++code ) {
                // 固定 ASCII 范围保证数组索引与字符码点一一对应，
                // 不需要哈希查找即可取得 glyph 度量。
                const std::size_t index = code - Common::ASCII_GLYPH_FIRST;
                const auto& metrics     = rasterizedFont->metrics.glyphs[index];
                const auto& glyph       = rasterizedFont->glyphs[index];
                if ( !metrics.hasBitmap || glyph.pixels.empty() ) continue;

                // tier 与字符共同编码为 TextureID，跨线程快照无需携带
                // 字体对象或字符串即可引用正确位图。
                const auto textureId = Common::Render::asciiGlyphTextureId(
                    tierIndex, static_cast<char>(code));
                m_textureAtlas->addTexture(
                    static_cast<std::uint32_t>(textureId),
                    glyph.pixels.data(),
                    glyph.width,
                    glyph.height);
            }
        }
    }

    auto unicodeCodepoints = collectProjectAudioLabelCodepoints();
    // 合并逻辑快照按需请求的可见标签码点，并再次排序去重，
    // 保证项目资源与增量请求不会产生重复纹理 ID。
    unicodeCodepoints.insert(unicodeCodepoints.end(),
                             m_requestedUnicodeCodepoints.begin(),
                             m_requestedUnicodeCodepoints.end());
    std::sort(unicodeCodepoints.begin(), unicodeCodepoints.end());
    unicodeCodepoints.erase(
        std::unique(unicodeCodepoints.begin(), unicodeCodepoints.end()),
        unicodeCodepoints.end());
    // 空集合跳过 CJK 字体文件访问，纯 ASCII 项目不会承担 Unicode
    // 字体栅格化和图集空间成本。
    std::optional<Graphic::RasterizedUnicodeFont> rasterizedUnicodeFont;
    if ( !unicodeCodepoints.empty() ) {
        // Unicode 使用单一基准高度再乘 framebuffer scale，避免每个
        // 字形单独选择层级导致 CJK 标签基线不一致。
        const auto preferredCjkFontPath = resolveCanvasCjkFontPath(skin);
        rasterizedUnicodeFont = Graphic::AsciiFontRasterizer::rasterizeUnicode(
            preferredCjkFontPath,
            unicodeCodepoints,
            std::max(
                1U,
                static_cast<std::uint32_t>(std::lround(
                    static_cast<float>(Common::UNICODE_FONT_RASTER_HEIGHT) *
                    fontRasterScale))));
        const auto defaultCjkFontPath = skin.getFontPath("cjk");
        // CJK 首选与默认路径相同时不做无意义的第二次大字符集栅格化。
        if ( !rasterizedUnicodeFont &&
             preferredCjkFontPath != defaultCjkFontPath ) {
            // 外部或枚举首选字体不覆盖所需码点时回退皮肤默认 CJK 字体。
            XWARN("Failed to rasterize preferred CJK font, using skin default");
            rasterizedUnicodeFont =
                Graphic::AsciiFontRasterizer::rasterizeUnicode(
                    defaultCjkFontPath,
                    unicodeCodepoints,
                    std::max(1U,
                             static_cast<std::uint32_t>(std::lround(
                                 static_cast<float>(
                                     Common::UNICODE_FONT_RASTER_HEIGHT) *
                                 fontRasterScale))));
        }
        if ( rasterizedUnicodeFont ) {
            // Unicode 度量保存本次实际栅格化结果，供逻辑层按码点查询
            // advance、基线和位图尺寸，而不是依赖 UI 字体图集。
            // metrics 与 glyph 数组按同一索引对应，只装入确实具有位图
            // 且能映射到有效 TextureID 的码点。
            m_unicodeFontMetrics = rasterizedUnicodeFont->metrics;
            for ( std::size_t index = 0U;
                  index < rasterizedUnicodeFont->metrics.glyphs.size();
                  ++index ) {
                // 栅格位图与度量数组由 rasterizer 保证相同顺序；索引
                // 绑定不可改成按码点重新排序，否则两者会错位。
                const auto& metrics =
                    rasterizedUnicodeFont->metrics.glyphs[index];
                const auto& glyph = rasterizedUnicodeFont->glyphs[index];
                if ( !metrics.metrics.hasBitmap || glyph.pixels.empty() ) {
                    continue;
                }
                const auto textureId =
                    Common::Render::unicodeGlyphTextureId(metrics.codepoint);
                // 无法编码的码点保留排版度量但不进入纹理图集。
                if ( textureId == Common::Render::TextureID::None ) continue;
                m_textureAtlas->addTexture(
                    static_cast<std::uint32_t>(textureId),
                    glyph.pixels.data(),
                    glyph.width,
                    glyph.height);
            }
        }
    }

    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        // 序列帧使用 SkinManager 分配的连续 ID，按声明顺序装入图集。
        // key 只标识效果配置，渲染协议实际使用 startId 和帧偏移寻址。
        uint32_t currentId = seq.startId;
        for ( const auto& frame : seq.frames ) {
            // currentId 每装入一帧递增，保持动画帧索引的 O(1) 映射。
            m_textureAtlas->addTexture(currentId++, frame);
        }
    }

    // 所有静态资源收集完成后一次性打包，避免中途取得随后失效的 UV。
    m_textureAtlas->build(4096);
    // 4096 是当前图集尺寸契约；若资源无法装入，具体缺失区域通过
    // 后续零尺寸 UV 检查和默认纹理回退处理。

    m_atlasUVs.clear();
    // UV 表从空状态重建，移除新皮肤已经删除的资源，避免逻辑线程
    // 继续把旧资源 ID 识别为图集内纹理。
    for ( uint32_t i = static_cast<uint32_t>(Common::Render::TextureID::None);
          i <= static_cast<uint32_t>(Common::Render::TextureID::Logo);
          ++i ) {
        if ( i == static_cast<uint32_t>(Common::Render::TextureID::Background) )
            continue;

        // Background 由独立 descriptor 绑定，其 UV 不得伪装为 atlas 区域。
        m_atlasUVs[i] = m_textureAtlas->getUV(i);
    }

    // 可选头部位于既有连续纹理区之后，必须显式发布给逻辑线程。
    // 只发布已加载且尺寸有效的区域，旧皮肤继续通过缺失项回退到 Note。
    if ( const auto it = skin.getData().assetPaths.find("note.holdhead");
         it != skin.getData().assetPaths.end() && !it->second.empty() ) {
        const auto id =
            static_cast<uint32_t>(Common::Render::TextureID::HoldHead);
        const auto uv = m_textureAtlas->getUV(id);
        // 宽高为零表示图集加载失败或资源无效，此时不发布该 ID，
        // 使逻辑层继续执行与旧皮肤相同的 Note 回退。
        if ( uv.z > 0.0F && uv.w > 0.0F ) m_atlasUVs[id] = uv;
    }


    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        // 动态序列 ID 位于固定枚举范围之外，需要逐项显式发布 UV。
        for ( uint32_t i = 0; i < seq.frames.size(); ++i ) {
            uint32_t id    = seq.startId + i;
            m_atlasUVs[id] = m_textureAtlas->getUV(id);
        }
    }

    if ( m_asciiFontAtlasMetrics.valid ) {
        // 字体 glyph 同样位于固定纹理范围外，只为有位图的字符发布 UV。
        for ( std::size_t tierIndex = 0U;
              tierIndex < Common::ASCII_FONT_RASTER_TIER_COUNT;
              ++tierIndex ) {
            const auto& metrics = m_asciiFontAtlasMetrics.tiers[tierIndex];
            // 无效层级没有已装入图集的 glyph，整层跳过。
            if ( !metrics.valid ) continue;

            for ( std::uint32_t code = Common::ASCII_GLYPH_FIRST;
                  code <= Common::ASCII_GLYPH_LAST;
                  ++code ) {
                const std::size_t index = code - Common::ASCII_GLYPH_FIRST;
                // 空白字符只有 advance，无需 UV；逻辑排版仍读取 metrics。
                if ( !metrics.glyphs[index].hasBitmap ) continue;
                const auto textureId = Common::Render::asciiGlyphTextureId(
                    tierIndex, static_cast<char>(code));
                m_atlasUVs[static_cast<std::uint32_t>(textureId)] =
                    m_textureAtlas->getUV(
                        static_cast<std::uint32_t>(textureId));
            }
        }
    }

    if ( m_unicodeFontMetrics.valid ) {
        // Unicode TextureID 由码点稳定映射，None 表示超出协议支持范围。
        for ( const auto& glyph : m_unicodeFontMetrics.glyphs ) {
            // 仅发布已经加入图集的可见字形，空白/缺字不伪造 UV。
            if ( !glyph.metrics.hasBitmap ) continue;
            const auto textureId =
                Common::Render::unicodeGlyphTextureId(glyph.codepoint);
            if ( textureId == Common::Render::TextureID::None ) continue;
            m_atlasUVs[static_cast<std::uint32_t>(textureId)] =
                m_textureAtlas->getUV(static_cast<std::uint32_t>(textureId));
        }
    }

    // UV 与两套字体度量作为同一资源代际发布，逻辑线程不会看到
    // 新纹理坐标搭配旧排版宽度的中间状态。
    Logic::EditorEngine::instance().setAtlasUVMap(
        m_cameraId, m_atlasUVs, m_asciiFontAtlasMetrics, m_unicodeFontMetrics);
    // 发布完成后再记录偏好值，确保 needReload 的“已加载”基线只代表
    // 一个已经完整生成并交付逻辑线程的图集。
    m_loadedAsciiFontPreference =
        Config::AppConfig::instance().getEditorSettings().preferredAsciiFont;
    m_loadedCjkFontPreference =
        Config::AppConfig::instance().getEditorSettings().preferredCjkFont;
    // 只有重载流程结束后更新已加载偏好和 DPI，needReload 才能正确
    // 判断后续真实变化而不是反复触发同一代际。
    m_loadedFontRasterScale = fontRasterScale;
    // 重载完成后无需额外布尔复位，渲染器已消费 needReload 请求；
    // 后续变化通过偏好/DPI 比较或新码点再次触发。
    XINFO("Basic2DCanvas textures reloaded into atlas for camera: " +
          m_cameraId);
}

}  // namespace MMM::Canvas
