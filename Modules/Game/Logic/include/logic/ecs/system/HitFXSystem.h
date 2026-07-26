#pragma once

#include "audio/StereoGainEnvelope.h"
#include "config/EditorConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::Logic::System
{
struct Batcher;


/**
 * @brief 打击音效与视觉特效系统
 * 负责处理音符到达判定线时的音效播放和序列帧动画显示。
 */
class HitFXSystem
{
public:
    struct HitEvent {
        double          timestamp;
        ::MMM::NoteType type;
        enum class Role { None, Head, Internal, Tail } role;
        int    trackSpan;
        int    trackIndex;
        int    trackOffset{ 0 };  // 用于 Flick 等偏移物件
        double duration;
        bool   isSubNote;
        /// @brief 物件绑定的自定义音效资源标识；为空时使用内置音效。
        std::string boundSound;

        bool operator<(const HitEvent& other) const
        {
            return timestamp < other.timestamp;
        }
    };

    // clang-format off
    /**
     * @brief 更新打击特效状态
     * @param animateTime 当前动画时间
     * @param events 本帧新触发的打击事件
     * @param trackCount 当前谱面轨道数量
     * @param config 编辑器配置
     */
    /// @warning 逻辑热路径：每个 Session update 执行。
    /// 只处理本帧事件和当前活跃特效，禁止文件系统访问和完整 ECS 遍历。
    // clang-format on
    void update(double animateTime, const std::vector<HitEvent>& events,
                std::int32_t trackCount, const Config::EditorConfig& config);

    /// @brief 触发音效（仅音效，带预测支持）。
    /// @param ev 待播放的物件打击事件。
    /// @param trackCount 当前谱面的总轨道数。
    /// @param config 当前编辑器配置。
    /// @warning 逻辑预测播放热路径：每个待触发物件调用一次，只允许固定计算和
    /// 音效池调度，禁止文件访问或阻塞等待。
    void triggerAudio(const HitEvent& ev, std::int32_t trackCount,
                      const Config::EditorConfig& config);

    /// @brief 计算物件中心对应的双声道增益包络。
    /// @param ev 待定位的物件打击事件。
    /// @param trackCount 当前谱面的总轨道数。
    /// @param enabled 是否启用立体打击音效。
    /// @return 普通物件为固定增益，Flick
    /// 为起点到滑动终点的线性增益包络；关闭时保持原始立体声。
    /// @warning 逻辑预测播放热路径：仅执行常量时间算术，不得访问 ECS 或分配。
    [[nodiscard]] static Audio::StereoGainEnvelope stereoGainEnvelopeForEvent(
        const HitEvent& ev, std::int32_t trackCount, bool enabled);

    /// @brief 解析打击事件实际使用的音效资源标识。
    /// @param ev 待解析的打击事件。
    /// @param effectiveType 已应用折线音效策略后的物件类型。
    /// @return 自定义绑定存在时返回绑定资源，否则返回对应内置音效资源。
    /// @warning 逻辑预测播放热路径：只返回稳定字符串引用，不得分配或访问文件。
    [[nodiscard]] static const std::string& soundEffectKeyForEvent(
        const HitEvent& ev, ::MMM::NoteType effectiveType);

    /**
     * @brief 触发视觉特效
     */
    void triggerVisual(const HitEvent& ev, const Config::EditorConfig& config);

    // clang-format off
    /**
     * @brief 生成打击特效的渲染指令
     * @param snapshot 目标渲染快照
     * @param animateTime 当前动画时间
     * @param config 编辑器配置
     * @param trackCount 总轨道数
     * @param judgmentLineY 判定线 Y 坐标
     * @param leftX 轨道区域左边界
     * @param singleTrackW 单个轨道宽度
     */
    /// @warning 渲染热路径：快照生成阶段执行。
    /// 只遍历当前活跃特效表并追加几何。
    // clang-format on
    void generateSnapshot(Batcher& batcher, double animateTime,
                          const Config::EditorConfig& config,
                          int32_t trackCount, float judgmentLineY, float leftX,
                          float singleTrackW);

    /**
     * @brief 清空当前所有正在播放的特效与 KPS 滚动窗口。
     * 通常在时间跳转（Seek）时调用，防止历史特效和触发统计残留。
     */
    void clearActiveEffects();

    /// @brief 取得最近一秒内每条轨道实际消费的 HitEffect 事件数量。
    /// @return 下标与轨道序号一致的只读 KPS 视图。
    /// @warning 渲染热路径读取；返回非拥有视图，不得跨越下一次 update 使用。
    [[nodiscard]] std::span<const std::uint32_t> trackKps() const
    {
        return m_trackKps;
    }

private:
    struct ActiveEffect {
        double      startTime{ 0.0 };
        double      duration{ 0.0 };
        int         trackIndex{ 0 };
        int         trackSpan{ 1 };
        int         trackOffset{ 0 };  // 用于定位打击点（如 Flick 箭头）
        bool        isLooping{ false };
        std::string effectKey;  // "note" or "flick"

        // 动画是否已播放完成
        bool isFinished(double currentTime, float baseFps,
                        size_t frameCount) const;
    };

    // 每个轨道当前激活的特效 (用于新特效覆盖旧特效)
    std::unordered_map<int, ActiveEffect> m_trackActiveEffects;

    /// @brief 最近一秒滚动窗口内的单个已消费打击事件。
    struct RecentHitEvent {
        /// @brief 事件谱面时间。
        double timestamp{ 0.0 };
        /// @brief 事件所属轨道。
        std::int32_t trackIndex{ 0 };
    };

    /// @brief 最近一秒内按时间排序的已消费打击事件。
    std::deque<RecentHitEvent> m_recentHitEvents;
    /// @brief 与当前轨道数量一致的逐轨 KPS 缓存。
    std::vector<std::uint32_t> m_trackKps;
    /// @brief 上一次更新 KPS 的动画时间，用于识别反向跳转。
    double m_lastKpsTime{ -1.0 };

    /// @brief 更新逐轨 KPS 的一秒滚动窗口。
    /// @param animateTime 当前动画时间。
    /// @param events 本帧实际消费的 HitEffect 事件。
    /// @param trackCount 当前谱面轨道数量。
    /// @warning 逻辑热路径：每个 Session update 执行；只处理新增和过期事件。
    void updateKps(double animateTime, const std::vector<HitEvent>& events,
                   std::int32_t trackCount);

    /// @brief 清空逐轨 KPS 滚动窗口并保留已分配容量。
    void clearKps();

    // Hold 类型的特效 (由于 Hold 可能会跨越很久，且可能同时有多个，单独追踪)
    // 实际上对于同一个轨道，Hold 也会被后面的 Note 覆盖
};

}  // namespace MMM::Logic::System
