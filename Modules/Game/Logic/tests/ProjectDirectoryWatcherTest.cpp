#include "logic/ProjectDirectoryWatcher.h"

#include <filesystem>
#include <iostream>

int main()
{
    using MMM::Logic::ProjectDirectoryWatcher;

    if ( ProjectDirectoryWatcher::isRelevantProjectPathChange(
             std::filesystem::path("mmm_project.json")) ) {
        std::cerr
            << "project metadata writes must not trigger a resource scan\n";
        return 1;
    }
    if ( !ProjectDirectoryWatcher::isRelevantProjectPathChange(
             std::filesystem::path("audio") / "effect.wav") ) {
        std::cerr << "audio resource changes must trigger a resource scan\n";
        return 1;
    }
    if ( !ProjectDirectoryWatcher::isRelevantProjectPathChange(
             std::filesystem::path("nested") / "mmm_project.json") ) {
        std::cerr << "only root project metadata should be ignored\n";
        return 1;
    }
    return 0;
}
