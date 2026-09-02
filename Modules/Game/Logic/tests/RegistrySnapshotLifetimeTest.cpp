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
bool testErasedSessionIsReleased()
{
    MMM::Logic::SessionRegistry registry;
    auto session = std::make_shared<MMM::Logic::BeatmapSession>();
    std::weak_ptr<MMM::Logic::BeatmapSession> sessionObserver = session;

    MMM::Logic::SessionEntry entry;
    entry.session                  = session;
    entry.cameraId                 = "Canvas_0";
    entry.displayName              = "Lifetime Test";
    entry.audioTimelineFingerprint = "complete-timeline";
    entry.mainAudioSyncFingerprint = "main-sync";
    registry.append(std::move(entry));

    {
        const auto snapshot = registry.publishedSnapshot();
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

    registry.erase(0);
    session.reset();
    if ( !sessionObserver.expired() ) {
        XERROR("Erased session remained owned by a retired registry snapshot");
        return false;
    }
    return true;
}

/// @brief 验证并发读句柄离开前会保护旧会话，离开后立即释放。
/// @return 旧快照生命周期符合读句柄范围时返回 true。
bool testReaderHandleControlsRetiredSessionLifetime()
{
    MMM::Logic::SessionRegistry registry;
    auto session = std::make_shared<MMM::Logic::BeatmapSession>();
    std::weak_ptr<MMM::Logic::BeatmapSession> sessionObserver = session;

    MMM::Logic::SessionEntry entry;
    entry.session  = session;
    entry.cameraId = "Canvas_1";
    registry.append(std::move(entry));

    auto snapshot = registry.publishedSnapshot();
    registry.erase(0);
    session.reset();
    if ( sessionObserver.expired() ) {
        XERROR("Published reader handle did not protect the in-flight session");
        return false;
    }

    snapshot.reset();
    if ( !sessionObserver.expired() ) {
        XERROR("Session remained alive after the final snapshot reader left");
        return false;
    }
    return true;
}

/// @brief 验证图集快照替换后旧读句柄仍有效且新读者获取新值。
/// @return 新旧图集快照均符合预期时返回 true。
bool testAtlasSnapshotReaderLifetime()
{
    constexpr std::uint32_t        TEXTURE_ID = 7U;
    MMM::Logic::RenderSyncRegistry registry;

    const std::unordered_map<std::uint32_t, glm::vec4> firstMap{
        { TEXTURE_ID, glm::vec4{ 0.0F, 0.0F, 0.25F, 0.5F } }
    };
    registry.setAtlasUVMap("Canvas_0", firstMap);
    auto firstSnapshot = registry.getAtlasUVMap("Canvas_0");

    const std::unordered_map<std::uint32_t, glm::vec4> secondMap{
        { TEXTURE_ID, glm::vec4{ 0.0F, 0.0F, 0.75F, 1.0F } }
    };
    registry.setAtlasUVMap("Canvas_0", secondMap);
    const auto secondSnapshot = registry.getAtlasUVMap("Canvas_0");

    if ( firstSnapshot->at(TEXTURE_ID).z != 0.25F ||
         secondSnapshot->at(TEXTURE_ID).z != 0.75F ) {
        XERROR(
            "Atlas snapshot replacement invalidated a reader or returned stale "
            "data");
        return false;
    }

    registry.eraseCamera("Canvas_0");
    if ( !registry.getAtlasUVMap("Canvas_0")->empty() ) {
        XERROR("Erased camera still exposed a retired atlas snapshot");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行注册表快照生命周期回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testErasedSessionIsReleased() &&
                   testReaderHandleControlsRetiredSessionLifetime() &&
                   testAtlasSnapshotReaderLifetime()
               ? 0
               : 1;
}
