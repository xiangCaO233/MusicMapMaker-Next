#pragma once

#include "config/EditorConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
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

        bool operator<(const HitEvent& other) const
        {
            return timestamp < other.timestamp;
        }
    };

    /**
     * @brief 更新打击特效状态
     * @param visualTime 当前视觉时间
     * @param events 本帧新触发的打击事件
     * @param config 编辑器配置
     */
    void update(double visualTime, const std::vector<HitEvent>& events,
                const Config::EditorConfig& config);

    /**
     * @brief 触发音效（仅音效，带预测支持）
     */
    void triggerAudio(const HitEvent& ev, const Config::EditorConfig& config);

    /**
     * @brief 触发视觉特效
     */
    void triggerVisual(const HitEvent& ev, const Config::EditorConfig& config);

    /**
     * @brief 生成打击特效的渲染指令
     * @param snapshot 目标渲染快照
     * @param visualTime 当前视觉时间
     * @param config 编辑器配置
     * @param trackCount 总轨道数
     * @param judgmentLineY 判定线 Y 坐标
     * @param leftX 轨道区域左边界
     * @param singleTrackW 单个轨道宽度
     */
    void generateSnapshot(Batcher& batcher, double visualTime,
                          const Config::EditorConfig& config,
                          int32_t trackCount, float judgmentLineY, float leftX,
                          float singleTrackW);

    /**
     * @brief 清空当前所有正在播放的特效
     * 通常在时间跳转（Seek）时调用，防止历史特效残留在未来或过去的时间点。
     */
    void clearActiveEffects() { m_trackActiveEffects.clear(); }

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

    // Hold 类型的特效 (由于 Hold 可能会跨越很久，且可能同时有多个，单独追踪)
    // 实际上对于同一个轨道，Hold 也会被后面的 Note 覆盖
};

}  // namespace MMM::Logic::System
