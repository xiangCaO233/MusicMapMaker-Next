#include "network/collaboration/CollaborationBuildFingerprint.h"

#include "config/Utf8Path.h"
#include "network/AssetSyncService.h"
#include "network/UpdateChecker.h"

#include <algorithm>

namespace MMM::Network::Collaboration
{
const std::string& collaborationBuildFingerprint()
{
    static const std::string fingerprint = [] {
        const auto executablePath =
            Network::UpdateChecker::currentExecutablePath();
        if ( executablePath.empty() ) return std::string{};
        return Network::AssetSyncService::sha256File(
            Config::utf8ToPath(executablePath));
    }();
    return fingerprint;
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
