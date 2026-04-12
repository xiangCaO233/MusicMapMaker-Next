#include "canvas/Basic2DCanvas.h"
#include "event/core/EventBus.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include <fmt/format.h>
#include <utility>

namespace MMM::Canvas
{

Basic2DCanvas::Basic2DCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer,
    const std::string&                        cameraId)
    : IUIView(name)
    , IRenderableView(name)
    , m_canvasName(name)
    , m_cameraId(cameraId.empty() ? name : cameraId)
    , m_syncBuffer(std::move(syncBuffer))
{
    m_targetWidth  = w;
    m_targetHeight = h;

    m_dropSubId = Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
        [this](const Event::GLFWDropEvent& e) {
            XINFO("Basic2DCanvas received GLFWDropEvent with {} paths",
                  e.paths.size());
            m_pendingDrops.push_back({ e.paths, e.pos });
        });
}

Basic2DCanvas::~Basic2DCanvas()
{
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(m_dropSubId);
}

void Basic2DCanvas::update(UI::UIManager* sourceManager)
{
    std::string windowName =
        fmt::format("{}###{}", TR("canvas.editor"), m_canvasName);
    UI::LayoutContext lctx(m_layoutCtx, windowName);
    RenderContext     rctx(
        this, m_canvasName.c_str(), m_targetWidth, m_targetHeight);

    // 1. 处理拖拽文件
    handleDrops(sourceManager);

    // 2. 拉取快照
    if ( m_syncBuffer ) {
        m_currentSnapshot = m_syncBuffer->pullLatestSnapshot();
    }

    if ( m_currentSnapshot ) {
        // 3. 处理快捷键
        handleHotkeys();

        // 4. 更新背景纹理
        updateBackgroundTexture();

        // 5. 处理交互 (拾取、选择、拖拽、滚轮)
        handleInteractions();
    }
}

bool Basic2DCanvas::isDirty() const
{
    return true;
}

void Basic2DCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_cameraId;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
}

bool Basic2DCanvas::needReload()
{
    return std::exchange(m_needReload, false);
}

} // namespace MMM::Canvas
