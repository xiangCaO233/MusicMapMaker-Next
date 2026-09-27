#pragma once

#include "common/walkthrough/ComposeLessonNotes.h"

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
};

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
/// @details 时间、Hold 长度允许 2 ms 浮点/吸附误差，轨道与类型必须精确相等。
/// @warning 仅在教学物件变更后的单次查询结果到达时调用，不进入普通 UI 帧。
bool matchesComposeLessonNotes(
    const ComposeLesson&                         lesson,
    const std::vector<Logic::ComposeLessonNote>& actual);
}  // namespace MMM::UI::Walkthrough
