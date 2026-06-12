#pragma once

#include "config/EditorConfig.h"
#include <entt/entt.hpp>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic::System
{

/**
 * @brief 音符坐标计算系统
 *
 * 此系统通过 ScrollCache 将音符的 timestamp/duration 映射到相对 Y 坐标与高度。
 */
class NoteTransformSystem
{
public:
    /**
     * @brief 更新逻辑坐标 (TransformComponent.pos.y)
     *
     * @param registry 音符注册表
     * @param timelineRegistry 时间线注册表 (用于获取 ScrollCache)
     * @param currentTime 当前播放时间
     * @param config 编辑器配置
     * @param beatmap 当前 Session 绑定的谱面；为空时使用保守默认值。
     * @warning 逻辑热路径：由 BeatmapSession update 调用；完整 registry view
     * 遍历只能在缓存脏或强制重建时执行，禁止在此处排序。
     */
    static void update(entt::registry& registry,
                       entt::registry& timelineRegistry, double currentTime,
                       const Config::EditorConfig& config,
                       MMM::BeatMap* beatmap, bool forceRebuild = false);
};

}  // namespace MMM::Logic::System
