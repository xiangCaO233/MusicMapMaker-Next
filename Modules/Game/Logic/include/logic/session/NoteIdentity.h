#pragma once

#include <string>

namespace MMM
{
class Note;
}

namespace MMM::Logic
{

struct NoteComponent;

/// @brief 生成当前进程内唯一、跨协作端极低碰撞概率的音符逻辑标识。
/// @return 由两个 64 位十六进制段组成的逻辑标识。
/// @warning 仅在谱面载入或用户创建音符时调用；内部原子计数器使用 relaxed
/// 顺序，只承担唯一编号而不参与线程同步，禁止放入每帧更新路径。
[[nodiscard]] std::string makeNoteCollaborationId();

/// @brief 为领域音符补齐协作逻辑标识。
/// @param note 待补齐的领域音符。
void ensureNoteCollaborationIdentity(::MMM::Note& note);

/// @brief 为 ECS 根音符及其折线子物件补齐协作逻辑标识。
/// @param note 待补齐的 ECS 音符。
void ensureNoteCollaborationIdentity(NoteComponent& note);

}  // namespace MMM::Logic
