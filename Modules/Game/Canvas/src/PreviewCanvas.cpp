#include "canvas/PreviewCanvas.h"
#include "canvas/TimeFormatUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "ui/IUIView.h"
#include "ui/UIManager.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <utility>

namespace MMM::Canvas
{
namespace
{
/// @brief 预览密度栏期望宽度，单位为 DPI 缩放前逻辑像素。
constexpr float PREVIEW_DENSITY_WIDTH = 36.0f;

/// @brief 预览画布与密度栏之间的间隔，单位为 DPI 缩放前逻辑像素。
constexpr float PREVIEW_DENSITY_GAP = 4.0f;

/// @brief 窄窗口中仍需保留的最小预览画布宽度。
constexpr float PREVIEW_MIN_CANVAS_WIDTH = 48.0f;
}  // namespace

PreviewCanvas::PreviewCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer)
    : IUIView(name)
    , IRenderableView(name)
    , m_canvasName(name)
    , m_syncBuffer(std::move(syncBuffer))
{
    m_targetWidth  = w;
    m_targetHeight = h;
}

/// @brief 绘制预览窗口右侧独立的全谱物件密度栏。
/// @param canvasPos 预览画布内容左上角屏幕坐标。
/// @param canvasSize 扣除密度栏后的预览画布逻辑尺寸。
/// @param reservedWidth 右侧为密度栏实际预留的逻辑宽度。
/// @param dpiScale 当前窗口 DPI 缩放。
/// @warning UI 热路径：每帧最多聚合并绘制 512 个缓存样本；禁止 ECS
/// 遍历、排序、文件访问或共享指针复制。
void PreviewCanvas::drawDensityOverview(const ImVec2& canvasPos,
                                        const ImVec2& canvasSize,
                                        float         reservedWidth,
                                        float         dpiScale) const
{
    if ( reservedWidth <= 1.0f || canvasSize.y <= 1.0f ) {
        return;
    }

    const float gap =
        std::min(reservedWidth * 0.25f,
                 std::max(1.0f, std::floor(PREVIEW_DENSITY_GAP * dpiScale)));
    const ImVec2 railMin{ canvasPos.x + canvasSize.x + gap, canvasPos.y };
    const ImVec2 railMax{ canvasPos.x + canvasSize.x + reservedWidth,
                          canvasPos.y + canvasSize.y };
    if ( railMax.x - railMin.x <= 1.0f ) {
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rounding = std::clamp(
        ImGui::GetStyle().FrameRounding,
        0.0f,
        std::min(railMax.x - railMin.x, railMax.y - railMin.y) * 0.5f);
    drawList->AddRectFilled(
        railMin, railMax, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    drawList->AddRect(
        railMin, railMax, ImGui::GetColorU32(ImGuiCol_Border), rounding);

    if ( !m_currentSnapshot ) {
        return;
    }
    const auto& density = m_currentSnapshot->previewDensity;
    if ( density.counts.empty() || !std::isfinite(density.duration) ||
         density.duration <= 0.0 || density.maxCount == 0 ) {
        return;
    }

    const float innerPadding =
        std::min(std::floor(2.0f * dpiScale),
                 std::max(0.0f, (railMax.x - railMin.x) * 0.2f));
    const ImVec2 innerMin{ railMin.x + innerPadding, railMin.y + innerPadding };
    const ImVec2 innerMax{ railMax.x - innerPadding, railMax.y - innerPadding };
    const float  innerWidth  = innerMax.x - innerMin.x;
    const float  innerHeight = innerMax.y - innerMin.y;
    if ( innerWidth <= 0.0f || innerHeight <= 0.0f ) {
        return;
    }

    const std::size_t displayRowCount = std::min<std::size_t>(
        density.counts.size(),
        static_cast<std::size_t>(std::max(1.0f, std::floor(innerHeight))));
    const double currentTime = std::isfinite(m_currentSnapshot->currentTime)
                                   ? m_currentSnapshot->currentTime
                                   : 0.0;
    const double progress =
        std::clamp(currentTime / density.duration, 0.0, 1.0);
    const std::size_t currentBin =
        std::min(density.counts.size() - 1,
                 static_cast<std::size_t>(
                     progress * static_cast<double>(density.counts.size())));
    const ImU32 normalColor = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    const ImU32 activeColor = ImGui::GetColorU32(ImGuiCol_PlotHistogramHovered);
    ImVec4      activeBackground =
        ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogramHovered);
    activeBackground.w *= 0.24f;
    const ImU32 activeBackgroundColor = ImGui::GetColorU32(activeBackground);

    for ( std::size_t row = 0; row < displayRowCount; ++row ) {
        const std::size_t binBegin =
            row * density.counts.size() / displayRowCount;
        const std::size_t binEnd = std::max(
            binBegin + 1, (row + 1) * density.counts.size() / displayRowCount);
        std::uint32_t rowCount = 0;
        for ( std::size_t bin = binBegin; bin < binEnd; ++bin ) {
            rowCount = std::max(rowCount, density.counts[bin]);
        }

        // 预览画布的未来时间向上，因此时间升序样本需自底向上排列。
        const std::size_t visualRow = displayRowCount - 1 - row;
        const float       rowY0 =
            innerMin.y + innerHeight * static_cast<float>(visualRow) /
                             static_cast<float>(displayRowCount);
        const float rowY1 =
            innerMin.y + innerHeight * static_cast<float>(visualRow + 1) /
                             static_cast<float>(displayRowCount);
        const bool isCurrent = currentBin >= binBegin && currentBin < binEnd;
        if ( isCurrent ) {
            drawList->AddRectFilled(ImVec2(innerMin.x, rowY0),
                                    ImVec2(innerMax.x, rowY1),
                                    activeBackgroundColor);
        }
        if ( rowCount == 0 ) {
            continue;
        }

        const float normalized =
            static_cast<float>(rowCount) / static_cast<float>(density.maxCount);
        const float barWidth      = std::max(1.0f, innerWidth * normalized);
        const float verticalInset = rowY1 - rowY0 >= 2.0f
                                        ? std::min(0.5f, (rowY1 - rowY0) * 0.2f)
                                        : 0.0f;
        drawList->AddRectFilled(
            ImVec2(innerMin.x, rowY0 + verticalInset),
            ImVec2(innerMin.x + barWidth, rowY1 - verticalInset),
            isCurrent ? activeColor : normalColor);
    }

    const float currentY =
        innerMax.y - static_cast<float>(progress) * innerHeight;
    drawList->AddLine(ImVec2(innerMin.x, currentY),
                      ImVec2(innerMax.x, currentY),
                      activeColor,
                      std::max(1.0f, std::floor(dpiScale)));
}

/// @brief 更新预览画布 ImGui 窗口和鼠标交互。
/// @warning
/// 热路径：主渲染线程每帧执行；只发送变化后的鼠标命令，避免每帧重复事件。
void PreviewCanvas::update(UI::UIManager* sourceManager)
{
    auto& appConfig      = Config::AppConfig::instance();
    auto& editorSettings = appConfig.getEditorSettings();
    if ( !editorSettings.showPreviewWindow ) {
        return;
    }

    // 预览窗口专用 ID：###PreviewWindow
    std::string windowName =
        fmt::format("{}###PreviewWindow", TR("canvas.preview"));
    bool windowOpen = editorSettings.showPreviewWindow;

    UI::LayoutContext lctx(m_layoutCtx, windowName, true, 0, &windowOpen);
    if ( !windowOpen ) {
        editorSettings.showPreviewWindow = false;
        appConfig.save();
        return;
    }
    const float dpiScale = std::max(1.0f, lctx.m_dpiScale);
    const float requestedDensityReserve =
        std::floor((PREVIEW_DENSITY_WIDTH + PREVIEW_DENSITY_GAP) * dpiScale);
    RenderContext rctx(this,
                       windowName.c_str(),
                       m_targetWidth,
                       m_targetHeight,
                       nullptr,
                       requestedDensityReserve,
                       PREVIEW_MIN_CANVAS_WIDTH * dpiScale);

    // --- 交互：发送鼠标位置指令给逻辑线程 ---
    ImVec2     mousePos         = ImGui::GetMousePos();
    ImVec2     windowPos        = ImGui::GetCursorScreenPos();
    ImVec2     contentSize      = rctx.getRenderSize();
    const bool hasValidMousePos = ImGui::IsMousePosValid(&mousePos) &&
                                  std::isfinite(mousePos.x) &&
                                  std::isfinite(mousePos.y);
    ImVec2     localMousePos{ 0.0f, 0.0f };
    if ( hasValidMousePos ) {
        localMousePos = { mousePos.x - windowPos.x, mousePos.y - windowPos.y };
    } else if ( m_lastMouseCommand.valid ) {
        localMousePos = { m_lastMouseCommand.pos.x, m_lastMouseCommand.pos.y };
    }

    bool isHoveringContent = hasValidMousePos && mousePos.x >= windowPos.x &&
                             mousePos.x <= windowPos.x + contentSize.x &&
                             mousePos.y >= windowPos.y &&
                             mousePos.y <= windowPos.y + contentSize.y;

    bool isHovered = ImGui::IsWindowHovered() && isHoveringContent;

    ImVec2 clickPos = ImGui::GetIO().MouseClickedPos[0];
    bool   clickStartedInContent =
        hasValidMousePos && clickPos.x >= windowPos.x &&
        clickPos.x <= windowPos.x + contentSize.x &&
        clickPos.y >= windowPos.y && clickPos.y <= windowPos.y + contentSize.y;

    // 仅当点击起源于内容区，并且当前窗口拥有焦点时，才视为拖拽
    bool isDragging = hasValidMousePos && ImGui::IsMouseDragging(0) &&
                      clickStartedInContent && ImGui::IsWindowFocused();

    drawDensityOverview(
        windowPos, contentSize, rctx.getReservedRightWidth(), dpiScale);

    float viewportWidth  = contentSize.x;
    float viewportHeight = contentSize.y;

    constexpr float mouseEpsilon = 0.1f;
    bool            shouldSendMouse =
        !m_lastMouseCommand.valid ||
        std::abs(m_lastMouseCommand.pos.x - localMousePos.x) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.pos.y - localMousePos.y) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportWidth - viewportWidth) >
            mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportHeight - viewportHeight) >
            mouseEpsilon ||
        m_lastMouseCommand.isHovering != isHovered ||
        m_lastMouseCommand.isDragging != isDragging;

    if ( shouldSendMouse ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetMousePosition{ .cameraId       = m_cameraId,
                                        .mouseX         = localMousePos.x,
                                        .mouseY         = localMousePos.y,
                                        .viewportWidth  = viewportWidth,
                                        .viewportHeight = viewportHeight,
                                        .isHovering     = isHovered,
                                        .isDragging     = isDragging }));
        m_lastMouseCommand.valid         = true;
        m_lastMouseCommand.pos           = { localMousePos.x, localMousePos.y };
        m_lastMouseCommand.viewportWidth = viewportWidth;
        m_lastMouseCommand.viewportHeight = viewportHeight;
        m_lastMouseCommand.isHovering     = isHovered;
        m_lastMouseCommand.isDragging     = isDragging;
    }

    // --- 拖拽提示：告知用户松手时跳转的位置 ---
    if ( isDragging && m_currentSnapshot &&
         m_currentSnapshot->isPreviewDragging ) {
        const auto timeText = formatCanvasTime(
            m_currentSnapshot->previewHoverTime, m_currentSnapshot);
        ImGui::SetTooltip("%s",
                          TR_FMT("canvas.preview.jump_to", timeText).c_str());
    }

    // --- 跳转时间逻辑 ---
    if ( m_currentSnapshot && isHovered ) {
        // 核心修复：仅在鼠标松开时，且初始点击是在当前内容区发生时，才触发跳转。
        // 这防止了从其他窗口拖拽进入预览区松开时造成的误触跳转。
        if ( ImGui::IsMouseReleased(0) && clickStartedInContent &&
             ImGui::IsWindowFocused() ) {
            float visualOffset = Config::AppConfig::instance()
                                     .getVisualConfig()
                                     .getEffectiveVisualOffset();
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdSeek{
                    m_currentSnapshot->hoveredTime - visualOffset }));
        }

        // --- 交互：滚轮调整预览区倍率 ---
        float wheel = ImGui::GetIO().MouseWheel;
        if ( std::abs(wheel) > 0.01f && !ImGui::GetIO().KeyCtrl &&
             !ImGui::GetIO().KeySuper && !ImGui::GetIO().KeyAlt ) {
            auto  editorCfg = Logic::EditorEngine::instance().getEditorConfig();
            float step      = 0.5f;
            if ( ImGui::GetIO().KeyShift ) step *= 2.0f;

            // 增大 areaRatio
            // 代表显示更多内容（缩小），减小代表显示更少内容（放大）
            // 习惯上向上滚动为放大（减小倍率）
            editorCfg.visual.previewConfig.areaRatio -= wheel * step;
            editorCfg.visual.previewConfig.areaRatio = std::clamp(
                editorCfg.visual.previewConfig.areaRatio, 1.0f, 50.0f);

            Logic::EditorEngine::instance().setEditorConfig(editorCfg);
        }
    }
}

bool PreviewCanvas::isDirty() const
{
    return Config::AppConfig::instance().getEditorSettings().showPreviewWindow;
}

/// @brief 判断当前帧是否需要准备预览快照。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要准备时返回 true。
bool PreviewCanvas::needsParallelUiPrepare(
    const UI::UiFrameSnapshot& snapshot) const
{
    (void)snapshot;
    return m_syncBuffer && m_isOpen &&
           Config::AppConfig::instance().getEditorSettings().showPreviewWindow;
}

/// @brief 在线程池中拉取并准备预览画布快照。
/// @param snapshot 当前帧 UI 快照。
void PreviewCanvas::prepareUiFrameData(const UI::UiFrameSnapshot& snapshot)
{
    (void)snapshot;
    m_preparedSnapshot = prepareCanvasSnapshot(
        m_syncBuffer.get(), m_lastOffsetSnapshot, m_lastAppliedYOffset, true);
    m_hasPreparedSnapshot = true;
}

/// @brief 将准备好的预览快照切换到主线程可见状态。
void PreviewCanvas::swapPreparedUiFrameData()
{
    if ( !m_hasPreparedSnapshot ) {
        return;
    }

    auto&         engine      = Logic::EditorEngine::instance();
    const int32_t activeIndex = engine.getActiveSessionIndex();
    const auto*   activeEntry = engine.getSessionEntry(activeIndex);
    if ( !activeEntry || activeEntry->isLogoPlaceholder ) {
        m_currentSnapshot     = nullptr;
        m_lastOffsetSnapshot  = nullptr;
        m_lastAppliedYOffset  = 0.0f;
        m_hasPreparedSnapshot = false;
        return;
    }

    m_currentSnapshot     = m_preparedSnapshot.snapshot;
    m_lastOffsetSnapshot  = m_preparedSnapshot.offsetSnapshot;
    m_lastAppliedYOffset  = m_preparedSnapshot.appliedYOffset;
    m_hasPreparedSnapshot = false;

    if ( !m_currentSnapshot ) {
        m_lastOffsetSnapshot = nullptr;
        m_lastAppliedYOffset = 0.0f;
    }
}

void PreviewCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_cameraId;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
}

std::vector<std::string> PreviewCanvas::getShaderSources(
    const std::string& shader_name)
{
    if ( m_shaderSourceCache.count(shader_name) ) {
        return m_shaderSourceCache[shader_name];
    }

    // 预览窗口复用 BasicCanvas 的着色器配置
    Config::SkinData::CanvasConfig canvas_config =
        Config::SkinManager::instance().getCanvasConfig(m_canvasName);

    if ( canvas_config.canvas_name == "" ) {
        XERROR("PreviewCanvas: 无法获取 {} 的着色器配置", m_canvasName);
        return {};
    }

    if ( auto it = canvas_config.canvas_shader_modules.find(shader_name);
         it != canvas_config.canvas_shader_modules.end() ) {

        auto            path = it->second;
        std::error_code shaderPathError;
        if ( !std::filesystem::exists(path, shaderPathError) ||
             shaderPathError ) {
            return {};
        }

        std::string vs = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "VertexShader.spv"));
        std::string fs = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "FragmentShader.spv"));

        std::vector<std::string> result;
        auto                     gsPath = path / "GeometryShader.spv";
        std::error_code          geometryShaderPathError;
        if ( std::filesystem::exists(gsPath, geometryShaderPathError) &&
             !geometryShaderPathError ) {
            result = { vs,
                       Graphic::VKShader::readFile(Config::pathToUtf8(gsPath)),
                       fs };
        } else {
            result = { vs, fs };
        }

        m_shaderSourceCache[shader_name] = result;
        return result;
    }

    return {};
}

std::string PreviewCanvas::getShaderName(const std::string& shader_module_name)
{
    return "PreviewCanvas:" + shader_module_name;
}

/// @brief 清空缓存的 shader 源码。
/// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
void PreviewCanvas::invalidateShaderSourceCache()
{
    m_shaderSourceCache.clear();
}

bool PreviewCanvas::needReload()
{
    return m_needReload;
}

void PreviewCanvas::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                   vk::Device&         logicalDevice,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
    m_physicalDevice = physicalDevice;
    m_logicalDevice  = logicalDevice;
    m_cmdPool        = cmdPool;
    m_queue          = queue;

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

    auto& skin   = Config::SkinManager::instance();
    auto  addTex = [&](Logic::TextureID id, const std::string& key) {
        auto p = skin.getAssetPath(key);
        if ( !p.empty() )
            m_textureAtlas->addTexture(static_cast<uint32_t>(id), p);
    };

    addTex(Logic::TextureID::Note, "note.note");
    addTex(Logic::TextureID::Node, "note.node");
    addTex(Logic::TextureID::HoldEnd, "note.holdend");
    addTex(Logic::TextureID::HoldBodyVertical, "note.holdbodyvertical");
    addTex(Logic::TextureID::HoldBodyHorizontal, "note.holdbodyhorizontal");
    addTex(Logic::TextureID::FlickArrowLeft, "note.arrowleft");
    addTex(Logic::TextureID::FlickArrowRight, "note.arrowright");
    addTex(Logic::TextureID::Track, "panel.track.background");
    addTex(Logic::TextureID::JudgeArea, "panel.track.judgearea");
    addTex(Logic::TextureID::Logo, "logo");

    // 自动加载所有序列帧资源，并使用 SkinManager 分配好的 ID
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

    // 更新特序列帧 UV
    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        for ( uint32_t i = 0; i < seq.frames.size(); ++i ) {
            uint32_t id    = seq.startId + i;
            m_atlasUVs[id] = m_textureAtlas->getUV(id);
        }
    }

    Logic::EditorEngine::instance().setAtlasUVMap(m_cameraId, m_atlasUVs);

    m_needReload = false;
}

const std::vector<Graphic::Vertex::VKBasicVertex>&
PreviewCanvas::getVertices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->vertices;
    }
    static std::vector<Graphic::Vertex::VKBasicVertex> empty;
    return empty;
}

const std::vector<uint32_t>& PreviewCanvas::getIndices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->indices;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

/// @brief 录制预览画布离屏绘制命令。
/// @warning 热路径：每帧命令录制时执行；只遍历快照命令列表并复用 descriptor。
void PreviewCanvas::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
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

    vk::DescriptorSet lastBound = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        vk::DescriptorSet tex = m_atlasUVs.count(cmd.customTextureId)
                                    ? atlasDescriptor
                                    : defaultDescriptor;

        if ( tex != lastBound ) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &tex,
                                      0,
                                      nullptr);
            lastBound = tex;
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

/// @brief 记录预览画布最终覆盖层离屏绘制命令。
/// @warning 热路径：每帧命令录制末尾执行；仅遍历 overlay 命令并复用已有描述符。
void PreviewCanvas::onRecordOverlayCmds(vk::CommandBuffer&      cmdBuf,
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

    vk::DescriptorSet lastBound = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->overlayCmds ) {
        vk::DescriptorSet tex = m_atlasUVs.count(cmd.customTextureId)
                                    ? atlasDescriptor
                                    : defaultDescriptor;

        if ( tex != lastBound ) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &tex,
                                      0,
                                      nullptr);
            lastBound = tex;
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

/// @brief 判断当前预览快照是否包含最终覆盖层绘制命令。
/// @return 当前快照存在覆盖层命令时返回 true。
/// @warning 渲染热路径：每帧离屏命令录制前执行，只读取快照命令数量。
bool PreviewCanvas::hasOverlayDrawCmds() const
{
    return m_currentSnapshot && !m_currentSnapshot->overlayCmds.empty();
}

}  // namespace MMM::Canvas
