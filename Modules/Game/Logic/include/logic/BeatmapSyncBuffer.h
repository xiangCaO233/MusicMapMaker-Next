#pragma once

#include "graphic/imguivk/mesh/VKBasicVertex.h"
#include "ui/brush/BrushDrawCmd.h"
#include <atomic>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace MMM::Logic
{

// 预定义纹理ID，用于跨线程纹理映射
enum class TextureID : uint32_t {
    None       = 0,
    Background = 1,
    Note       = 2,
    Node       = 3,
    HoldBodyVertical,
    HoldBodyHorizontal,
    HoldEnd,
    FlickArrowLeft,
    FlickArrowRight,
    Track,
    JudgeArea,

    NoteSelectionBorder = 100,

    EffectStart = 1000
};

/**
 * @brief 碰撞拾取包围盒
 */
struct Hitbox {
    entt::entity entity;
    float        x;
    float        y;
    float        w;
    float        h;
};

/**
 * @brief 渲染快照数据，包含 UI 画布所需的所有几何与指令信息
 */
struct RenderSnapshot {
    std::vector<Graphic::Vertex::VKBasicVertex> vertices;
    std::vector<uint32_t>                       indices;
    std::vector<UI::BrushDrawCmd>               cmds;
    std::vector<Hitbox>                         hitboxes;

    // 纹理 UV 映射表 (TextureID -> u,v,w,h)
    std::unordered_map<uint32_t, glm::vec4> uvMap;

    // 背景纹理绝对路径
    std::string backgroundPath;

    // 背景原始尺寸
    glm::vec2 bgSize{ 0.0f, 0.0f };

    // 播放状态
    bool   isPlaying{ false };
    double currentTime{ 0.0 };

    /// @brief 清理当前快照数据（保留内存容量）
    void clear()
    {
        vertices.clear();
        indices.clear();
        cmds.clear();
        hitboxes.clear();
        uvMap.clear();
        backgroundPath.clear();
        bgSize      = glm::vec2(0.0f);
        isPlaying   = false;
        currentTime = 0.0;
    }
};

/**
 * @brief 负责在 ECS 逻辑线程和 UI 渲染线程间实现无锁三缓冲原子交换
 */
class BeatmapSyncBuffer
{
public:
    BeatmapSyncBuffer();
    ~BeatmapSyncBuffer() = default;

    // 禁用拷贝与移动
    BeatmapSyncBuffer(BeatmapSyncBuffer&&)                 = delete;
    BeatmapSyncBuffer(const BeatmapSyncBuffer&)            = delete;
    BeatmapSyncBuffer& operator=(BeatmapSyncBuffer&&)      = delete;
    BeatmapSyncBuffer& operator=(const BeatmapSyncBuffer&) = delete;

    /**
     * @brief [逻辑线程] 获取一个可供写入的工作快照缓冲区
     * @return 准备被写入的缓冲区指针
     */
    RenderSnapshot* getWorkingSnapshot();

    /**
     * @brief [逻辑线程] 提交写完的快照（原子交换为 completed）
     */
    void pushWorkingSnapshot();

    /**
     * @brief [UI 线程] 拉取最新的完成快照（原子交换），并把当前用完的退还
     * @return 最新的快照指针，始终返回一个有效的快照地址（哪怕里面数据为空）
     */
    RenderSnapshot* pullLatestSnapshot();

private:
    // 三个实际的物理缓冲区
    RenderSnapshot m_buffers[3];

    // 工作指针，只有逻辑线程会访问
    RenderSnapshot* m_working{ nullptr };

    // 原子指针，指向最新写完且可供读取的缓冲区
    std::atomic<RenderSnapshot*> m_completed{ nullptr };

    // 原子标记，指示 completed 是否包含未被 UI 消费的新数据
    std::atomic<bool> m_hasNew{ false };

    // UI 线程上次拿到的读取缓冲区
    // 当 UI 请求新的，就把这个老的换回 m_completed 给逻辑线程重复利用
    RenderSnapshot* m_reading{ nullptr };
};

}  // namespace MMM::Logic
