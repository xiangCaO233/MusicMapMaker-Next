#pragma once

#include "config/EditorSettings.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include <entt/entt.hpp>
#include <filesystem>
#include <span>
#include <string_view>

namespace MMM
{
class Project;
}

namespace MMM::Logic::SessionUtils
{

/// @brief 一次音符变更对增量缓存维护暴露的前后状态。
struct NoteCacheMutationView {
    /// @brief 变更对应的 ECS 实体。
    entt::entity entity{ entt::null };
    /// @brief 变更前的音符组件；创建时为空。
    const NoteComponent* before{ nullptr };
    /// @brief 变更后的音符组件；删除时为空。
    const NoteComponent* after{ nullptr };
};

/// @brief 直接把少量音符变更合并进排序、统计、密度与打击事件缓存。
/// @param ctx 当前会话上下文。
/// @param mutations 已完成 ECS 写入的音符变更集合。
/// @return 现有缓存可安全增量维护时返回 true；需要调用方回退全量脏标记时
/// 返回 false。
/// @warning 逻辑编辑热路径：单次放置和联机差量落地时调用；只允许按变更数
/// 排序，并从最早受影响位置修补前缀，禁止完整 ECS 遍历或完整全谱排序。
bool applyNoteCacheMutationsIncrementally(
    SessionContext& ctx, std::span<const NoteCacheMutationView> mutations);

/// @brief 将一个新建的普通正式音符直接追加到当前领域谱面。
/// @param ctx 当前会话上下文。
/// @param note 已写入 ECS 且具有稳定协作标识的音符组件。
/// @return 普通 Note、Hold 或 Flick 成功增量写回时返回 true。
/// @warning 逻辑编辑热路径：单次放置调用；只允许追加一个领域对象并在引用索引
/// 中二分插入，禁止完整 ECS 遍历或整谱重建。
bool syncCreatedNoteToBeatmap(SessionContext& ctx, const NoteComponent& note);

/// @brief 判断音符是否允许在当前折线编辑模式下响应编辑操作。
/// @param note 待判断的音符组件。
/// @param settings 当前编辑器行为设置。
/// @return 折线编辑开启时返回 true；关闭时仅主 Note 和 Hold 返回 true。
/// @warning 逻辑与渲染热路径：只读取组件类型和设置布尔值。
bool isNoteEditable(const NoteComponent&          note,
                    const Config::EditorSettings& settings);

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

/// @brief 按物件放置磁吸配置计算单个 BPM 段内最近的分拍线。
/// @param rawTime 原始逻辑时间。
/// @param timingTime 当前 BPM 段起始时间。
/// @param nextTimingTime 下一 BPM 段起始时间；末段可传正无穷。
/// @param bpm 当前 BPM 值。
/// @param settings 编辑器行为设置。
/// @return 已启用且存在合法分拍候选时返回磁吸结果，否则返回未磁吸结果。
/// @warning 逻辑热路径：绘制、移动和悬停预览会频繁调用；仅允许固定上限循环。
SnapResult calculateObjectPlacementSnap(double rawTime, double timingTime,
                                        double nextTimingTime, double bpm,
                                        const Config::EditorSettings& settings);

/// @brief 计算指定时间点从首个 BPM Timing 起算的拍号。
/// @param time 待查询的谱面时间，单位秒。
/// @param bpmEvents 已按时间排序的 BPM 事件列表。
/// @param fallbackBpm 无效 BPM 事件使用的回退 BPM。
/// @return 从 1 开始的拍号；时间早于首个 BPM Timing 或无 BPM 事件时返回 0。
/// @warning
/// 逻辑热路径：状态快照和画布悬浮信息会频繁调用；只允许线性扫描已缓存的 BPM
/// 事件，禁止在此函数中遍历 ECS 或执行排序。
int calculateBeatIndex(double                                       time,
                       const std::vector<const TimelineComponent*>& bpmEvents,
                       double fallbackBpm);

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

/// @brief 计算玩家物件和 Timing 共同决定的谱面内容结束时间。
/// @param beatMap 待读取的谱面领域对象。
/// @return 不包含自动采样长度的非负结束时间，单位秒。
/// @warning 低频描述符构建路径：会遍历完整 Note/Timing 集合，禁止在每帧
/// update 热路径直接调用。
double calculateChartContentEndSeconds(const MMM::BeatMap& beatMap);

/// @brief 重建当前会话的复合音频时间线描述符。
/// @param ctx 目标会话上下文。
/// @param project 当前项目；为空时仅生成缺失资源诊断。
/// @return 描述符指纹是否发生变化。
/// @warning 低频脏分支：会遍历采样、解析文件路径并排序，禁止无脏标记调用。
bool rebuildAudioTimelineDescriptor(SessionContext&       ctx,
                                    const ::MMM::Project* project);

/// @brief 将当前会话描述符提交为全局唯一活动音频时间线。
/// @param ctx 目标会话；仅 isActiveSession 为 true 时允许修改 AudioManager。
/// @param shouldPlay 提交和 Seek 后是否立即播放。
/// @return 时间线已复用或成功加载时返回 true。
/// @warning 低频激活路径：可能同步解码全部引用资源，禁止每帧无条件调用。
bool activateAudioTimeline(SessionContext& ctx, bool shouldPlay);

/// @brief 多标签切换时可独立测试的时间和播放态决策。
struct AudioTimelineSwitchDecision {
    /// @brief 目标会话应采用的时间。
    double m_targetTime{ 0.0 };
    /// @brief 切换后是否恢复播放。
    bool m_resumePlayback{ false };
};

/// @brief 根据完整时间线指纹决定多标签切换语义。
/// @return 仅同指纹时同步旧时间并按配置恢复播放；不同指纹保留目标状态。
AudioTimelineSwitchDecision resolveAudioTimelineSwitch(
    std::string_view previousFingerprint, std::string_view targetFingerprint,
    double previousTime, double targetTime, bool previousWasPlaying,
    bool stopPlaybackOnScroll, bool synchronizeMatchingTimelines);

/// @brief 将 AudioManager transport 快照应用为会话唯一播放时钟。
/// @param ctx 接收状态的会话。
/// @param loadedFingerprint 当前 AudioManager 已加载指纹。
/// @param snapshot 位置、状态和发布时间一致的音频时钟快照。
/// @param nowSteadySeconds 当前逻辑帧的 steady_clock 秒数。
/// @param syncConfig 音画校准配置。
/// @return 应继续处理播放态动画和 HitFX 时返回 true。
/// @warning 逻辑热路径：只比较缓存字符串并更新常量级会话状态。
bool applyAudioTimelineTransportSnapshot(
    SessionContext& ctx, std::string_view loadedFingerprint,
    const Audio::AudioTimelineClockSnapshot& snapshot, double nowSteadySeconds,
    const Config::SyncConfig& syncConfig);

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
