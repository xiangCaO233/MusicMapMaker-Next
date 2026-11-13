#pragma once
#include "MobileCanvasGesture.h"
#include "canvas/PlanarObservationInput.h"
#include "common/ObjectDeletion.h"
#include "common/render/RenderSnapshot.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/interaction/EditorInteraction.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/vec2.hpp>
#include <imgui.h>
#include <optional>
#include <utility>
#include <vector>

namespace MMM::Canvas
{
/// @brief 移动物件擦除仅暂存值身份，正式数据直到释放才改变。
/// @details 与桌面右键橡皮隔离，不模拟 Shift，不用 Undo 回滚预览。
/// 主画布快照已经提供可见命中框，连续采样不访问 Registry。
/// 实体与领域成对去重，采样和音符的相同实体编号不会互相覆盖。
/// 一笔只擦整物件；折线局部断开由已有节点菜单负责。
/// 生命周期边界：
/// - 对象随画布构造，起笔不创建新的解释器。
/// - 不保存快照、窗口或绘图列表的引用。
/// - 第二指先由所有权仲裁处理，本类从不回滚正式数据。
/// - 中断不读取历史栈顶，上一笔的重做仍可正常使用。
/// - 单次容量来自可见几何，不为逐帧候选分配哈希节点。
class MobileObjectEraser
{
public:
    /// @brief 丢弃预览而不产生删除或历史操作，容量留给下次起笔复用。
    /// @warning 遮罩接管时可每帧调用，只清本地状态；不访问模型、历史或线程锁。
    void cancel()
    {
        // 保留容量只清长度，后续起笔复用存储，取消不产生模型副作用。
        m_active = m_previousValid = false;
        m_targets.clear();
    }
    /// @brief 读取本地笔画状态，供切谱与模式切换取消。
    /// @warning 每帧只读标量，不等待逻辑线程。
    bool active() const { return m_active; }
    /// @brief 推进一帧触控意图并绘制待删轮廓，正常释放仅发一个批次。
    /// @param output 公共移动手势分类结果，长按菜单在擦除模式禁用。
    /// @param point 本次回放的屏幕点；Begin 必须使用实际起点。
    /// @param snapshot 仅本帧借用，实例变化立即丢弃旧身份。
    /// @param frame 带投影、插值偏移和窗口归属的屏幕域。
    /// @param scale 内容倍率，热区不依赖帧缓冲像素密度。
    /// @warning 连续帧只遍历可见框并更新已预留容量，无分配、排序或锁。
    /// 起笔预留当前全部可见框容量，释放才构造正式命令载荷。
    void update(MobileCanvasGesture::Output output, ImVec2 point,
                const Common::Render::RenderSnapshot& snapshot,
                const UI::Interaction::SurfaceFrame& frame, float scale)
    {
        using O = MobileCanvasGesture::Output;
        if ( output == O::Begin ) {
            // Begin 也可能是轻点抬手后的回放，仍需完整经过预览与提交。
            cancel();
            m_instance = snapshot.beatmapInstanceId;
            // 导航会取消本笔，正常擦除期间视野不变；以当前可见框数量预留上界。
            // 不逐帧 reserve，也不在经过每个目标时分配哈希节点。
            m_targets.reserve(snapshot.hitboxes.size());
            m_active = true;
        }
        if ( !m_active ) return;
        // 同路径重新载入也会更换实例，路径相等不足以复用旧句柄。
        if ( m_instance != snapshot.beatmapInstanceId || !snapshot.hasBeatmap ||
             snapshot.isPlaying ) {
            cancel();
            return;
        }
        // 预览跟随每次实际位置，不以固定延迟批量回放，也不修改原鼠标状态。
        // 离开内容区不延伸到标签或工具台，再进入时从新点开始以免跨空白连线。
        const bool inside = frame.m_hovered && point.x >= frame.m_x &&
                            point.x < frame.m_x + frame.m_width &&
                            point.y >= frame.m_y &&
                            point.y < frame.m_y + frame.m_height;
        if ( inside && (output == O::Begin || output == O::Pass ||
                        output == O::Release) ) {
            // 与渲染同源的平面逆投影保证二维和透视视角使用同一命中域。
            // 未启用观察投影时，二维局部像素无需额外矩阵变换。
            const auto unproject =
                [&](glm::vec2 p) -> std::optional<glm::vec2> {
                return frame.m_projection
                           ? planarObservationUnproject(*frame.m_projection, p)
                           : std::optional{ p };
            };
            // 先去窗口原点，再去动态纵向偏移；不能把两种坐标混为一个减数。
            // 辅助偏置只计算触摸热区，不修改当前触点的实际谱面位置。
            const auto end =
                unproject({ point.x - frame.m_x, point.y - frame.m_y });
            const auto pad = unproject({ point.x - frame.m_x + 6 * scale,
                                         point.y - frame.m_y + 6 * scale });
            // 无有效平面交点时不沿用上一帧的伪造终点，避免擦除跨越视角边界。
            // 热区按当前投影局部估计，不能以 framebuffer 密度直接扩大世界几何。
            if ( end && pad ) {
                const glm::vec2 p{ end->x, end->y - frame.m_snapshotYOffset };
                const glm::vec2 radius{ std::abs(pad->x - end->x),
                                        std::abs(pad->y - end->y) };
                // 起点和终点之间扫过细头部也算命中，不能只采样每帧指尖位置。
                // 仅完整可见候选参与，UI 不通过后台遍历搜索屏幕外对象。
                for ( const auto& box : snapshot.hitboxes ) {
                    // 同一折线可有多个部件框，领域身份去重后只提交一次根删除。
                    // 不按 subIndex 细分目标，局部断开仍由节点专用入口负责。
                    const Logic::ObjectDeletionTarget target{ box.entity,
                                                              box.kind };
                    if ( box.entity == entt::null ||
                         !intersects(m_previousValid ? m_previous : p,
                                     p,
                                     box,
                                     radius) ||
                         contains(target) )
                        continue;
                    // 联机在笔画中增添可见目标可能突破起笔上界，安全取消整笔。
                    // 不能截断成部分删除，也不能为继续扫动临时扩大容器。
                    if ( m_targets.size() == m_targets.capacity() ) {
                        cancel();
                        return;
                    }
                    // 只记录接触到的身份，目标图形在释放之前始终保留在谱面中。
                    // 重复经过同一目标不能扩大历史，也不能覆盖其它领域同号对象。
                    m_targets.push_back(target);
                }
                // 当前点成为下一段的唯一起点，返程不会重新清空已扫过目标。
                // 当帧没有移动时退化为点命中，保持轻点和拖动起点语义一致。
                m_previous      = p;
                m_previousValid = true;
            } else
                m_previousValid = false;
        } else if ( output != O::Suppress )
            m_previousValid = false;
        // 描边跟随当前快照位置，不把起笔时的矩形永久缓存为过期几何。
        // 预览只画内容层，不建立独立弹窗或遮罩，不能挡住后续原始触点。
        render(snapshot, frame, scale);
        // 连续拖动的真实 Up 从 Pass 到达；快速轻点则由 Release 单独排空。
        // 空手势根本不发命令，既有重做栈与当前选区保持原样。
        if ( output == O::Release ||
             (output == O::Pass && !ImGui::IsMouseDown(0)) ) {
            if ( !m_targets.empty() ) {
                // 释放是离散命令边沿，此时才复制最终身份到可跨线程的值载荷。
                // 不借出本地容器给事件队列，立即取消本地状态也不会悬空。
                Logic::ObjectDeletion deletion{ m_instance, m_targets };
                UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                    Logic::CmdDeleteSelected{ std::move(deletion) }));
            }
            cancel();
        }
    }

private:
    /// @brief 查询当前笔画内的领域身份，预留顺序容器不分配去重节点。
    /// @warning 连续帧仅查本笔已接触身份，容量由起笔一次预留。
    /// 不以索引相同判断跨领域相等，不能把采样误认为玩家音符。
    bool contains(Logic::ObjectDeletionTarget target) const
    {
        return std::find(m_targets.begin(), m_targets.end(), target) !=
               m_targets.end();
    }
    /// @brief 线段与扩大后的矩形相交，防止低帧率跨过薄音符而漏擦。
    /// @warning 每个可见框固定两轴运算，不扫描 ECS 或构造几何容器。
    static bool intersects(glm::vec2 from, glm::vec2 to,
                           const Common::Render::Hitbox& box, glm::vec2 radius)
    {
        // 参数区间限制在线段 [0,1]，不能把无限直线经过的框也计为擦过。
        // 每个轴收紧同一参数区间；空交集立即拒绝候选。
        // 矩形先按局部触控热区扩大，起点落在框内时进入参数直接夹到零。
        // 两个轴取同一参数区间交集，不能分别命中后把不相交的结果相加。
        // 算法只回答是否经过，不负责猜测被手指遮住的任意邻近物件。
        float            lo = 0, hi = 1;
        const std::array starts{ from.x, from.y }, ends{ to.x, to.y };
        const std::array mins{ box.x - radius.x, box.y - radius.y };
        const std::array maxs{ box.x + box.w + radius.x,
                               box.y + box.h + radius.y };
        for ( int axis = 0; axis < 2; ++axis ) {
            const float delta = ends[axis] - starts[axis];
            if ( std::abs(delta) < .00001f ) {
                // 平行于一轴时只检查该轴区间，避免零长度轻点除零。
                if ( starts[axis] < mins[axis] || starts[axis] > maxs[axis] )
                    return false;
                continue;
            }
            // 两方向统一取有序进入／离开参数，反向扫动无需另一套算法。
            // 共线或端点触及允许命中，低速返程与快速跨越具有相同边界。
            const float a = (mins[axis] - starts[axis]) / delta;
            const float b = (maxs[axis] - starts[axis]) / delta;
            lo            = std::max(lo, std::min(a, b));
            hi            = std::min(hi, std::max(a, b));
            if ( lo > hi ) return false;
        }
        return true;
    }
    /// @brief 已扫过目标沿原图形描边，使用主题强调色并遵守父窗口裁剪。
    /// @warning 每帧只消费已有快照；四角投影不创建新 UI 控件或输入层。
    void render(const Common::Render::RenderSnapshot& snapshot,
                const UI::Interaction::SurfaceFrame& frame, float scale) const
    {
        // 绘制资源只借用当前 ImGui 帧，主题切换不会遗留上一主题的颜色。
        // 使用窗口 draw list 的既有裁剪，标签和相邻面板不受轮廓覆盖。
        auto*      draw  = ImGui::GetWindowDrawList();
        const auto color = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
        for ( const auto& box : snapshot.hitboxes ) {
            if ( !contains({ box.entity, box.kind }) ) continue;
            // 四角逐一投影，三维观察下仍沿实际平面轮廓，不画屏幕近似大框。
            // 身份集合固定，父对象所有可见部位都强调为整物件待删。
            std::array<glm::vec2, 4> corners{ { { box.x, box.y },
                                                { box.x + box.w, box.y },
                                                { box.x + box.w,
                                                  box.y + box.h },
                                                { box.x, box.y + box.h } } };
            bool                     valid = true;
            // 一角失去有效投影就跳过整个轮廓，不把不完整多边形连到屏幕边缘。
            // 这里不改变擦除目标，显示失败不制造额外模型删除。
            for ( auto& p : corners ) {
                p.y += frame.m_snapshotYOffset;
                if ( frame.m_projection ) {
                    const auto projected =
                        planarObservationProject(*frame.m_projection, p);
                    if ( !projected ) {
                        valid = false;
                        break;
                    }
                    p = *projected;
                }
                p += glm::vec2(frame.m_x, frame.m_y);
            }
            if ( valid )
                draw->AddQuad({ corners[0].x, corners[0].y },
                              { corners[1].x, corners[1].y },
                              { corners[2].x, corners[2].y },
                              { corners[3].x, corners[3].y },
                              color,
                              2 * scale);
        }
    }
    std::vector<Logic::ObjectDeletionTarget>
                   m_targets;            ///< 每笔复用容量的待删身份。
    glm::vec2      m_previous{};         ///< 上一个有效平面点，跨窗口时失效。
    std::uintptr_t m_instance{};         ///< 旧谱面实体不得套到新实例。
    bool m_active{}, m_previousValid{};  ///< 起笔和连续扫动的本地门闩。
};
}  // namespace MMM::Canvas
