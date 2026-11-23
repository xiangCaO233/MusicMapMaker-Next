#pragma once

#include "mmm/annotation/BeatmapAnnotation.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MMM::Logic
{

/// @brief 批注目标玩家物件的导出类型；无目标或目标缺失时为 None。
enum class AnnotationExportObjectType { None, Note, Hold, Flick, Polyline };

/// @brief 批注表中一条已经解析到实际谱面位置的独立数据行。
struct AnnotationTableRow {
    /// @brief 批注实际展示时间，单位秒。
    double timestamp{ 0.0 };

    /// @brief 批注稳定标识。
    std::string id;

    /// @brief 批注目标类型。
    ::MMM::BeatmapAnnotationTargetKind targetKind{
        ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP
    };

    /// @brief 目标物件轨道；独立时间戳或目标丢失时为 -1。
    std::int32_t track{ -1 };

    /// @brief 面向导出的目标起始轨道；音频采样已转换为 BGM 区内零基编号。
    std::int32_t exportTrack{ -1 };

    /// @brief 滑键或折线终点的零基轨道；其它对象为 -1。
    std::int32_t exportEndTrack{ -1 };

    /// @brief 目标玩家物件的种类，音频采样和时间标记使用 None。
    AnnotationExportObjectType objectType{ AnnotationExportObjectType::None };

    /// @brief 目标是否指向折线内嵌子段，而非整条折线。
    bool isPolylineSubNote{ false };

    /// @brief 目标物件是否已经不存在。
    bool targetMissing{ false };

    /// @brief 批注作者。
    std::string author;

    /// @brief Markdown 正文。
    std::string content;
};

/// @brief 批位文本需要的标准化 BPM 锚点。
struct AnnotationTableBpmPoint {
    /// @brief 节点时间，单位秒。
    double time;
    /// @brief 经过归一化的有效 BPM。
    double bpm;
};

/// @brief 批注表时间格式所需的完整一代数据。
struct AnnotationTableTiming {
    /// @brief 按时间递增、已去重的 BPM 锚点。
    std::vector<AnnotationTableBpmPoint> bpmPoints;
    /// @brief 节点和分拍数同时发布，防止同一行使用不同代的格式参数。
    int beatDivisor{ 4 };
};

/// @brief 一次逻辑侧刷新发布的只读结果，内部不保留 Session、谱面或实体。
struct AnnotationTableSnapshot {
    /// @brief 当前载入实例的唯一标识，零表示会话中没有谱面。
    std::uintptr_t instanceId{};
    /// @brief 批注修订变化时替换整批行，播放和 Timing 状态变化可复用行。
    std::shared_ptr<const std::vector<AnnotationTableRow>> rows;
    /// @brief 滚动缓存准备就绪后发布；为空时使用默认时间格式。
    std::shared_ptr<const AnnotationTableTiming> timing;
    /// @brief 当前谱面是否处于播放状态，播放期间只允许查看。
    bool playing{};
};

/// @brief 从同一代注册表选取的活动谱面及其最后一次批注表发布。
struct ActiveAnnotationTableRead {
    /// @brief 活动画布的稳定标识；切页等待期间不得显示另一画布的旧行。
    std::string cameraId;
    /// @brief 首次请求发布之前为空，之前发布的完整快照可继续保活。
    std::shared_ptr<const AnnotationTableSnapshot> snapshot;
    /// @brief 采样即时的协作权限，离线后的限制无需等待下一次投影。
    bool offlineReadOnly{};
};

/// @brief 请求下一次逻辑刷新，并返回当前活动谱面的最近一次完整发布。
/// @return 无活动谱面会话时为空；有会话但首次未发布时 snapshot 为空。
/// @warning UI 低频轮询，只取得注册表和批注快照的短时共享句柄；不获取
/// 会话更新锁，也不解引用可变谱面。不要放入逐行绘制路径。
std::optional<ActiveAnnotationTableRead> readActiveAnnotationTable();

}  // namespace MMM::Logic
