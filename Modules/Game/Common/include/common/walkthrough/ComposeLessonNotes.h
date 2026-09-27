#pragma once

#include "mmm/note/Note.h"

#include <atomic>
#include <entt/entt.hpp>
#include <vector>

namespace MMM::Logic
{
/// @brief 创作教程用于比较的一段音符几何，忽略配色、采样和协作身份。
/// @details 草稿持久化采用非负轨道坐标，玩家音符也采用非负坐标。
/// 草稿参考不保留 ECS 身份；正式物件查询额外保留根实体句柄，
/// 供错误框按钮定向操作，谱面实例身份仍由调用方单独核验。
/// 教学查询跨线程传输时只移动这些标量与子段数组。
struct ComposeLessonNote {
    /// @brief 折线中的一段几何，轨道采用该区域从左到右的相对索引。
    struct SubNote {
        /// @brief 子段类型参与折线拓扑验收，不能只比较节点个数。
        ::MMM::NoteType type{ ::MMM::NoteType::NOTE };
        double          timestamp{ 0.0 };  ///< 秒。
        double          duration{ 0.0 };   ///< 秒。
        int             track{ 0 };        ///< 区域内轨道索引。
        int             dtrack{ 0 };       ///< Flick 相对终点。
    };
    ::MMM::NoteType type{ ::MMM::NoteType::NOTE };
    /// @brief 时间字段与渲染会话统一采用秒，批注毫秒仅在目录层换算。
    double               timestamp{ 0.0 };  ///< 根节点时间，单位秒。
    double               duration{ 0.0 };   ///< Hold 持续时间，单位秒。
    int                  track{ 0 };        ///< 区域内轨道索引。
    int                  dtrack{ 0 };       ///< Flick 相对终点。
    std::vector<SubNote> subNotes;          ///< Polyline 全部子段。
    entt::entity entity{ entt::null };  ///< 实际物件的根身份；参考不持有实体。
};

/// @brief UI 与逻辑线程之间的一次性教学物件查询结果。
/// @details 命令持有 shared_ptr 直到逻辑线程写完；UI 不能在 ready 前读取
/// notes。 查询只由编辑阶段的物件修订触发，正常渲染路径不分配这种结果。 ready
/// 只从 false 变为 true，结果对象不会被复用或再次写入。
struct ComposeLessonCapture {
    std::vector<ComposeLessonNote> notes;  ///< 仅在 ready 发布前写入。
    /// @brief 写入者为逻辑线程；UI 每帧仅在教学编辑阶段以 acquire 读取。
    /// @warning UI 热路径必须用无阻塞原子标志；不能等待会话 update 长锁。
    std::atomic<bool> ready{ false };
};
}  // namespace MMM::Logic
