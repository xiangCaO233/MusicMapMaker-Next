#include "logic/session/NoteIdentity.h"

#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "mmm/note/Note.h"
#include "mmm/sample/AudioSample.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace MMM::Logic
{

namespace
{
/// @brief 进程内音符标识序号；仅保证编号唯一，不承担状态同步。
/// @warning 写入者是低频谱面载入或编辑命令，读取者仅为当前调用者；relaxed
/// 顺序足以保证序号不重复，禁止在每帧路径批量调用。
std::atomic<std::uint64_t> g_noteIdentitySequence{ 0U };

/// @brief 将 64 位数值追加为固定宽度十六进制文本。
void appendHex64(std::string& output, std::uint64_t value)
{
    std::array<char, 16>       buffer{};
    constexpr std::string_view DIGITS = "0123456789abcdef";
    for ( std::size_t index = buffer.size(); index > 0U; --index ) {
        buffer[index - 1U] = DIGITS[value & 0xFU];
        value >>= 4U;
    }
    output.append(buffer.data(), buffer.size());
}
}  // namespace

std::string makeNoteCollaborationId()
{
    const auto wallClock = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto monotonic = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto sequence =
        g_noteIdentitySequence.fetch_add(1U, std::memory_order_relaxed);
    const auto processSalt = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&g_noteIdentitySequence));

    std::string identity;
    identity.reserve(32U);
    appendHex64(identity, wallClock ^ processSalt);
    appendHex64(identity, monotonic ^ (sequence * 0x9E3779B97F4A7C15ULL));
    return identity;
}

void ensureNoteCollaborationIdentity(::MMM::Note& note)
{
    if ( note.m_collaborationId.empty() ) {
        note.m_collaborationId = makeNoteCollaborationId();
    }
}

void ensureNoteCollaborationIdentity(NoteComponent& note)
{
    if ( note.m_collaborationId.empty() ) {
        note.m_collaborationId = makeNoteCollaborationId();
    }
    for ( auto& subNote : note.m_subNotes ) {
        if ( subNote.collaborationId.empty() ) {
            subNote.collaborationId = makeNoteCollaborationId();
        }
    }
}

void ensureSampleCollaborationIdentity(::MMM::AudioSampleEvent& sample)
{
    if ( sample.m_collaborationId.empty() ) {
        sample.m_collaborationId = makeNoteCollaborationId();
    }
}

void ensureSampleCollaborationIdentity(SampleComponent& sample)
{
    if ( sample.m_collaborationId.empty() ) {
        sample.m_collaborationId = makeNoteCollaborationId();
    }
}

}  // namespace MMM::Logic
