#include "common/AudioResourceDragPayload.h"
#include "mmm/project/AudioResource.h"

#include <string>

/// @brief 验证音频资源拖放载荷的成功构造和输入边界。
/// @return 所有断言通过时返回零，否则返回对应失败阶段编号。
int main()
{
    using namespace MMM;
    using namespace MMM::Common;

    // 合法 ID 应完整复制，并保留资源类型供画布选择目标轨道。
    const auto payload =
        makeAudioResourceDragPayload("main-track.ogg", AudioTrackType::Main);
    // 同时检查 optional、ID 视图和枚举字段，覆盖载荷往返的核心契约。
    if ( !payload || audioResourceIdView(*payload) != "main-track.ogg" ||
         payload->m_audioTrackType != AudioTrackType::Main ) {
        // 第一阶段失败表示合法载荷未能保持任一关键字段。
        return 1;
    }

    // 构造超过固定容量的 ID，验证接口不会截断为另一个资源标识。
    const std::string oversizedId(AUDIO_RESOURCE_DRAG_ID_CAPACITY + 32U, 'x');
    // 超长与空 ID 均不具备有效资源身份，必须返回空载荷。
    if ( makeAudioResourceDragPayload(oversizedId, AudioTrackType::Effect) ||
         makeAudioResourceDragPayload("", AudioTrackType::Effect) ) {
        // 第二阶段失败表示无效 ID 被错误接受。
        return 2;
    }

    // 所有边界保持时以进程退出码报告测试成功。
    return 0;
}
