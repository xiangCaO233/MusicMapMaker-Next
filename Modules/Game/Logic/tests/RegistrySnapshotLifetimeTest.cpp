/// @file
/// @brief 注册表拥有权与快照替换回归，不依赖窗口或音频设备。
/// 同一线程安排读句柄的存活区间，验证释放边界而不是并发调度时序。
/// 测试只使用内存注册表与空会话，失败直接报告到项目日志。
#include "logic/BeatmapSession.h"
#include "logic/RenderSyncRegistry.h"
#include "logic/SessionRegistry.h"

#include "log/colorful-log.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace
{

/// @brief 验证会话移除且无读者后不再被注册表保活。
/// @return 被移除会话已析构时返回 true。
/// 同时检查元数据进入快照，避免只发布裸会话而丢失音频时间线指纹。
bool testErasedSessionIsReleased()
{
    MMM::Logic::SessionRegistry registry;
    auto session = std::make_shared<MMM::Logic::BeatmapSession>();
    // 弱引用只观察销毁，不成为阻止会话析构的额外拥有者。
    std::weak_ptr<MMM::Logic::BeatmapSession> sessionObserver = session;

    MMM::Logic::SessionEntry entry;
    // 两个指纹使用不同值，防止发布逻辑把完整时间线与 Main 同步字段混用。
    entry.session                  = session;
    entry.cameraId                 = "Canvas_0";
    entry.displayName              = "Lifetime Test";
    entry.audioTimelineFingerprint = "complete-timeline";
    entry.mainAudioSyncFingerprint = "main-sync";
    // 移入条目后，本地 session 和注册表是预期的拥有方。
    registry.append(std::move(entry));

    {
        // 用作用域结束快照借用，后面的删除测试不应留着读者保活会话。
        const auto snapshot = registry.publishedSnapshot();
        // 数量先于 front 检查；短路求值使空发布结果安全地进入失败分支。
        // 会话地址相等确认发布的是注册对象，而不是另外创建的空会话。
        if ( snapshot->sessions.size() != 1U ||
             snapshot->sessions.front().session != session ||
             snapshot->sessions.front().audioTimelineFingerprint !=
                 "complete-timeline" ||
             snapshot->sessions.front().mainAudioSyncFingerprint !=
                 "main-sync" ) {
            XERROR("Session registry did not publish the appended session");
            return false;
        }
    }

    // 删除索引零会产生空的新快照，旧的无读者快照应随替换释放。
    registry.erase(0);
    // 去掉本地强引用后，残留拥有者只能来自注册表及其内部发布快照。
    session.reset();
    if ( !sessionObserver.expired() ) {
        // 若退休快照被永久积攒，条目虽已删除却仍占用会话资源。
        XERROR("Erased session remained owned by a retired registry snapshot");
        return false;
    }
    return true;
}

/// @brief 验证并发读句柄离开前会保护旧会话，离开后立即释放。
/// @return 旧快照生命周期符合读句柄范围时返回 true。
/// 以持有旧快照模拟正在处理的一轮逻辑更新，不启动额外工作线程。
bool testReaderHandleControlsRetiredSessionLifetime()
{
    // 注册表在整个读句柄检查期间存活，排除注册表析构带来的整体清理。
    MMM::Logic::SessionRegistry registry;
    auto session = std::make_shared<MMM::Logic::BeatmapSession>();
    // 只留一个不拥有资源的观察入口，之后用 expired 判断真实释放时刻。
    std::weak_ptr<MMM::Logic::BeatmapSession> sessionObserver = session;

    MMM::Logic::SessionEntry entry;
    entry.session  = session;
    entry.cameraId = "Canvas_1";
    // 这里只需要会话拥有权，未设置的显示名称与时间线元数据不参与断言。
    registry.append(std::move(entry));

    // 先取得旧版，再发布移除后的新版；两份快照允许同时存在。
    auto snapshot = registry.publishedSnapshot();
    registry.erase(0);
    // 移除新注册表条目不应穿透到仍被读者使用的旧快照。
    session.reset();
    if ( sessionObserver.expired() ) {
        // 本地会话句柄已释放，此时旧快照必须独立维持在途访问的有效性。
        XERROR("Published reader handle did not protect the in-flight session");
        return false;
    }

    // 主动结束最后一个旧版读者，应立即解除剩余强引用而非等待下一次发布。
    snapshot.reset();
    if ( !sessionObserver.expired() ) {
        // 检查发生在 registry 析构前，防止析构清理掩盖运行中的持有泄漏。
        XERROR("Session remained alive after the final snapshot reader left");
        return false;
    }
    return true;
}

/// @brief 验证图集快照替换后旧读句柄仍有效且新读者获取新值。
/// @return 新旧图集快照均符合预期时返回 true。
/// 使用同一纹理 ID 改变 UV，区分更新表内容与只增加新键的情况。
bool testAtlasSnapshotReaderLifetime()
{
    constexpr std::uint32_t TEXTURE_ID = 7U;
    // 小型单纹理表足以观察快照隔离，不需要构建真实 GPU 图集。
    MMM::Logic::RenderSyncRegistry registry;

    const std::unordered_map<std::uint32_t, glm::vec4> firstMap{
        { TEXTURE_ID, glm::vec4{ 0.0F, 0.0F, 0.25F, 0.5F } }
    };
    registry.setAtlasUVMap("Canvas_0", firstMap);
    // 此别名句柄指向快照子表，必须通过父快照的所有权保活数据。
    auto firstSnapshot = registry.getAtlasUVMap("Canvas_0");

    const std::unordered_map<std::uint32_t, glm::vec4> secondMap{
        // 选择二进制可精确表示的值，避免近似浮点比较干扰版本区分。
        { TEXTURE_ID, glm::vec4{ 0.0F, 0.0F, 0.75F, 1.0F } }
    };
    registry.setAtlasUVMap("Canvas_0", secondMap);
    // 新读者应看到新值，旧读者仍须读到旧值，不能原地覆写共享容器。
    const auto secondSnapshot = registry.getAtlasUVMap("Canvas_0");
    // 两个读句柄同时保留，显式覆盖图集替换时旧子表仍可访问的场景。

    if ( firstSnapshot->at(TEXTURE_ID).z != 0.25F ||
         secondSnapshot->at(TEXTURE_ID).z != 0.75F ) {
        // z 分量是本例区分两个版本的观测值，UV 映射不能跨版本串改。
        XERROR(
            "Atlas snapshot replacement invalidated a reader or returned stale "
            "data");
        return false;
    }

    registry.eraseCamera("Canvas_0");
    // 未注册 Basic2DCanvas 回退表，删除后新查询必须得到空图集。
    if ( !registry.getAtlasUVMap("Canvas_0")->empty() ) {
        // 查询入口不应为了延长旧读者寿命而继续向新读者返回已删除的记录。
        XERROR("Erased camera still exposed a retired atlas snapshot");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行注册表快照生命周期回归测试。
/// @return 全部测试通过时返回 0。
/// 非零退出码供测试运行器报告失败，无需解析日志文本来判断结果。
/// 不使用 assert，发布构建关闭断言时仍执行拥有权检查。
int main()
{
    // 三种场景依次覆盖无读者释放、有读者保活以及别名子表的快照隔离。
    // 遇到首个失败即返回非零，日志保留对应场景而不叠加后续诊断。
    return testErasedSessionIsReleased() &&
                   testReaderHandleControlsRetiredSessionLifetime() &&
                   testAtlasSnapshotReaderLifetime()
               ? 0
               : 1;
}
