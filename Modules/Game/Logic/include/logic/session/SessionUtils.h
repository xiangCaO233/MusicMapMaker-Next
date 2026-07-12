#pragma once

#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include <entt/entt.hpp>
#include <filesystem>

namespace MMM
{
class Project;
}

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
/// @param animateTime 当前动画渲染时间。
/// @param cameras 所有的相机视口字典
/// @param fallbackBpm 无效 BPM 事件使用的回退 BPM。
/// @warning
/// 逻辑热路径：鼠标悬停、绘制和拖拽物件时会频繁调用；禁止在此函数中加入文件系统访问、完整
/// ECS 遍历、完整排序或 try/catch。
/// @return SnapResult 磁吸计算结果
SnapResult getSnapResult(
    double rawTime, float mouseY, const CameraInfo& camera,
    const Config::EditorConfig&                  config,
    const std::vector<const TimelineComponent*>& bpmEvents,
    entt::registry& timelineRegistry, double animateTime,
    const std::unordered_map<std::string, CameraInfo>& cameras,
    double                                             fallbackBpm);

/// @brief 根据当前动画时间同步打击事件的索引。
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

/// @brief 获取当前 Session 的有效总时长。
/// @param ctx 会话上下文引用。
/// @return 音频总时长和谱面元数据总时长中的较大值，单位秒。
/// @warning 逻辑/UI 热路径：播放、seek clamp 和快照生成会调用；只允许读取
/// AudioManager 当前缓存时长和 currentBeatmap 元数据，禁止加入文件系统访问。
double getEffectiveTotalTimeSeconds(const SessionContext& ctx);

/// @brief 解析当前谱面主音频的可访问文件路径。
/// @param ctx 会话上下文引用。
/// @param project 当前项目；为空时回退到谱面文件所在目录。
/// @return 优先返回存在的音频路径；没有可访问文件时返回最佳候选路径。
/// @warning 低频资源路径解析：会访问文件系统和项目资源表，仅允许在载入谱面、
/// 播放状态切换、切换画布或元数据变更时调用，禁止放入每帧 update。
std::filesystem::path resolveMainAudioPath(const SessionContext& ctx,
                                           const ::MMM::Project* project);

/// @brief 根据当前背景类型探测并更新会话中的背景原始尺寸。
/// @param ctx 目标会话上下文。
/// @param metadata 待探测的谱面基础元数据。
/// @param project 当前项目；为空时从谱面目录解析资源。
/// @warning 低频资源加载路径：会访问文件系统并打开图片或视频，
/// 仅允许在谱面加载和背景元数据变更时调用。
void updateBackgroundSize(SessionContext& ctx, const MMM::BaseMapMeta& metadata,
                          const ::MMM::Project* project);

/// @brief 载入新的谱面数据到上下文中
/// @param ctx 会话上下文引用
/// @param beatmap 指向要载入的谱面指针
void loadBeatmap(SessionContext& ctx, std::shared_ptr<MMM::BeatMap> beatmap);

/// @brief 将上下文中的 ECS 实体数据序列化写回到谱面对象中
/// @param ctx 会话上下文引用
void syncBeatmap(SessionContext& ctx);

}  // namespace MMM::Logic::SessionUtils
