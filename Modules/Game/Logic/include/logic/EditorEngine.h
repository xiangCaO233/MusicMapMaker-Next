#pragma once

#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "mmm/project/Project.h"
#include <atomic>
#include <filesystem>
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

    // ... (保持禁用拷贝移动部分)

    /**
     * @brief 启动逻辑线程
     */
    void start();

    /**
     * @brief 停止逻辑线程
     */
    void stop();

    /**
     * @brief 打开项目目录并加载其中的所有资源
     */
    void openProject(const std::filesystem::path& projectPath);

    /**
     * @brief 获取当前项目
     */
    Project*       getCurrentProject() { return m_currentProject.get(); }
    const Project* getCurrentProject() const { return m_currentProject.get(); }

    /**
     * @brief 向当前活动的 Session 推送指令
     */
    void pushCommand(LogicCommand&& cmd);


    /**
     * @brief 获取指定摄像机/画布的同步缓冲区
     */
    std::shared_ptr<BeatmapSyncBuffer> getSyncBuffer(
        const std::string& cameraId);

    /**
     * @brief 设置全局图集 UV 映射 (由 UI 线程在构建图集后调用)
     */
    void setAtlasUVMap(const std::string&                             cameraId,
                       const std::unordered_map<uint32_t, glm::vec4>& uvMap)
    {
        std::lock_guard<std::mutex> lock(m_atlasMutex);
        m_cameraUVMaps[cameraId] = uvMap;
    }

    /**
     * @brief 获取当前全局图集 UV 映射
     */
    std::unordered_map<uint32_t, glm::vec4> getAtlasUVMap(
        const std::string& cameraId)
    {
        std::lock_guard<std::mutex> lock(m_atlasMutex);
        if ( m_cameraUVMaps.find(cameraId) != m_cameraUVMaps.end() ) {
            return m_cameraUVMaps[cameraId];
        }
        // 回退到默认图集 (Main)
        if ( cameraId != "Main" ) {
            auto it = m_cameraUVMaps.find("Main");
            if ( it != m_cameraUVMaps.end() ) return it->second;
        }
        return {};
    }

    /**
     * @brief 获取当前编辑器配置
     */
    const Config::EditorConfig& getEditorConfig() const
    {
        return m_editorConfig;
    }

    /**
     * @brief 设置编辑器配置 (同时分发指令给 Session)
     */
    void setEditorConfig(const Config::EditorConfig& config)
    {
        m_editorConfig = config;
        // 同步回全局 AppConfig 实例
        Config::AppConfig::instance().getEditorConfig() = config;
        pushCommand(CmdUpdateEditorConfig{ config });
    }

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

    /// @brief 当前打开的项目
    std::unique_ptr<Project> m_currentProject;

    /// @brief 所有的同步缓冲区 (Key 为 CameraID)
    std::unordered_map<std::string, std::shared_ptr<BeatmapSyncBuffer>>
        m_syncBuffers;

    /// @brief 保护缓冲区的锁
    std::mutex m_bufferMutex;

    /// @brief 编辑器配置
    Config::EditorConfig m_editorConfig;

    /// @brief 各摄像机独立的图集 UV 映射表
    std::unordered_map<std::string, std::unordered_map<uint32_t, glm::vec4>>
               m_cameraUVMaps;
    std::mutex m_atlasMutex;
};

}  // namespace MMM::Logic
