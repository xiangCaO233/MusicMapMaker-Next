#pragma once

#include "common/walkthrough/ComposeLessonNotes.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace MMM::UI::Walkthrough
{
struct Topic;
/// @brief 一段由 CanonRock 时间戳批注圈定的创作教学。
/// @details 时间采用谱面持久化的毫秒单位，播放命令边界再换算成秒。
struct ComposeLesson {
    std::string m_title;              ///< 起始批注去掉进阶前缀后的教学名称。
    double      m_beginMs{ 0.0 };     ///< 目标段落的起点。
    double      m_endMs{ 0.0 };       ///< 同名结束批注标记的终点。
    bool        m_advanced{ false };  ///< 从进阶标记开始属于进阶路线。
    /// @brief 草稿区同一时间窗内的目标几何；删除练习可为空。
    std::vector<Logic::ComposeLessonNote> m_reference;
    bool m_allowUndoButton{ true };  ///< 删除教学及后续段落只能由用户亲自编辑。
};

/// @brief 编辑练习中当前物件与目标物件之间需要修正的部位。
enum class ComposeLessonRepairKind { None, Move, FlickTail, HoldTail, Delete };

/// @brief 一次正式物件查询与草稿目标的一对一匹配结果。
/// @details 只在修订变化后重建，画布每帧仅读取匹配标记绘制提示。
struct ComposeLessonFeedback {
    const ComposeLesson* lesson{
        nullptr
    };  ///< 服务内稳定参考，退出路线时失效。
    std::uintptr_t beatmapInstanceId{ 0 };    ///< 防止跨谱面显示旧物件。
    std::uint64_t  composeNoteRevision{ 0 };  ///< 防止展示旧修订的错误物件。
    std::vector<Logic::ComposeLessonNote> actual;  ///< 最近一次逻辑查询结果。
    std::vector<bool> expectedMatched;  ///< 每个草稿目标是否已有正式物件。
    std::vector<bool> actualMatched;    ///< 每个正式物件是否满足一份目标。
    ComposeLessonRepairKind repairKind{
        ComposeLessonRepairKind::None
    };  ///< 当前段落的编辑语义。
    /// @brief 未完成的实际物件对应的参考下标；无可修正目标时为 -1。
    std::vector<int> repairTargetForActual;
    bool             showUndoButton{
        false
    };  ///< 仅删除教学之前的普通放置段落开放辅助按钮。
};

/// @brief 对实际物件与教学参考作完整一对一匹配并产生绘制反馈。
/// @warning 只在段落进入和正式物件修订后调用，不进入每帧绘制路径。
ComposeLessonFeedback compareComposeLessonNotes(
    const ComposeLesson& lesson, std::vector<Logic::ComposeLessonNote> actual,
    std::uintptr_t beatmapInstanceId, std::uint64_t composeNoteRevision);

/// @brief 从示例谱面读取并配对教学开始、结束批注。
/// @param beatmapFile CanonRock 教学谱面的实际路径。
/// @return 按时间排列的完整段落；批注损坏时返回具体原因，不生成半条路线。
/// @warning 低频资源加载：只在加载演练目录时读取文件和排序，不得逐帧调用。
std::expected<std::vector<ComposeLesson>, std::string> loadComposeLessons(
    const std::filesystem::path& beatmapFile);

/// @brief 用谱面批注生成基础和进阶两条创作路线及其三阶段步骤。
/// @param topic 已从内置声明解析的创作主题；仅替换它的步骤分支。
/// @param lessons 完整配对、按时间顺序的教学段落。
/// @warning 仅在服务构造时调用，生成的步骤供每帧只读查找。
void populateComposeLessonTopic(Topic&                            topic,
                                const std::vector<ComposeLesson>& lessons);

/// @brief 比较主轨道最终物件与草稿参考的完整几何及数量。
/// @details 时间、有效 Hold 长度允许 2 ms
/// 浮点/吸附误差，轨道与类型必须精确相等。
/// 折线父级的持续长度与横移缓存不参与验收，完整子段路径仍须相同。
/// @warning 仅在教学物件变更后的单次查询结果到达时调用，不进入普通 UI 帧。
bool matchesComposeLessonNotes(
    const ComposeLesson&                         lesson,
    const std::vector<Logic::ComposeLessonNote>& actual);
}  // namespace MMM::UI::Walkthrough
