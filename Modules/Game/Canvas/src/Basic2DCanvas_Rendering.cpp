#include "canvas/Basic2DCanvas.h"

#include "canvas/BackgroundVideoTiming.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "font/AsciiFontRasterizer.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKShader.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
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
std::filesystem::path resolveCanvasAsciiFontPath(Config::SkinManager& skin)
{
    const auto& preference =
        Config::AppConfig::instance().getEditorSettings().preferredAsciiFont;
    if ( !preference.empty() && preference != "Default" ) {
        const auto& fonts = skin.getAsciiFonts();
        auto        it =
            std::find_if(fonts.begin(), fonts.end(), [&](const auto& entry) {
                return entry.first == preference;
            });
        if ( it != fonts.end() ) {
            return it->second;
        }

        const auto      externalPath = Config::utf8ToPath(preference);
        std::error_code pathError;
        if ( std::filesystem::is_regular_file(externalPath, pathError) &&
             !pathError ) {
            return externalPath;
        }
    }
    return skin.getFontPath("ascii");
}

}  // namespace

/// @brief 当快照背景路径或类型变化时加载或清理背景资源。
/// @warning 低频阻塞路径：可能访问文件系统、创建 Vulkan 纹理并等待
/// GPU；调用方必须保证不在每帧无条件执行。
void Basic2DCanvas::updateBackgroundTexture()
{
    if ( m_currentSnapshot &&
         (m_currentSnapshot->backgroundPath != m_loadedBgPath ||
          m_currentSnapshot->backgroundIsVideo != m_loadedBackgroundIsVideo) ) {
        m_loadedBgPath            = m_currentSnapshot->backgroundPath;
        m_loadedBackgroundIsVideo = m_currentSnapshot->backgroundIsVideo;

        // 关键修复：替换背景纹理前，必须等待所有在途帧完成渲染。
        // 旧纹理的 DescriptorSet 可能仍被上一帧的 CommandBuffer 引用，
        // 立即 Free 会触发 Vulkan Validation 错误
        // Vulkan 校验规则：VUID-vkFreeDescriptorSets-pDescriptorSets-00309。
        // 该操作仅在切换项目时触发，GPU 停顿代价可接受。
        if ( m_logicalDevice && m_bgTexture ) {
            (void)m_logicalDevice.waitIdle();
        }

        std::error_code textureExistsError;
        const bool      texturePathExists =
            !m_loadedBgPath.empty() &&
            std::filesystem::is_regular_file(Config::utf8ToPath(m_loadedBgPath),
                                             textureExistsError) &&
            !textureExistsError;
        m_bgTexture.reset();
        m_pendingVideoPixels.clear();
        m_pendingVideoUploadRevision    = 0;
        m_recordedVideoUploadRevision   = 0;
        m_videoFrameAvailable           = false;
        m_videoSourceAvailable          = false;
        m_videoFrameVisible             = false;
        m_videoShouldBeVisibleThisFrame = false;
        m_hasRequestedVideoFrame        = false;
        m_lastVideoFrameRequestSysTime  = 0.0;
        m_pendingVideoSeekRetryCount    = 0;
        m_hasUploadedVideoTimestamp     = false;
        m_videoDiscontinuityPending     = false;
        m_videoReachedEnd               = false;
        m_pendingVideoStateValid        = false;

        if ( m_loadedBackgroundIsVideo ) {
            const std::filesystem::path videoPath =
                texturePathExists ? Config::utf8ToPath(m_loadedBgPath)
                                  : std::filesystem::path{};
            m_requiredVideoRequestGeneration =
                m_backgroundVideoPlayer->setSource(videoPath);
            if ( !videoPath.empty() ) {
                m_videoSourceAvailable         = true;
                m_hasRequestedVideoFrame       = true;
                m_lastRequestedVideoTime       = 0.0;
                m_videoDiscontinuityPending    = true;
                m_videoDiscontinuityTargetTime = 0.0;
                m_lastVideoFrameRequestSysTime = currentSteadySeconds();
            }
        } else if ( m_physicalDevice && m_logicalDevice && m_cmdPool &&
                    m_queue && texturePathExists ) {
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
                XERROR("Failed to load background texture: {}", m_loadedBgPath);
                m_bgTexture.reset();
            }
        } else {
            (void)m_backgroundVideoPlayer->setSource({});
        }
    }
}

/// @brief 提交已完成 GPU 纹理更新的视频帧状态。
/// @warning UI 阶段或离屏录制阶段调用；两阶段由渲染器串行。
void Basic2DCanvas::commitUploadedVideoFrame(double        timestamp,
                                             std::uint64_t requestGeneration,
                                             bool          reachedEnd)
{
    if ( requestGeneration < m_requiredVideoRequestGeneration ) {
        return;
    }

    m_hasUploadedVideoTimestamp  = true;
    m_uploadedVideoTimestamp     = timestamp;
    m_videoFrameAvailable        = true;
    m_videoDiscontinuityPending  = false;
    m_videoReachedEnd            = reachedEnd;
    m_pendingVideoSeekRetryCount = 0;
}

/// @brief 按谱面播放时钟更新背景视频帧。
/// @warning UI 热路径：解码在专用线程执行；只有首帧或分辨率变更
/// 时会在此低频分支等待 GPU 并重建纹理。
void Basic2DCanvas::updateBackgroundVideoFrame()
{
    if ( !m_currentSnapshot || !m_currentSnapshot->backgroundIsVideo ||
         m_loadedBgPath.empty() || !m_videoSourceAvailable ) {
        m_videoShouldBeVisibleThisFrame = false;
        m_videoFrameVisible             = false;
        return;
    }

    const double currentSysTime = currentSteadySeconds();
    const double targetTime     = calculateBackgroundVideoTime(
        m_currentSnapshot->resolvePlaybackTimeAt(currentSysTime),
        m_currentSnapshot->backgroundVideoStartTime);
    if ( !std::isfinite(targetTime) ) {
        m_videoShouldBeVisibleThisFrame = false;
        m_videoFrameVisible             = false;
        return;
    }
    const bool isBeforeVideoStart   = targetTime < 0.0;
    m_videoShouldBeVisibleThisFrame = !isBeforeVideoStart;

    if ( !isBeforeVideoStart ) {
        constexpr double        REQUEST_INTERVAL_SECONDS   = 1.0 / 120.0;
        constexpr double        SEEK_DISCONTINUITY_SECONDS = 0.25;
        constexpr double        PAUSED_SEEK_RETRY_SECONDS  = 0.1;
        constexpr std::uint32_t MAX_PAUSED_SEEK_RETRIES    = 3;
        const bool              movedBackward =
            m_hasRequestedVideoFrame &&
            targetTime + REQUEST_INTERVAL_SECONDS < m_lastRequestedVideoTime;
        const bool requestJumped =
            !m_hasRequestedVideoFrame || movedBackward ||
            std::abs(targetTime - m_lastRequestedVideoTime) >
                SEEK_DISCONTINUITY_SECONDS;
        const bool isPausedSeek =
            !m_currentSnapshot->isPlaying && m_hasRequestedVideoFrame &&
            std::abs(targetTime - m_lastRequestedVideoTime) >=
                REQUEST_INTERVAL_SECONDS;
        const double generationAdvanceThreshold =
            m_currentSnapshot->isPlaying ? SEEK_DISCONTINUITY_SECONDS
                                         : REQUEST_INTERVAL_SECONDS;
        const bool startsNewGeneration =
            (requestJumped || isPausedSeek) &&
            (!m_videoDiscontinuityPending ||
             std::abs(targetTime - m_videoDiscontinuityTargetTime) >=
                 generationAdvanceThreshold);
        const bool resumesFromEnd = m_videoReachedEnd && movedBackward;
        const bool retriesPendingPausedSeek =
            !m_currentSnapshot->isPlaying && m_videoDiscontinuityPending &&
            m_hasRequestedVideoFrame &&
            m_pendingVideoSeekRetryCount < MAX_PAUSED_SEEK_RETRIES &&
            currentSysTime - m_lastVideoFrameRequestSysTime >=
                PAUSED_SEEK_RETRY_SECONDS;
        const bool shouldRequest =
            (!m_videoReachedEnd || resumesFromEnd) &&
            (!m_hasRequestedVideoFrame ||
             std::abs(targetTime - m_lastRequestedVideoTime) >=
                 REQUEST_INTERVAL_SECONDS ||
             retriesPendingPausedSeek);
        if ( shouldRequest ) {
            const std::uint64_t requestGeneration =
                m_backgroundVideoPlayer->requestFrame(targetTime,
                                                      startsNewGeneration);
            if ( startsNewGeneration ) {
                m_requiredVideoRequestGeneration = requestGeneration;
                m_videoReachedEnd                = false;
                m_videoDiscontinuityPending      = true;
                m_videoDiscontinuityTargetTime   = targetTime;
                m_pendingVideoSeekRetryCount     = 0;
                m_pendingVideoPixels.clear();
                m_pendingVideoStateValid      = false;
                m_recordedVideoUploadRevision = m_pendingVideoUploadRevision;
            }
            m_hasRequestedVideoFrame       = true;
            m_lastRequestedVideoTime       = targetTime;
            m_lastVideoFrameRequestSysTime = currentSysTime;
            if ( retriesPendingPausedSeek ) {
                ++m_pendingVideoSeekRetryCount;
            }
        }
    }

    BackgroundVideoFrame decodedFrame;
    if ( m_backgroundVideoPlayer->tryTakeLatestFrame(decodedFrame) &&
         decodedFrame.requestGeneration >= m_requiredVideoRequestGeneration &&
         decodedFrame.frame.width > 0 && decodedFrame.frame.height > 0 &&
         decodedFrame.frame.rgba.size() ==
             static_cast<std::size_t>(decodedFrame.frame.width) *
                 decodedFrame.frame.height * 4U ) {
        const bool mustRecreateTexture =
            !m_bgTexture || m_bgTexture->width() != decodedFrame.frame.width ||
            m_bgTexture->height() != decodedFrame.frame.height;
        const bool isAlreadyUploadedFrame =
            !mustRecreateTexture && m_hasUploadedVideoTimestamp &&
            std::abs(decodedFrame.frame.timestamp - m_uploadedVideoTimestamp) <=
                1e-6;
        if ( !isAlreadyUploadedFrame ) {
            ++m_pendingVideoUploadRevision;
        }
        if ( mustRecreateTexture ) {
            if ( m_logicalDevice && m_bgTexture ) {
                (void)m_logicalDevice.waitIdle();
            }
            if ( m_physicalDevice && m_logicalDevice && m_cmdPool && m_queue ) {
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
                    m_recordedVideoUploadRevision =
                        m_pendingVideoUploadRevision;
                    m_pendingVideoPixels.clear();
                    m_pendingVideoStateValid = false;
                    commitUploadedVideoFrame(decodedFrame.frame.timestamp,
                                             decodedFrame.requestGeneration,
                                             decodedFrame.reachedEnd);
                } else {
                    XERROR("Failed to create streaming video texture: {}",
                           m_loadedBgPath);
                    m_bgTexture.reset();
                }
            }
        } else if ( !isAlreadyUploadedFrame ) {
            m_pendingVideoPixels     = std::move(decodedFrame.frame.rgba);
            m_pendingVideoStateValid = true;
            m_pendingVideoTimestamp  = decodedFrame.frame.timestamp;
            m_pendingVideoRequestGeneration = decodedFrame.requestGeneration;
            m_pendingVideoReachedEnd        = decodedFrame.reachedEnd;
        } else {
            commitUploadedVideoFrame(decodedFrame.frame.timestamp,
                                     decodedFrame.requestGeneration,
                                     decodedFrame.reachedEnd);
        }
        if ( !m_bgTexture || !m_bgTexture->isValid() ) {
            m_videoFrameAvailable       = false;
            m_hasRequestedVideoFrame    = false;
            m_videoDiscontinuityPending = false;
        }
    }

    m_videoFrameVisible =
        m_videoShouldBeVisibleThisFrame && m_videoFrameAvailable;
}

/// @brief 在离屏 RenderPass 开始前上传最新视频帧。
/// @warning 渲染命令录制热路径：没有新帧时仅比较修订号；有新帧时只
/// 复制到已映射 staging 槽并录制图像传输命令。
void Basic2DCanvas::onRecordResourceUploads(vk::CommandBuffer& cmdBuf,
                                            uint32_t           frameIndex)
{
    if ( !m_loadedBackgroundIsVideo || !m_bgTexture ||
         m_pendingVideoPixels.empty() || !m_pendingVideoStateValid ||
         m_recordedVideoUploadRevision == m_pendingVideoUploadRevision ) {
        return;
    }

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
        m_pendingVideoPixels.clear();
        m_pendingVideoStateValid = false;
    }
}

const std::vector<Graphic::Vertex::VKBasicVertex>&
Basic2DCanvas::getVertices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->vertices;
    }
    static std::vector<Graphic::Vertex::VKBasicVertex> empty;
    return empty;
}

const std::vector<uint32_t>& Basic2DCanvas::getIndices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->indices;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

/// @brief 录制主画布离屏绘制命令。
/// @warning 热路径：每帧命令录制时执行；只遍历快照命令列表并复用 descriptor。
void Basic2DCanvas::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                                     vk::PipelineLayout      pipelineLayout,
                                     vk::DescriptorSetLayout setLayout,
                                     vk::DescriptorSet       defaultDescriptor,
                                     uint32_t                frameIndex)
{
    if ( !m_currentSnapshot ) return;

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }
    vk::DescriptorSet backgroundDescriptor = VK_NULL_HANDLE;
    if ( m_bgTexture ) {
        backgroundDescriptor =
            m_bgTexture->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet lastBoundTexture = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        vk::DescriptorSet actualTexture = cmd.texture;

        const bool isBackground =
            cmd.customTextureId ==
            static_cast<uint32_t>(Logic::TextureID::Background);
        if ( isBackground &&
             (!m_bgTexture || (m_currentSnapshot->backgroundIsVideo &&
                               !m_videoFrameVisible)) ) {
            continue;
        }

        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( isBackground ) {
            actualTexture = backgroundDescriptor;
        }

        if ( actualTexture == VK_NULL_HANDLE ) {
            actualTexture = defaultDescriptor;
        }

        if ( actualTexture != lastBoundTexture ) {
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
            vk::Rect2D physicalScissor = getPhysicalScissor(cmd.scissor);
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

/// @brief 录制主画布发光层离屏绘制命令。
/// @warning 热路径：启用发光时每帧执行；只遍历 glow 命令列表。
void Basic2DCanvas::onRecordGlowCmds(vk::CommandBuffer&      cmdBuf,
                                     vk::PipelineLayout      pipelineLayout,
                                     vk::DescriptorSetLayout setLayout,
                                     vk::DescriptorSet       defaultDescriptor,
                                     uint32_t                frameIndex)
{
    if ( !m_currentSnapshot ) return;

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }
    vk::DescriptorSet backgroundDescriptor = VK_NULL_HANDLE;
    if ( m_bgTexture ) {
        backgroundDescriptor =
            m_bgTexture->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet lastBoundTexture = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->glowCmds ) {
        vk::DescriptorSet actualTexture = cmd.texture;

        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( cmd.customTextureId ==
                    static_cast<uint32_t>(Logic::TextureID::Background) ) {
            actualTexture = backgroundDescriptor;
        }

        if ( actualTexture == VK_NULL_HANDLE ) {
            actualTexture = defaultDescriptor;
        }

        if ( actualTexture != lastBoundTexture ) {
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
            vk::Rect2D physicalScissor = getPhysicalScissor(cmd.scissor);
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

/// @brief 记录主画布最终覆盖层离屏绘制命令。
/// @warning 热路径：每帧命令录制末尾执行；仅遍历 overlay 命令并复用已有描述符。
void Basic2DCanvas::onRecordOverlayCmds(vk::CommandBuffer&      cmdBuf,
                                        vk::PipelineLayout      pipelineLayout,
                                        vk::DescriptorSetLayout setLayout,
                                        vk::DescriptorSet defaultDescriptor,
                                        uint32_t          frameIndex)
{
    if ( !m_currentSnapshot ) return;

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }
    vk::DescriptorSet backgroundDescriptor = VK_NULL_HANDLE;
    if ( m_bgTexture ) {
        backgroundDescriptor =
            m_bgTexture->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet lastBoundTexture = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->overlayCmds ) {
        vk::DescriptorSet actualTexture = cmd.texture;

        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( cmd.customTextureId ==
                    static_cast<uint32_t>(Logic::TextureID::Background) ) {
            actualTexture = backgroundDescriptor;
        }

        if ( actualTexture == VK_NULL_HANDLE ) {
            actualTexture = defaultDescriptor;
        }

        if ( actualTexture != lastBoundTexture ) {
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
            vk::Rect2D physicalScissor = getPhysicalScissor(cmd.scissor);
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

/// @brief 判断当前快照是否包含发光绘制命令。
/// @return 当前快照存在发光命令时返回 true。
/// @warning 渲染热路径：每帧离屏命令录制前执行，只读取快照命令数量。
bool Basic2DCanvas::hasGlowDrawCmds() const
{
    return m_currentSnapshot && !m_currentSnapshot->glowCmds.empty();
}

/// @brief 判断当前快照是否包含最终覆盖层绘制命令。
/// @return 当前快照存在覆盖层命令时返回 true。
/// @warning 渲染热路径：每帧离屏命令录制前执行，只读取快照命令数量。
bool Basic2DCanvas::hasOverlayDrawCmds() const
{
    return m_currentSnapshot && !m_currentSnapshot->overlayCmds.empty();
}

std::vector<std::string> Basic2DCanvas::getShaderSources(
    const std::string& shader_name)
{
    if ( m_shaderSourceCache.count(shader_name) )
        return m_shaderSourceCache[shader_name];

    Config::SkinData::CanvasConfig canvas_config =
        Config::SkinManager::instance().getCanvasConfig(m_canvasName);
    if ( canvas_config.canvas_name == "" ) {
        XERROR("无法获取画布{}的配置", m_canvasName);
        return {};
    }

    if ( auto shaderModuleIt =
             canvas_config.canvas_shader_modules.find(shader_name);
         shaderModuleIt != canvas_config.canvas_shader_modules.end() ) {
        auto            shader_spv_path = shaderModuleIt->second;
        std::error_code shaderPathError;
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

        m_shaderSourceCache[shader_name] = result;
        return result;
    } else {
        XERROR("无法获取画布{}的{}着色器配置", "Basic2DCanvas", shader_name);
        return {};
    }
}

std::string Basic2DCanvas::getShaderName(const std::string& shader_module_name)
{
    return m_canvasName + ":" + shader_module_name;
}

/// @brief 清空缓存的 shader 源码。
/// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
void Basic2DCanvas::invalidateShaderSourceCache()
{
    m_shaderSourceCache.clear();
}

void Basic2DCanvas::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                   vk::Device&         logicalDevice,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
    m_physicalDevice = physicalDevice;
    m_logicalDevice  = logicalDevice;
    m_cmdPool        = cmdPool;
    m_queue          = queue;

    auto& skin = Config::SkinManager::instance();

    m_textureAtlas = std::make_unique<Graphic::VKTextureAtlas>(
        physicalDevice, logicalDevice, cmdPool, queue);

    unsigned char white[] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255 };
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::None), white, 4, 4);

    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::Note),
                               skin.getAssetPath("note.note"));
    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::Node),
                               skin.getAssetPath("note.node"));
    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::HoldEnd),
                               skin.getAssetPath("note.holdend"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::HoldBodyVertical),
        skin.getAssetPath("note.holdbodyvertical"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::HoldBodyHorizontal),
        skin.getAssetPath("note.holdbodyhorizontal"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::FlickArrowLeft),
        skin.getAssetPath("note.arrowleft"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::FlickArrowRight),
        skin.getAssetPath("note.arrowright"));

    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::Track),
                               skin.getAssetPath("panel.track.background"));
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::JudgeArea),
        skin.getAssetPath("panel.track.judgearea"));
    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::Logo),
                               skin.getAssetPath("logo"));

    m_asciiFontAtlasMetrics      = {};
    const auto preferredFontPath = resolveCanvasAsciiFontPath(skin);
    std::array<std::optional<Graphic::RasterizedAsciiFont>,
               Common::ASCII_FONT_RASTER_TIER_COUNT>
         rasterizedFonts;
    auto rasterizeFontTiers = [&](const std::filesystem::path& fontPath) {
        bool loadedAnyTier = false;
        for ( std::size_t tierIndex = 0U;
              tierIndex < Common::ASCII_FONT_RASTER_TIER_COUNT;
              ++tierIndex ) {
            rasterizedFonts[tierIndex] =
                Graphic::AsciiFontRasterizer::rasterize(
                    fontPath, Common::ASCII_FONT_RASTER_HEIGHTS[tierIndex]);
            loadedAnyTier =
                rasterizedFonts[tierIndex].has_value() || loadedAnyTier;
        }
        return loadedAnyTier;
    };
    bool       loadedFont      = rasterizeFontTiers(preferredFontPath);
    const auto defaultFontPath = skin.getFontPath("ascii");
    if ( !loadedFont && preferredFontPath != defaultFontPath ) {
        XWARN("Failed to rasterize preferred ASCII font, using skin default");
        loadedFont = rasterizeFontTiers(defaultFontPath);
    }
    if ( loadedFont ) {
        for ( std::size_t tierIndex = 0U;
              tierIndex < Common::ASCII_FONT_RASTER_TIER_COUNT;
              ++tierIndex ) {
            const auto& rasterizedFont = rasterizedFonts[tierIndex];
            if ( !rasterizedFont ) continue;

            m_asciiFontAtlasMetrics.tiers[tierIndex] = rasterizedFont->metrics;
            m_asciiFontAtlasMetrics.valid            = true;
            for ( std::uint32_t code = Common::ASCII_GLYPH_FIRST;
                  code <= Common::ASCII_GLYPH_LAST;
                  ++code ) {
                const std::size_t index = code - Common::ASCII_GLYPH_FIRST;
                const auto& metrics     = rasterizedFont->metrics.glyphs[index];
                const auto& glyph       = rasterizedFont->glyphs[index];
                if ( !metrics.hasBitmap || glyph.pixels.empty() ) continue;

                const auto textureId = Logic::asciiGlyphTextureId(
                    tierIndex, static_cast<char>(code));
                m_textureAtlas->addTexture(
                    static_cast<std::uint32_t>(textureId),
                    glyph.pixels.data(),
                    glyph.width,
                    glyph.height);
            }
        }
    }

    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        uint32_t currentId = seq.startId;
        for ( const auto& frame : seq.frames ) {
            m_textureAtlas->addTexture(currentId++, frame);
        }
    }

    m_textureAtlas->build(4096);

    m_atlasUVs.clear();
    for ( uint32_t i = static_cast<uint32_t>(Logic::TextureID::None);
          i <= static_cast<uint32_t>(Logic::TextureID::Logo);
          ++i ) {
        if ( i == static_cast<uint32_t>(Logic::TextureID::Background) )
            continue;

        m_atlasUVs[i] = m_textureAtlas->getUV(i);
    }

    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        for ( uint32_t i = 0; i < seq.frames.size(); ++i ) {
            uint32_t id    = seq.startId + i;
            m_atlasUVs[id] = m_textureAtlas->getUV(id);
        }
    }

    if ( m_asciiFontAtlasMetrics.valid ) {
        for ( std::size_t tierIndex = 0U;
              tierIndex < Common::ASCII_FONT_RASTER_TIER_COUNT;
              ++tierIndex ) {
            const auto& metrics = m_asciiFontAtlasMetrics.tiers[tierIndex];
            if ( !metrics.valid ) continue;

            for ( std::uint32_t code = Common::ASCII_GLYPH_FIRST;
                  code <= Common::ASCII_GLYPH_LAST;
                  ++code ) {
                const std::size_t index = code - Common::ASCII_GLYPH_FIRST;
                if ( !metrics.glyphs[index].hasBitmap ) continue;
                const auto textureId = Logic::asciiGlyphTextureId(
                    tierIndex, static_cast<char>(code));
                m_atlasUVs[static_cast<std::uint32_t>(textureId)] =
                    m_textureAtlas->getUV(
                        static_cast<std::uint32_t>(textureId));
            }
        }
    }

    Logic::EditorEngine::instance().setAtlasUVMap(
        m_cameraId, m_atlasUVs, m_asciiFontAtlasMetrics);
    m_loadedAsciiFontPreference =
        Config::AppConfig::instance().getEditorSettings().preferredAsciiFont;
    XINFO("Basic2DCanvas textures reloaded into atlas for camera: " +
          m_cameraId);
}

}  // namespace MMM::Canvas
