#pragma once
#include "MobileCanvasGesture.h"
#include "common/render/RenderSnapshot.h"
#include "config/skin/translation/Translation.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <initializer_list>
#include <optional>

namespace MMM::Canvas
{
/// @brief 已选插值段落的移动首尾事务，不改变原标记移动或桌面命中。
/// @details 起笔捕获两个端点；预览只改范围，原曲线作为未提交参考保留。
/// 普通时间点没有持续范围，不提供把手，也不借用段落末端作为独立实体。
/// 本类只借用当前快照，释放的正式更新仍由原时间线命令入口提交。
/// @par 事务边界
/// 选择工具先点选首标记，第二笔抓取端点，标记中央仍可整体移动。
/// 首把手在泳道左侧，尾把手在右侧，短范围也不需要键盘切换部件。
/// 微小抖动是轻点，长按把手不打开正文菜单，越过容差才改变候选范围。
/// 复制的是段落原定义，不是整个谱面，也不展开任何导出采样点。
/// 非法候选保持原模型；取消不借助历史栈，之前的重做不受影响。
/// 已发布段落被改写后立即失效，不能在释放时用旧定义覆盖协作变更。
/// 范围预览使用常量线段，不在拖动中重建函数、拍轴或曲线点缓存。
/// @par 时间契约
/// 时间与时长都是秒，抓取偏置是屏幕像素，必须在调用者投影后转换。
/// 端点互换会改变函数方向，因此只夹取最小时长，永不交换起终值。
/// 最终吸附仍走原时间线助手，不以主画布 BPM 或像素近似替代。
/// 原助手的临时容器只在离散释放边沿使用，不扩大到逐帧预览路径。
/// @warning 每次按下复制两份插值定义以锁定不可变函数／拍轴缓存生命周期；
/// 仅此低频边沿增减共享所有权，连续帧不复制 shared_ptr、不分配或等待。
class MobileTimingRange
{
public:
    /// @brief 平台中立的段落值类型，不增加新的文件格式或实体种类。
    using Element = Common::Render::TimingInterpolationElement;
    // 平台只描述手指意图，不向原生枚举增加特殊 TimingEffect 值。
    /// @brief 首尾有不同横向锚点，短段也能明确选择要调整的边界。
    enum class Part { None, Start, End };
    /// @brief 放弃候选与预览，不访问历史或调用 Undo。
    /// @warning 取消可在遮罩帧重复调用，首次释放后只检查空 optional。
    void cancel()
    {
        // 空 optional 可以重复清理；没有活动候选时不访问引用计数或模型。
        // 清理不等价于正常释放，不能由析构或遮罩分支提交最后位置。
        m_source.reset();
        m_preview.reset();
        m_part    = Part::None;
        m_started = m_moved = false;
    }
    /// @brief 待定轻点也持有端点身份，长按不能转交正文菜单。
    /// @warning 每帧只查询本地标量，不借用会话或原生事件队列。
    bool pending() const { return m_source.has_value(); }
    /// @brief 当前预览只在本帧借用，不向其它窗口传递拥有型引用。
    /// @warning 每帧返回稳定本地观察指针，不增加共享所有权。
    const Element* preview() const { return m_preview ? &*m_preview : nullptr; }
    /// @brief 每次真实按下锁定端点，抓取偏移避免手指在热区边缘起手时跳动。
    /// @param segment 当前快照中的完整段落，实体句柄保留代数。
    /// @param part 按下时选定的首或尾，拖动中不能重新命中其它端点。
    /// @param grabOffset 边界与触点间的纵向像素偏置。
    /// @param originY 真实按下的屏幕纵坐标，回到此处恢复精确原范围。
    void latch(const Element& segment, Part part, float grabOffset,
               float originY)
    {
        cancel();
        if ( part == Part::None || !std::isfinite(segment.time) ||
             segment.time < 0 ||
             !std::isfinite(segment.interpolation.m_duration) ||
             segment.interpolation.m_duration < .001 ||
             !std::isfinite(segment.time + segment.interpolation.m_duration) )
            return;
        // 原定义和可写候选分离，预览绝不反写只读快照中的函数缓存。
        // 此处是唯一的拥有型复制边沿，后续只改两个 double。
        // 非法快照无法建立有序夹取区间，必须在使用 std::clamp 前拒绝。
        m_source     = segment;
        m_preview    = segment;
        m_part       = part;
        m_grabOffset = grabOffset;
        m_originY    = originY;
    }
    /// @brief 拒绝协作改写或删除后的旧定义，不把起笔前状态覆盖回新快照。
    /// @warning 活动帧仅比较原目标，缓存指针不复制，也不读取 Registry。
    bool matches(const Common::Render::RenderSnapshot& snapshot) const
    {
        // 未起笔时没有需要校验的目标；全量快照换页并不要求重建解释器。
        // 按语义比较定义而非缓存地址，重新发布相同函数不会误取消手势。
        if ( !m_source ) return true;
        for ( const auto& s : snapshot.timingInterpolations )
            if ( s.entity == m_source->entity )
                return s.time == m_source->time && s.value == m_source->value &&
                       s.effect == m_source->effect &&
                       s.interpolation == m_source->interpolation;
        return false;
    }
    /// @brief 推进端点位置；相反端点、起终值、曲线与采样定义始终保持不变。
    /// @param toTime 连续帧只反投影；释放才沿用原时间线吸附并最终校验。
    /// @param output 通用触点分类，不直接消费平台原生事件队列。
    /// @param y 真实屏幕纵坐标，快速释放使用分类器保存的最终位置。
    /// @param moved 已越过通用手指容差，不以时间轴缩放变化推断用户移动。
    /// @param released 仅正常释放边沿为真，多指或系统中断必须先 cancel。
    /// @param snapshot 当前快照只借用到本次调用结束，用于本地冲突提示。
    /// @return 正常释放且有合法变化时为真，调用者提交一次后立即 cancel。
    /// @warning 每帧固定值更新；原吸附助手有临时容器，仅在释放边沿调用。
    template<class ToTime>
    bool update(MobileCanvasGesture::Output output, float y, bool moved,
                bool released, const Common::Render::RenderSnapshot& snapshot,
                ToTime toTime)
    {
        using O = MobileCanvasGesture::Output;
        if ( !m_source ) return false;
        if ( output == O::Begin ) m_started = true;
        // 待定触点仍未进入范围编辑，不因按住的时间变长而补发模型命令。
        // Suppress 和 Menu 都不能改变候选，更不能被解释为正常松手。
        if ( !m_started ||
             (output != O::Begin && output != O::Pass && output != O::Release) )
            return false;
        m_moved |= moved;
        if ( m_moved ) {
            // 只用通用分类器的拖动标志，不能因补偿像素变化把轻点升级。
            // 真实抓取点到边界的像素偏差随本笔保持，手指不必压在细线中心。
            // 返回原起点时恢复精确原值，不能被附近拍线吸成一次微小编辑。
            const double original =
                m_part == Part::Start
                    ? m_source->time
                    : m_source->time + m_source->interpolation.m_duration;
            double target = std::abs(y - m_originY) < .01F
                                ? original
                                : toTime(y + m_grabOffset, released);
            if ( !std::isfinite(target) ) return false;
            // 固定边界始终来自起笔值；不能拿上一帧候选反复加差产生漂移。
            // 计算终点只做一次加法，保持原始末端的双精度时间身份。
            const double fixedEnd =
                m_source->time + m_source->interpolation.m_duration;
            // 端点不能互换；跨过另一端时夹到 1 ms，符合原生段落最小时长。
            // 起点还受绝对零约束，不通过改变另一个端点来迁就手指位置。
            if ( m_part == Part::Start ) {
                m_preview->time = std::clamp(target, 0.0, fixedEnd - .001);
                // 大绝对时间相减可能有一个 ULP 的下溢，时长仍须不小于 1 ms。
                m_preview->interpolation.m_duration =
                    std::max(.001, fixedEnd - m_preview->time);
            } else {
                // 尾端调整保留原起点，范围变长不会改变曲线起始参数。
                target = std::max(target, m_source->time + .001);
                m_preview->interpolation.m_duration =
                    std::max(.001, target - m_source->time);
            }
        }
        // 无移动、无变化及冲突都不发空更新，不清除此前可用的重做。
        // 最终命令仍需逻辑线程根据完整时间线再次检查，UI 快照不作权威许可。
        return released && m_moved && valid(snapshot) &&
               (std::abs(m_preview->time - m_source->time) > 1e-9 ||
                std::abs(m_preview->interpolation.m_duration -
                         m_source->interpolation.m_duration) > 1e-9);
    }
    /// @brief 对应专业泳道；普通模式仍使用整条可见区域。
    /// @warning 每帧固定分支，不读取配置或构造查找容器。
    /// @note 效果枚举不是泳道索引，显式映射避免底层数值变化影响命中。
    static int lane(TimingEffect effect)
    {
        switch ( effect ) {
        case TimingEffect::BPM: return 0;
        case TimingEffect::SCROLL: return 1;
        case TimingEffect::JUMP: return 2;
        case TimingEffect::HS: return 3;
        default: return 4;
        }
    }
    /// @brief 同一几何同时用于命中和绘制，缩放不改变端点时间。
    /// @warning 每帧固定标量计算，不缓存窗口地址或分配状态。
    /// @note 左右偏置按泳道宽度计算，不按物理 framebuffer 像素猜测设备倍率。
    static ImVec2 anchor(Part part, const Element& s, ImVec2 origin,
                         ImVec2 size, bool professional, float y)
    {
        const float width = professional ? size.x / 5 : size.x;
        return { origin.x + (professional ? lane(s.effect) * width : 0) +
                     width * (part == Part::Start ? .25F : .75F),
                 y };
    }
    /// @brief 命中限于所属泳道，首尾重叠时优先最近的明确锚点。
    /// @warning 按下边沿计算两个椭圆热区，不遍历后台实体。
    static Part hit(const Element& s, ImVec2 point, ImVec2 origin, ImVec2 size,
                    bool professional, float scale, float startY, float endY)
    {
        const float width = professional ? size.x / 5 : size.x;
        const float left =
            origin.x + (professional ? lane(s.effect) * width : 0);
        if ( point.x < left || point.x >= left + width || point.y < origin.y ||
             point.y >= origin.y + size.y )
            return Part::None;
        // 先拒绝所属泳道之外的触点，再求最近端点，避免跨列吸走普通时间点。
        // 椭圆热区在窄列收窄横轴，纵轴仍提供手指容差，正文中心留作整体移动。
        Part  result = Part::None;
        float best   = 1;
        for ( auto part : { Part::Start, Part::End } ) {
            const auto  a  = anchor(part,
                                    s,
                                    origin,
                                    size,
                                    professional,
                                    part == Part::Start ? startY : endY);
            const float dx = point.x - a.x, dy = point.y - a.y;
            // 中间留给正文和普通点，窄泳道不能让两个端点热区吃掉整个标记。
            const float rx       = std::min(22 * scale, width * .22F),
                        ry       = 22 * scale;
            const float distance = dx * dx / (rx * rx) + dy * dy / (ry * ry);
            if ( distance < best ) {
                // 严格小于保持首尾距离相同时的确定顺序，不随帧率交替命中。
                best   = distance;
                result = part;
            }
        }
        return result;
    }
    /// @brief 仅画主题细把手和活动范围带，不为预览重建原曲线采样或修改实体。
    /// @param s 空闲时借用选中段，活动时借用本地候选，不逐帧复制插值定义。
    /// @param allowed
    /// 本地冲突校验结果，只影响反馈；不能作为模型更新的最终许可。
    /// @note 原曲线保留为参考，不把未提交的函数域变化混入真实播放或导出。
    /// @note 本地预览没有等待网络、Fence 或后台快照确认，按触点逐次更新。
    /// @warning 每帧只使用当前窗口 DrawList 与常量几何，遵守原图像裁剪。
    void render(const Element& s, ImVec2 origin, ImVec2 size, bool professional,
                float scale, float startY, float endY, bool allowed) const
    {
        auto* draw = ImGui::GetWindowDrawList();
        // 仅使用当前窗口的正常绘制列表，不能用前景列表覆盖抽屉或其它模态。
        // 裁剪同时限制静态把手和活动边框，窗口调整后无需缓存屏幕矩形。
        draw->PushClipRect(
            origin, { origin.x + size.x, origin.y + size.y }, true);
        const float width = professional ? size.x / 5 : size.x;
        const auto  left =
            origin.x + (professional ? lane(s.effect) * width : 0);
        const auto color = ImGui::GetColorU32(allowed ? ImGuiCol_PlotHistogram
                                                      : ImGuiCol_TextDisabled);
        if ( m_started ) {
            // 原曲线留作未提交参考，范围边框立即跟手；释放后才由模型重采样。
            // 超出可见区由同一裁剪处理，不把离屏边界伪装成屏幕边缘把手。
            for ( float x : { left + 2, left + width - 2 } )
                draw->AddLine({ x, startY }, { x, endY }, color, 2 * scale);
            draw->AddText(nullptr,
                          0,
                          { origin.x + 4 * scale, origin.y + 4 * scale },
                          color,
                          TR(allowed ? "ui.mobile.timing.resize_hint"
                                     : "ui.mobile.timing.resize_conflict")
                              .data(),
                          nullptr,
                          size.x - 8 * scale);
        }
        for ( auto part : { Part::Start, Part::End } ) {
            const auto  a    = anchor(part,
                                      s,
                                      origin,
                                      size,
                                      professional,
                                      part == Part::Start ? startY : endY);
            const float half = std::min(14 * scale, width * .2F);
            // 可见图形与触摸半径分开，窄泳道仍保持细线，不覆盖整行内容。
            // 圆角随主题参数，不能硬编码成与用户美化设置无关的圆形按钮。
            draw->AddRectFilled({ a.x - half, a.y - 2 * scale },
                                { a.x + half, a.y + 2 * scale },
                                color,
                                ImGui::GetStyle().FrameRounding);
        }
        draw->PopClipRect();
    }
    /// @brief 本地范围与已发布同效果对象不可交叠内部，共享端点仍然合法。
    /// @details UI 只查询已发布对象；屏幕外独立点由逻辑命令的全量校验兜底。
    /// @warning 活动拖动帧遍历现有快照，不复制容器、不展开曲线、不扫描 ECS。
    bool valid(const Common::Render::RenderSnapshot& snapshot) const
    {
        // 有限值先于所有区间比较，NaN 不能通过开区间条件悄悄进入模型。
        if ( !m_preview ) return false;
        const double start = m_preview->time,
                     end   = start + m_preview->interpolation.m_duration;
        if ( !std::isfinite(end) || start < 0 || end - start < .001 - 1e-9 )
            return false;
        // 正文段落来自完整缓存；独立点只查询快照，不新增后台遍历或长锁。
        for ( const auto& other : snapshot.timingInterpolations )
            // 两段可以端到端衔接，只有严格内部交集才拒绝。
            if ( other.entity != m_preview->entity &&
                 other.effect == m_preview->effect &&
                 std::min(end, other.time + other.interpolation.m_duration) -
                         std::max(start, other.time) >
                     1e-9 )
                return false;
        for ( const auto& row : snapshot.timelineElements ) {
            // 同时间行可以有多种效果，只提取当前泳道身份，不能误挡另一列。
            // 源实体必须排除，否则缩短首端时会把自己的旧标记判为冲突。
            entt::entity entity = entt::null;
            switch ( m_preview->effect ) {
            case TimingEffect::BPM: entity = row.bpmEntity; break;
            case TimingEffect::SCROLL: entity = row.scrollEntity; break;
            case TimingEffect::JUMP: entity = row.jumpEntity; break;
            case TimingEffect::HS: entity = row.hsEntity; break;
            default: break;
            }
            if ( entity != entt::null && entity != m_preview->entity &&
                 row.time > start + 1e-9 && row.time < end - 1e-9 )
                return false;
        }
        // 本地通过仍不是写入许可，释放后原动作入口会重新检查密度、拍轴与范围。
        return true;
    }

private:
    std::optional<Element> m_source,
        m_preview;               ///< 只在起笔复制的原始定义与本地候选。
    Part  m_part{ Part::None };  ///< 整笔固定，拖出热区不能改为正文整体移动。
    float m_grabOffset{}, m_originY{};  ///< 细线偏置与真实原点，避免起笔跳动。
    bool  m_started{}, m_moved{};       ///< 候选、实际拖动和正常释放分开解释。
};
}  // namespace MMM::Canvas
