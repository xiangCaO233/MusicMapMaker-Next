#include "TestHelper.hpp"
#include "mmm/beatmap/BeatMap.h"
#include <iostream>

int main(int argc, char* argv[])
{
    if ( argc < 3 ) return 1;
    std::filesystem::path input  = argv[1];
    std::filesystem::path output = argv[2];

    XINFO("Running Malody Consistency Test: {}", input.string());

    MMM::BeatMap m1 = MMM::BeatMap::loadFromFile(input);
    m1.sync();

    if ( !m1.saveToFile(output) ) return 1;

    MMM::BeatMap m2 = MMM::BeatMap::loadFromFile(output);
    m2.sync();

    if ( !MMM::Test::compareBeatMaps(m1, m2) ) {
        XERROR("Malody Consistency Check Failed!");
        // 进一步检查哪些物件重复了
        XERROR("m1 total: {}, m2 total: {}",
               m1.m_allNotes.size(),
               m2.m_allNotes.size());
        return 1;
    }

    XINFO("Malody Consistency Check Passed.");
    return 0;
}
