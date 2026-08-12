#include "network/collaboration/CollaborationBuildFingerprint.h"

#include "config/Utf8Path.h"
#include "network/AssetSyncService.h"
#include "network/UpdateChecker.h"

#include <algorithm>
#include <mutex>

namespace MMM::Network::Collaboration
{
namespace
{
std::once_flag s_fingerprintInitialization;
std::string    s_fingerprint;

/// @brief 从当前主程序路径读取并计算一次二进制 SHA-256。
void initializeFingerprintStorage()
{
    const auto executablePath = Network::UpdateChecker::currentExecutablePath();
    if ( executablePath.empty() ) return;
    s_fingerprint = Network::AssetSyncService::sha256File(
        Config::utf8ToPath(executablePath));
}
}  // namespace

bool initializeCollaborationBuildFingerprint()
{
    std::call_once(s_fingerprintInitialization, initializeFingerprintStorage);
    return isValidCollaborationBuildFingerprint(s_fingerprint);
}

const std::string& collaborationBuildFingerprint()
{
    static_cast<void>(initializeCollaborationBuildFingerprint());
    return s_fingerprint;
}

bool isValidCollaborationBuildFingerprint(std::string_view fingerprint)
{
    return fingerprint.size() == 64U &&
           std::all_of(fingerprint.begin(), fingerprint.end(), [](char value) {
               return (value >= '0' && value <= '9') ||
                      (value >= 'a' && value <= 'f');
           });
}
}  // namespace MMM::Network::Collaboration
