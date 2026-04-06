#pragma once

#include "config/EditorConfig.h"
#include <entt/entt.hpp>

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
     */
    static void update(entt::registry& registry,
                       entt::registry& timelineRegistry, double currentTime,
                       const Config::EditorConfig& config);
};

}  // namespace MMM::Logic::System
