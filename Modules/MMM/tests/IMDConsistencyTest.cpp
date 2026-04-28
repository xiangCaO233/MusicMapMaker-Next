#include "TestHelper.hpp"
#include "mmm/beatmap/BeatMap.h"
#include <iostream>

int main(int argc, char* argv[])
{
    if ( argc < 3 ) return 1;
    std::filesystem::path input  = argv[1];
    std::filesystem::path output = argv[2];

    XINFO("Running IMD Consistency Test: {}", input.string());

    MMM::BeatMap m1 = MMM::BeatMap::loadFromFile(input);
    m1.sync();

    if ( !m1.saveToFile(output) ) return 1;

    MMM::BeatMap m2 = MMM::BeatMap::loadFromFile(output);
    m2.sync();

    if ( !MMM::Test::compareBeatMaps(m1, m2) ) {
        XERROR("IMD Consistency Check Failed!");
        return 1;
    }

    XINFO("IMD Consistency Check Passed.");
    return 0;
}
