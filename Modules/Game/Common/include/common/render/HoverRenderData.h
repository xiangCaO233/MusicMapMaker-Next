#pragma once

#include "common/ChartObjectKind.h"
#include <cmath>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <string>

namespace MMM::Common::Render
{
using Logic::ChartObjectKind;

/// @brief 拾取包围盒对应的可交互物件部位。
/// @note 部位用于选择拖动语义，具体物件领域另由 ChartObjectKind 表示。
enum class HoverPart : uint8_t {
    None = 0,
    Head,
    HoldBody,
    HoldEnd,
    FlickArrow,
    PolylineNode,
    SampleAnchor,
    SampleOffset
};

/// @brief UI 悬浮检视面板展示的细分物件结构类型。
/// @note 该枚举比 HoverPart 更细，用于选择不同字段和提示文案。
enum class HoverInspectKind : uint8_t {
    None = 0,
    Note,
    HoldHead,
    HoldBody,
    HoldEnd,
    FlickHead,
    FlickBody,
    FlickEnd,
    PolylineHead,
    PolylineNode,
    PolylineHoldBody,
    PolylineHoldEnd,
    PolylineFlickBody,
    PolylineFlickEnd,
    AudioSampleAnchor,
    AudioSampleTrigger
};

/// @brief 悬浮部位对应的拍位、时间和轨道信息。
struct HoverBeatPoint {
    /// @brief 是否显示该部位的拍位与时间信息
    bool show{ false };
    /// @brief 部位所在拍号
    int beatIndex{ 0 };
    /// @brief 部位所在分拍分子
    int numerator{ 0 };
    /// @brief 部位所在分拍分母
    int denominator{ 1 };
    /// @brief 部位精确时间戳，单位秒
    double time{ 0.0 };
    /// @brief 部位所在拍的起点时间，单位秒。
    double beatStartTime{ 0.0 };
    /// @brief 部位所在拍的终点时间，单位秒。
    double beatEndTime{ 0.0 };
    /// @brief 当前 BPM 下完整一拍的名义时长，单位秒。
    double beatDuration{ 0.0 };
    /// @brief 部位所在轨道
    int32_t track{ 0 };
};

/// @brief 当前悬浮对象交给 UI 展示的结构化只读快照。
struct HoverInspectInfo {
    /// @brief 是否显示结构化悬浮检视信息
    bool show{ false };
    /// @brief 当前检视物件实体。
    entt::entity entity{ entt::null };
    /// @brief 当前检视物件所在的独立 ECS 注册表。
    ChartObjectKind objectKind{ ChartObjectKind::PlayerNote };
    /// @brief 当前悬浮部位类型
    HoverInspectKind kind{ HoverInspectKind::None };
    /// @brief Head 部位信息
    HoverBeatPoint head;
    /// @brief Body 部位信息
    HoverBeatPoint body;
    /// @brief End 部位信息
    HoverBeatPoint end;
    /// @brief 是否显示持续时间
    bool showDuration{ false };
    /// @brief 持续时间，单位秒
    double duration{ 0.0 };
    /// @brief 是否显示滑动轨道数
    bool showDtrack{ false };
    /// @brief Flick 滑动轨道数
    int32_t dtrack{ 0 };
    /// @brief 是否显示轨道位置
    bool showTrack{ false };
    /// @brief 当前部位轨道位置
    int32_t track{ 0 };

    /// @brief 是否显示自动采样资源信息。
    bool showAudioSample{ false };
    /// @brief 当前物件是否存在可独立试听的项目音频引用。
    bool showAudioPreview{ false };
    /// @brief 自动采样引用的项目音频资源 ID。
    std::string audioResourceId;
    /// @brief 自动采样物件音量。
    float volume{ 1.0F };
    /// @brief 当前试听绑定所属的 Polyline 子物件索引；负值表示物件本体。
    std::int32_t sampleBindingSubIndex{ -1 };
    /// @brief 自动采样相对锚点的有符号播放偏移，单位毫秒。
    std::int64_t offsetMs{ 0 };

    /// @brief 当前悬浮位置按重叠检测规则命中的物件数量。
    int overlapCount{ 1 };
};

/// @brief 悬浮检视或编辑手势触发的单轨单拍临时分拍预览。
struct HoverSubdivisionPreview {
    /// @brief 是否需要替换目标轨道当前拍内的分拍线。
    bool show{ false };
    /// @brief 临时预览所属的玩家轨道。
    int32_t track{ 0 };
    /// @brief 悬浮部件在目标拍内的最简分拍分子。
    int numerator{ 0 };
    /// @brief 临时预览使用的分拍分母。
    int denominator{ 1 };
    /// @brief 常用分拍线并集位掩码；为零时仅绘制 denominator 对应分拍线。
    std::uint32_t commonBeatDivisorMask{ 0U };
    /// @brief 触发临时预览的物件部件时间，单位秒。
    double focusTime{ 0.0 };
    /// @brief 目标拍起点时间，单位秒。
    double beatStartTime{ 0.0 };
    /// @brief 目标拍终点时间，单位秒。
    double beatEndTime{ 0.0 };
    /// @brief 生成临时分拍线所用的完整拍时长，单位秒。
    double beatDuration{ 0.0 };
};

/// @brief 用于 UI 碰撞拾取的轴对齐包围盒。
struct Hitbox {
    /// @brief 包围盒所属 ECS 实体。
    entt::entity entity;
    /// @brief 包围盒代表的物件部位。
    HoverPart part{ HoverPart::None };
    /// @brief Polyline 子物件或其他细分部位索引；负值表示物件本体。
    int subIndex{ -1 };
    /// @brief 包围盒左上角横坐标。
    float x;
    /// @brief 包围盒左上角纵坐标。
    float y;
    /// @brief 包围盒宽度。
    float w;
    /// @brief 包围盒高度。
    float h;
    /// @brief 实体所在的独立 ECS 注册表。
    ChartObjectKind kind{ ChartObjectKind::PlayerNote };
};

/// @brief 以包围盒中心为基准缩放交互拾取区域。
/// @param hitbox 原始渲染几何对应的包围盒。
/// @param scaleX 横向缩放；非正数或非有限值回退为 1。
/// @param scaleY 纵向缩放；非正数或非有限值回退为 1。
/// @return 保留实体与部件信息的缩放后包围盒。
/// @warning UI 与调试渲染热路径：每个候选框调用，只做常量级算术且不分配。
[[nodiscard]] inline Hitbox scaleInteractionHitbox(const Hitbox& hitbox,
                                                   float         scaleX,
                                                   float scaleY) noexcept
{
    // 非正或非有限缩放不具备有效几何意义，按单位缩放处理。
    const float safeScaleX =
        std::isfinite(scaleX) && scaleX > 0.0F ? scaleX : 1.0F;
    const float safeScaleY =
        std::isfinite(scaleY) && scaleY > 0.0F ? scaleY : 1.0F;
    if ( safeScaleX == 1.0F && safeScaleY == 1.0F ) {
        // 单位缩放直接返回原对象，避免执行无意义的中心重算。
        return hitbox;
    }
    // 先保存原包围盒中心，缩放后再围绕相同中心恢复左上角。
    const float centerX = hitbox.x + hitbox.w * 0.5F;
    const float centerY = hitbox.y + hitbox.h * 0.5F;

    Hitbox scaled = hitbox;
    scaled.w      = hitbox.w * safeScaleX;
    scaled.h      = hitbox.h * safeScaleY;
    scaled.x      = centerX - scaled.w * 0.5F;
    scaled.y      = centerY - scaled.h * 0.5F;
    // 极端输入若导致任一结果溢出，回退完整原包围盒而非返回部分坏值。
    if ( !std::isfinite(scaled.x) || !std::isfinite(scaled.y) ||
         !std::isfinite(scaled.w) || !std::isfinite(scaled.h) ) {
        return hitbox;
    }
    return scaled;
}

}  // namespace MMM::Common::Render
