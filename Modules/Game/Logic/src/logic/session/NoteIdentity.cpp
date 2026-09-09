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
/// @param output 已有前缀保留，向末尾追加 16 个小写十六进制字符。
/// @param value 按无符号位模式编码，不添加符号或 0x 前缀。
/// @pre output 可继续追加；调用者提前预留两段容量以避免中途扩容。
void appendHex64(std::string& output, std::uint64_t value)
{
    std::array<char, 16>       buffer{};
    constexpr std::string_view DIGITS = "0123456789abcdef";
    // 从低四位反向填充，固定宽度保留前导零，使两段拼接无需分隔符。
    for ( std::size_t index = buffer.size(); index > 0U; --index ) {
        buffer[index - 1U] = DIGITS[value & 0xFU];
        value >>= 4U;
    }
    output.append(buffer.data(), buffer.size());
}
}  // namespace

/// @brief 混合时间、地址盐和调用序号生成协作标识文本。
/// @return 固定 32 字符；用于业务身份，不作为秘密令牌或时间戳解析。
/// 各输入经过异或混合，不应从生成文本反推出创建顺序或物件类型。
/// @warning 载入和创建物件的低频路径，包含时钟查询、字符串分配及 relaxed
/// 原子计数。
std::string makeNoteCollaborationId()
{
    // 墙钟与单调钟提供不同时间信息，序号区分同一时钟刻度内的调用。
    const auto wallClock = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto monotonic = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto sequence =
        g_noteIdentitySequence.fetch_add(1U, std::memory_order_relaxed);
    const auto processSalt = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&g_noteIdentitySequence));
    // 地址只作为不同进程布局的扰动值，不解引用也不写入对象地址信息表。

    std::string identity;
    identity.reserve(32U);
    // 两段固定宽度；预留最终容量，避免第二次追加引发字符串扩容。
    appendHex64(identity, wallClock ^ processSalt);
    appendHex64(identity, monotonic ^ (sequence * 0x9E3779B97F4A7C15ULL));
    return identity;
}

/// @brief 为领域音符保留或创建持久化身份。
/// @param note 待载入或新建的物件，调用方负责串行修改。
/// 标识为空才生成新值；不检查也不重写外部文件已经携带的非空格式。
void ensureNoteCollaborationIdentity(::MMM::Note& note)
{
    // 已有 ID 可能被撤销记录或远端操作引用，不能在同步时重新生成。
    if ( note.m_collaborationId.empty() ) {
        note.m_collaborationId = makeNoteCollaborationId();
    }
}

/// @brief 为 ECS 根音符和折线子物件分别补齐身份。
/// @param note 已构造的组件，保留所有非空 ID 及子物件次序。
/// 只处理此组件拥有的子物件列表，不遍历 ECS 注册表中的其他实体。
void ensureNoteCollaborationIdentity(NoteComponent& note)
{
    if ( note.m_collaborationId.empty() ) {
        note.m_collaborationId = makeNoteCollaborationId();
    }
    // 根已具有身份也仍需检查子节点，新增加的折线节点可能尚未编号。
    for ( auto& subNote : note.m_subNotes ) {
        if ( subNote.collaborationId.empty() ) {
            subNote.collaborationId = makeNoteCollaborationId();
        }
    }
}

/// @brief 为领域采样事件补齐协作身份。
/// @param sample ID 与事件内容一同保存，不使用音频资源 ID 代替事件身份。
/// 相同采样路径和时间可以对应不同编辑事件，不能按内容去重身份。
void ensureSampleCollaborationIdentity(::MMM::AudioSampleEvent& sample)
{
    // 同一资源可以被多个事件引用，每个缺失身份的事件独立编号。
    if ( sample.m_collaborationId.empty() ) {
        sample.m_collaborationId = makeNoteCollaborationId();
    }
}

/// @brief 为 ECS 采样组件补齐身份，供编辑与协作命令定位。
/// @param sample 当前组件，已有 ID 与领域事件之间的对应关系保持不变。
/// 调用点应位于载入或编辑命令提交阶段，不在每帧渲染时补号。
void ensureSampleCollaborationIdentity(SampleComponent& sample)
{
    if ( sample.m_collaborationId.empty() ) {
        sample.m_collaborationId = makeNoteCollaborationId();
    }
}

}  // namespace MMM::Logic
