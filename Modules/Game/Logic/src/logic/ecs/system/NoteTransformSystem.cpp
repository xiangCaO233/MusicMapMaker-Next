#include "logic/ecs/system/NoteTransformSystem.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"

namespace MMM::Logic::System
{

/// @brief 更新音符逻辑坐标缓存。
/// @param registry 持有音符与变换组件的注册表。
/// @param timelineRegistry 已初始化 ScrollCache 上下文的时间线注册表。
/// @param currentTime 本次投影的时间锚点，与音符时间使用相同单位。
/// @param config 重建滚速缓存所用的有效配置。
/// @param beatmap 缓存重建需要的谱面上下文。
/// @param forceRebuild 即使滚速缓存未脏也刷新全部物件变换。
/// @pre 调用方负责在物件或投影条件变化时请求重建，不在本函数追踪版本。
/// @note 本函数不处理相机横移，多个画布仍可各自按布局计算横向投影。
/// @warning 逻辑热路径：每个 Session update 调用；完整 registry view
/// 遍历只允许在 cacheDirty 或 forceRebuild 时执行，禁止在此处排序。
void NoteTransformSystem::update(entt::registry&             registry,
                                 entt::registry&             timelineRegistry,
                                 double                      currentTime,
                                 const Config::EditorConfig& config,
                                 MMM::BeatMap* beatmap, bool forceRebuild)
{
    auto& cache      = timelineRegistry.ctx().get<ScrollCache>();
    bool  cacheDirty = cache.isDirty;
    // 在 rebuild 清除标记之前保存旧状态，本轮仍需据此刷新物件投影。
    if ( cache.isDirty ) {
        cache.rebuild(timelineRegistry, config, beatmap);
    }

    if ( !cacheDirty && !forceRebuild ) {
        // 常规无变化轮次直接退出，不能因每次调用都遍历全部音符。
        return;
    }

    double currentAbsY = cache.getAbsY(currentTime);
    // 全部物件共享同一锚点，避免在逐实体循环里重复查询当前绝对纵坐标。

    auto noteView = registry.view<TransformComponent, const NoteComponent>();
    // 仅处理已有变换组件的音符，不在遍历期间补建组件或改变实体集合。
    for ( auto entity : noteView ) {
        auto&       transform = noteView.get<TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);

        double noteAbsY = cache.getAbsY(note.m_timestamp);
        double noteHs   = cache.getHsAt(note.m_timestamp);
        // 滚速积分给出绝对距离，物件所在时刻的 HS 再作用于相对纵向距离。
        float relY = static_cast<float>((noteAbsY - currentAbsY) * noteHs);

        float minY = relY;
        float maxY = relY + 20.0f;
        // 普通物件保留基础纵向厚度，长条和折线随后按自身时间跨度扩展。

        if ( note.m_type == ::MMM::NoteType::HOLD ) {
            // 长条尾部通过滚速缓存映射，不能简单以持续秒数乘固定像素速度。
            double endAbsY = cache.getAbsY(note.m_timestamp + note.m_duration);
            maxY = static_cast<float>((endAbsY - currentAbsY) * noteHs);
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                    !note.m_subNotes.empty() ) {
            for ( const auto& sub : note.m_subNotes ) {
                // 子节点使用自己的时间和 HS，不能统一沿用根节点的局部倍率。
                double subAbsY = cache.getAbsY(sub.timestamp);
                double subHs   = cache.getHsAt(sub.timestamp);
                float  subRelY =
                    static_cast<float>((subAbsY - currentAbsY) * subHs);
                minY = std::min(minY, subRelY);

                double subEndAbsY = cache.getAbsY(sub.timestamp + sub.duration);
                // 子节点可能携带长条跨度，尾部同样参与纵向范围计算。
                float subEndRelY =
                    static_cast<float>((subEndAbsY - currentAbsY) * subHs);
                maxY = std::max(maxY, subEndRelY + 20.0f);
            }
        }

        // 仅在 NoteTransformSystem 中计算 Y 轴逻辑位置，X
        // 轴与宽度由渲染系统基于布局动态计算
        transform.m_pos.y  = minY;
        transform.m_size.y = maxY - minY;
        // 输出仅为本轮重建的逻辑投影，不写回音符时间戳或谱面几何属性。
    }
}

}  // namespace MMM::Logic::System
