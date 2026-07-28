#include "FormatTestHelpers.hpp"
#include "TestHelper.hpp"
#include "mmm/beatmap/BeatMap.h"

static constexpr int TOTAL_MALODY_SECTIONS = 4;

int main(int argc, char* argv[])
{
    if ( argc < 3 ) return 1;
    std::filesystem::path input  = argv[1];
    std::filesystem::path output = argv[2];

    XINFO("========================================");
    XINFO("  Malody Consistency Test: {}", input.filename().string());
    XINFO("========================================");

    // ── 第一轮：JSON 分段落对比 ──
    MMM::BeatMap m1 = MMM::BeatMap::loadFromFile(input);
    m1.sync();

    if ( !m1.saveToFile(output) ) return 1;

    MMM::BeatMap m2 = MMM::BeatMap::loadFromFile(output);
    m2.sync();

    // JSON 分段落对比
    int sectionPassed = MMM::Test::compareMalodySections(input, output);
    XINFO("Malody JSON Section Comparison: {}/{} sections passed",
          sectionPassed,
          TOTAL_MALODY_SECTIONS);

    // ── 第二轮：逻辑一致性对比 ──
    bool logicPassed = MMM::Test::compareBeatMaps(m1, m2, true);
    if ( logicPassed ) {
        XINFO("[Malody Logical Consistency]: PASS");
    } else {
        XERROR("[Malody Logical Consistency]: FAIL");
        XERROR("  m1 total notes: {}, m2 total notes: {}",
               m1.m_allNotes.size(),
               m2.m_allNotes.size());
        XERROR("  m1 audio samples: {}, m2 audio samples: {}",
               m1.m_audioSamples.size(),
               m2.m_audioSamples.size());
    }

    // ── 汇总 ──
    int totalPassed = sectionPassed + (logicPassed ? 1 : 0);
    int totalTests  = TOTAL_MALODY_SECTIONS + 1;
    XINFO("========================================");
    if ( sectionPassed == TOTAL_MALODY_SECTIONS && logicPassed ) {
        XINFO(
            "  Malody Consistency: ALL {}/{} PASSED", totalPassed, totalTests);
        return 0;
    } else {
        XERROR("  Malody Consistency: {}/{} passed, {} failed",
               totalPassed,
               totalTests,
               totalTests - totalPassed);
        return 1;
    }
}
