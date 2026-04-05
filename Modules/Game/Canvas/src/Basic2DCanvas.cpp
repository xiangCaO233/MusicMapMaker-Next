#include "canvas/Basic2DCanvas.h"
#include "common/LogicCommands.h"
#include "config/skin/SkinConfig.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "ui/ITextureLoader.h"
#include "ui/IUIView.h"
#include "ui/UIManager.h"
#include <filesystem>
#include <glm/glm.hpp>
#include <utility>

namespace MMM::Canvas
{
Basic2DCanvas::Basic2DCanvas(const std::string& name, uint32_t w, uint32_t h,
                             const std::string& cameraId)
    : IUIView(name)
    , IRenderableView(name)
    , m_canvasName(name)
    , m_cameraId(cameraId.empty() ? name : cameraId)
{
    m_targetWidth  = w;
    m_targetHeight = h;
}

// 接口实现
void Basic2DCanvas::update(UI::UIManager* sourceManager)
{
    std::string windowName =
        fmt::format("{}###{}", TR("canvas.editor"), m_canvasName);
    UI::LayoutContext lctx(m_layoutCtx, windowName);
    RenderContext     rctx(
        this, m_canvasName.c_str(), m_targetWidth, m_targetHeight);

    // 尝试拉取最新的逻辑线程渲染快照 (专属摄像机缓冲)
    auto syncBuffer = Logic::EditorEngine::instance().getSyncBuffer(m_cameraId);
    if ( syncBuffer ) {
        m_currentSnapshot = syncBuffer->pullLatestSnapshot();
    }

    // --- 快捷键处理：仅主画布响应空格切换播放/暂停 ---
    if ( m_currentSnapshot && ImGui::IsKeyPressed(ImGuiKey_Space, false) ) {
        // 如果 ImGui 没有捕获键盘（即没有在输入框中），则响应
        // if ( !ImGui::GetIO().WantCaptureKeyboard ) {
        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdSetPlayState{ !m_currentSnapshot->isPlaying });
        // }
    }

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

    // --- MVP 交互实现：拾取测试 ---
    if ( m_currentSnapshot ) {
        // 获取鼠标在 ImGui 窗口中的相对位置
        ImVec2 mousePos = ImGui::GetMousePos();

        // 由于 Basic2DCanvas 是绘制在全屏 Dock 窗口中的 Image，
        // 我们需要计算相对于窗口内容的本地坐标。
        ImVec2 windowPos     = ImGui::GetWindowPos();
        ImVec2 localMousePos = { mousePos.x - windowPos.x,
                                 mousePos.y - windowPos.y };

        entt::entity hoveredEntity = entt::null;

        // 倒序遍历 Hitbox (最上层的优先)
        for ( auto it = m_currentSnapshot->hitboxes.rbegin();
              it != m_currentSnapshot->hitboxes.rend();
              ++it ) {
            if ( localMousePos.x >= it->x && localMousePos.x <= it->x + it->w &&
                 localMousePos.y >= it->y &&
                 localMousePos.y <= it->y + it->h ) {
                hoveredEntity = it->entity;
                break;
            }
        }

        // 推送悬停指令到逻辑线程
        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdSetHoveredEntity{ hoveredEntity });

        // --- 交互：点击选择与拖拽 ---
        if ( ImGui::IsMouseClicked(0) ) {
            if ( hoveredEntity != entt::null ) {
                // 点击了实体，发送选择指令
                Logic::EditorEngine::instance().pushCommand(
                    Logic::CmdSelectEntity{ hoveredEntity,
                                            !ImGui::GetIO().KeyCtrl });

                // 如果没在拖拽，则尝试开始拖拽
                Logic::EditorEngine::instance().pushCommand(
                    Logic::CmdStartDrag{ hoveredEntity, m_cameraId });
            } else {
                // 点击空白处，取消所有选择
                Logic::EditorEngine::instance().pushCommand(
                    Logic::CmdSelectEntity{ entt::null, true });
            }
        }

        if ( ImGui::IsMouseDragging(0) ) {
            Logic::EditorEngine::instance().pushCommand(Logic::CmdUpdateDrag{
                m_cameraId, localMousePos.x, localMousePos.y });
        }

        if ( ImGui::IsMouseReleased(0) ) {
            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdEndDrag{ m_cameraId });
        }
    }
}

// --- 改变尺寸后的回调 ---
void Basic2DCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_cameraId;  // 使用 cameraId 告知逻辑线程哪个视口变了
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
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

void Basic2DCanvas::onRecordDrawCmds(vk::CommandBuffer& cmdBuf,
                                     vk::PipelineLayout pipelineLayout,
                                     vk::DescriptorSet  defaultDescriptor)
{
    if ( !m_currentSnapshot ) return;

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        // 核心修复：强制通过 getImTextureID 确保描述符集已在 ImGui 中注册
        atlasDescriptor = (vk::DescriptorSet)(VkDescriptorSet)
                              m_textureAtlas->getImTextureID();
    }

    vk::DescriptorSet lastBoundTexture = VK_NULL_HANDLE;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        vk::DescriptorSet actualTexture = cmd.texture;

        // 核心修复：只要该 ID 在图集 UV 映射表中（包含 ID 0），就使用图集
        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( cmd.customTextureId ==
                    static_cast<uint32_t>(Logic::TextureID::Background) ) {
            if ( m_bgTexture ) {
                actualTexture = (vk::DescriptorSet)(VkDescriptorSet)
                                    m_bgTexture->getImTextureID();
            }
        }

        if ( actualTexture == VK_NULL_HANDLE ) {
            actualTexture = defaultDescriptor;
        }

        // 避免冗余绑定
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

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

///@brief 是否需要重新记录命令 (比如数据变了)
bool Basic2DCanvas::isDirty() const
{
    return true;
}

/**
 * @brief 获取 Shader 源码接口 (在固定时刻需要创建)
 */
std::vector<std::string> Basic2DCanvas::getShaderSources(
    const std::string& shader_name)
{
    // 如果缓存里有，直接返回内存里的字符串拷贝
    if ( m_shaderSourceCache.find(shader_name) != m_shaderSourceCache.end() ) {
        return m_shaderSourceCache[shader_name];
    }

    // 读取spv源码初始化shader
    Config::SkinData::CanvasConfig canvas_config =
        Config::SkinManager::instance().getCanvasConfig(m_canvasName);
    if ( canvas_config.canvas_name == "" ) {
        XERROR("无法获取画布{}的配置", m_canvasName);
        return {};
    }

    // 获取着色器源码
    if ( auto shaderModuleIt =
             canvas_config.canvas_shader_modules.find(shader_name);
         shaderModuleIt != canvas_config.canvas_shader_modules.end() ) {
        // 着色器spv文件路径
        auto shader_spv_path = shaderModuleIt->second;
        if ( !std::filesystem::exists(shader_spv_path) ) {
            XWARN("Shader module {} not defiend.", shader_name);
            return {};
        }

        // 读取着色器文件内容
        std::string vertexShaderSource = Graphic::VKShader::readFile(
            (shader_spv_path / "VertexShader.spv").generic_string());
        std::string fragmentShaderSource = Graphic::VKShader::readFile(
            (shader_spv_path / "FragmentShader.spv").generic_string());

        // 源码列表
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

        // 存入缓存
        m_shaderSourceCache[shader_name] = result;
        return result;
    } else {
        XERROR("无法获取画布{}的{}着色器配置", m_canvasName, shader_name);
        return {};
    }
}

/**
 * @brief 获取 Shader 名称(需要按名称储存和销毁)
 */
std::string Basic2DCanvas::getShaderName(const std::string& shader_module_name)
{
    return m_canvasName + ":" + shader_module_name;
}

/// @brief 是否需要重载
bool Basic2DCanvas::needReload()
{
    return std::exchange(m_needReload, false);
}

/// @brief 重载纹理
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

    // 注册所有纹理到图集
    // 1. 注册 4x4 纯白纹理作为 None (用于绘制纯色几何体而不切换 DrawCall)
    // 使用 4x4 配合 UV 收缩可以彻底防止线性过滤导致的采样溢出
    unsigned char white[] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255 };
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::None), white, 4, 4);



    // 2. 注册资源纹理
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

    // 自动加载所有序列帧资源，并使用 SkinManager 分配好的 ID
    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        uint32_t currentId = seq.startId;
        for ( const auto& frame : seq.frames ) {
            m_textureAtlas->addTexture(currentId++, frame);
        }
    }

    // 构建图集
    m_textureAtlas->build(4096);

    // 更新 UV 映射缓存并同步给逻辑线程
    m_atlasUVs.clear();
    for ( uint32_t i = static_cast<uint32_t>(Logic::TextureID::None);
          i <= static_cast<uint32_t>(Logic::TextureID::JudgeArea);
          ++i ) {
        // 背景不进入合图，跳过其 UV 记录，防止逻辑层错误识别
        if ( i == static_cast<uint32_t>(Logic::TextureID::Background) )
            continue;

        m_atlasUVs[i] = m_textureAtlas->getUV(i);
    }

    // 更新特序列帧 UV
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
