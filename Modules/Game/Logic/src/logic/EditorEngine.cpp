#include "logic/EditorEngine.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include <chrono>

namespace MMM::Logic
{

EditorEngine& EditorEngine::instance()
{
    static EditorEngine instance;
    return instance;
}

EditorEngine::EditorEngine()
{
    // 默认创建一个 Session
    m_activeSession = std::make_unique<BeatmapSession>();

    // 订阅画布尺寸改变事件
    Event::EventBus::instance().subscribe<Event::CanvasResizeEvent>(
        [this](const Event::CanvasResizeEvent& e) {
            // 目前假设只有一个主画布，未来如果有多摄像机，需要匹配 canvasName
            // 或传入 camera ID
            pushCommand(CmdUpdateViewport{ e.canvasName,
                                           static_cast<float>(e.newSize.x),
                                           static_cast<float>(e.newSize.y) });
        });
}

EditorEngine::~EditorEngine()
{
    stop();
}

void EditorEngine::start()
{
    if ( m_running ) {
        return;
    }
    m_running = true;

    m_thread = std::thread(&EditorEngine::loop, this);
    XINFO("EditorEngine logic thread started.");
}

void EditorEngine::stop()
{
    if ( m_running ) {
        m_running = false;
        if ( m_thread.joinable() ) {
            m_thread.join();
        }
        XINFO("EditorEngine logic thread stopped.");
    }
}

void EditorEngine::pushCommand(LogicCommand&& cmd)
{
    if ( m_activeSession ) {
        m_activeSession->pushCommand(std::move(cmd));
    }
}

std::shared_ptr<BeatmapSyncBuffer> EditorEngine::getSyncBuffer(
    const std::string& cameraId)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    if ( m_syncBuffers.find(cameraId) == m_syncBuffers.end() ) {
        m_syncBuffers[cameraId] = std::make_shared<BeatmapSyncBuffer>();
    }
    return m_syncBuffers[cameraId];
}

void EditorEngine::loop()
{
    auto lastTime = std::chrono::high_resolution_clock::now();

    // 独立线程死循环，不限制帧率
    while ( m_running ) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> dt = currentTime - lastTime;
        lastTime                         = currentTime;

        if ( m_activeSession ) {
            m_activeSession->update(dt.count(), m_editorConfig);
        } else {
            // 如果没有活动的 Session，稍微让出一下 CPU 避免空转占用过高
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

}  // namespace MMM::Logic
