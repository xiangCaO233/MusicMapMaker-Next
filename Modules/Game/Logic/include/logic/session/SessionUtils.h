#pragma once

#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include <entt/entt.hpp>

namespace MMM::Logic::SessionUtils
{

/// @brief 判断给定视口是否为主编辑画布。
bool isMainCanvasCameraId(const std::string& cameraId);

/// @brief 在视口表中查找当前会话的主编辑画布。
const CameraInfo* findMainCanvasCamera(
    const std::unordered_map<std::string, CameraInfo>& cameras);

/// @brief 磁吸计算结果数据结构
struct SnapResult {
    bool   isSnapped{ false };  ///< 是否触发了磁吸
    double snappedTime{ 0.0 };  ///< 磁吸后的逻辑时间
    int    numerator{ 0 };      ///< 分子
    int    denominator{ 1 };    ///< 分母
};

/// @brief 获取给定坐标的磁吸时间结果
/// @param rawTime 原始逻辑时间
/// @param mouseY 鼠标的 Y 轴坐标
/// @param camera 当前视口信息
/// @param config 编辑器配置
/// @param bpmEvents 全局 BPM 事件列表
/// @param timelineRegistry 时间轴注册表
/// @param visualTime 当前视觉时间
/// @param cameras 所有的相机视口字典
/// @warning
/// 逻辑热路径：鼠标悬停、绘制和拖拽物件时会频繁调用；禁止在此函数中加入文件系统访问、完整
/// ECS 遍历、完整排序或 try/catch。
/// @return SnapResult 磁吸计算结果
SnapResult getSnapResult(
    double rawTime, float mouseY, const CameraInfo& camera,
    const Config::EditorConfig&                  config,
    const std::vector<const TimelineComponent*>& bpmEvents,
    entt::registry& timelineRegistry, double visualTime,
    const std::unordered_map<std::string, CameraInfo>& cameras);

/// @brief 根据当前视觉时间同步打击事件的索引
/// @param ctx 会话上下文引用
/// @warning 逻辑热路径：播放、Seek
/// 和滚动时调用；普通路径只二分查找，音符变更后的 脏分支会先重建 hitEvents。
void syncHitIndex(SessionContext& ctx);

/// @brief 确保 BPM 事件缓存已按时间排序并可直接用于热路径计算。
/// @param ctx 会话上下文引用
/// @warning 逻辑热路径脏分支：只有时间线变更后才允许完整遍历和排序 Timing
/// ECS，普通绘制/滚动/渲染路径必须复用缓存结果。
void ensureBpmEvents(SessionContext& ctx);

/// @brief 标记打击事件序列需要在下次播放/索引消费前重建。
/// @param ctx 会话上下文引用
void markHitEventsDirty(SessionContext& ctx);

/// @brief 确保打击事件序列已根据当前音符 ECS 重建。
/// @param ctx 会话上下文引用
/// @warning 逻辑热路径脏分支：只在音符变更后第一次播放、Seek
/// 或索引同步前重建，禁止每次编辑 action 无条件调用。
void ensureHitEvents(SessionContext& ctx);

/// @brief 全量重建谱面的打击事件序列
/// @param ctx 会话上下文引用
void rebuildHitEvents(SessionContext& ctx);

/// @brief 载入新的谱面数据到上下文中
/// @param ctx 会话上下文引用
/// @param beatmap 指向要载入的谱面指针
void loadBeatmap(SessionContext& ctx, std::shared_ptr<MMM::BeatMap> beatmap);

/// @brief 将上下文中的 ECS 实体数据序列化写回到谱面对象中
/// @param ctx 会话上下文引用
void syncBeatmap(SessionContext& ctx);

}  // namespace MMM::Logic::SessionUtils
