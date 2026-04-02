#pragma once

#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/LogicCommands.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace MMM::Logic
{

/**
 * @brief 编辑器逻辑引擎 (全局单例)
 *
 * 负责管理后台逻辑线程的生命周期，以及分发指令给当前活动的 BeatmapSession。
 */
class EditorEngine
{
public:
    static EditorEngine& instance();

    EditorEngine();
    ~EditorEngine();

    // 禁用拷贝与移动
    EditorEngine(EditorEngine&&)                 = delete;
    EditorEngine(const EditorEngine&)            = delete;
    EditorEngine& operator=(EditorEngine&&)      = delete;
    EditorEngine& operator=(const EditorEngine&) = delete;

    /**
     * @brief 启动逻辑线程
     */
    void start();

    /**
     * @brief 停止逻辑线程
     */
    void stop();

    /**
     * @brief 向当前活动的 Session 推送指令
     */
    void pushCommand(LogicCommand&& cmd);

    /**
     * @brief 获取指定摄像机/画布的同步缓冲区
     */
    std::shared_ptr<BeatmapSyncBuffer> getSyncBuffer(
        const std::string& cameraId);

private:
    /**
     * @brief 逻辑线程的主循环
     */
    void loop();

    /// @brief 逻辑线程
    std::thread m_thread;

    /// @brief 线程运行标志
    std::atomic<bool> m_running{ false };

    /// @brief 当前激活的谱面会话 (ECS 核心)
    std::unique_ptr<BeatmapSession> m_activeSession;

    /// @brief 所有的同步缓冲区 (Key 为 CameraID)
    std::unordered_map<std::string, std::shared_ptr<BeatmapSyncBuffer>>
        m_syncBuffers;

    /// @brief 保护缓冲区的锁
    std::mutex m_bufferMutex;
};

}  // namespace MMM::Logic
