#include "canvas/Basic2DCanvas.h"
#include "config/skin/SkinConfig.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include <cmath>
#include <filesystem>

namespace MMM::Canvas
{

void Basic2DCanvas::updateBackgroundTexture()
{
    if ( m_currentSnapshot &&
         m_currentSnapshot->backgroundPath != m_loadedBgPath ) {
        m_loadedBgPath = m_currentSnapshot->backgroundPath;
        if ( m_physicalDevice && m_logicalDevice && m_cmdPool && m_queue &&
             !m_loadedBgPath.empty() &&
             std::filesystem::exists(m_loadedBgPath) ) {
            try {
                m_bgTexture =
                    std::make_unique<Graphic::VKTexture>(m_loadedBgPath,
                                                         m_physicalDevice,
                                                         m_logicalDevice,
                                                         m_cmdPool,
                                                         m_queue);
                XINFO("Loaded background texture: {}", m_loadedBgPath);
            } catch ( const std::exception& e ) {
                XERROR("Failed to load background texture: {}", e.what());
                m_bgTexture.reset();
            }
        } else {
            m_bgTexture.reset();
        }
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

    vk::DescriptorSet lastBoundTexture = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        vk::DescriptorSet actualTexture = cmd.texture;

        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( cmd.customTextureId ==
                    static_cast<uint32_t>(Logic::TextureID::Background) ) {
            if ( m_bgTexture ) {
                actualTexture =
                    m_bgTexture->getNativeDescriptorSet(pool, setLayout);
            }
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

    vk::DescriptorSet lastBoundTexture = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->glowCmds ) {
        vk::DescriptorSet actualTexture = cmd.texture;

        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( cmd.customTextureId ==
                    static_cast<uint32_t>(Logic::TextureID::Background) ) {
            if ( m_bgTexture ) {
                actualTexture =
                    m_bgTexture->getNativeDescriptorSet(pool, setLayout);
            }
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
        auto shader_spv_path = shaderModuleIt->second;
        if ( !std::filesystem::exists(shader_spv_path) ) {
            XWARN("Shader module {} not defiend.", shader_name);
            return {};
        }

        std::string vertexShaderSource = Graphic::VKShader::readFile(
            (shader_spv_path / "VertexShader.spv").generic_string());
        std::string fragmentShaderSource = Graphic::VKShader::readFile(
            (shader_spv_path / "FragmentShader.spv").generic_string());

        std::vector<std::string> result;

        if ( auto geometryShaderPath = (shader_spv_path / "GeometryShader.spv");
             std::filesystem::exists(geometryShaderPath) ) {
            result = { vertexShaderSource,
                       Graphic::VKShader::readFile(
                           geometryShaderPath.generic_string()),
                       fragmentShaderSource };
        } else {
            result = { vertexShaderSource, fragmentShaderSource };
        }

        m_shaderSourceCache[shader_name] = result;
        return result;
    } else {
        XERROR("无法获取画布{}的{}着色器配置", m_canvasName, shader_name);
        return {};
    }
}

std::string Basic2DCanvas::getShaderName(const std::string& shader_module_name)
{
    return m_canvasName + ":" + shader_module_name;
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
                               skin.getAssetPath("note.note").generic_string());
    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::Node),
                               skin.getAssetPath("note.node").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::HoldEnd),
        skin.getAssetPath("note.holdend").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::HoldBodyVertical),
        skin.getAssetPath("note.holdbodyvertical").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::HoldBodyHorizontal),
        skin.getAssetPath("note.holdbodyhorizontal").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::FlickArrowLeft),
        skin.getAssetPath("note.arrowleft").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::FlickArrowRight),
        skin.getAssetPath("note.arrowright").generic_string());

    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::Track),
        skin.getAssetPath("panel.track.background").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::JudgeArea),
        skin.getAssetPath("panel.track.judgearea").generic_string());
    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::Logo),
                               skin.getAssetPath("logo").generic_string());

    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        uint32_t currentId = seq.startId;
        for ( const auto& frame : seq.frames ) {
            m_textureAtlas->addTexture(currentId++, frame.generic_string());
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

    Logic::EditorEngine::instance().setAtlasUVMap(m_cameraId, m_atlasUVs);
    XINFO("Basic2DCanvas textures reloaded into atlas for camera: " +
          m_cameraId);
}

}  // namespace MMM::Canvas
