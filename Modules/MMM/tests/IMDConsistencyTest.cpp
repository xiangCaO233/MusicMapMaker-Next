#include "FormatTestHelpers.hpp"
#include "TestHelper.hpp"
#include "mmm/beatmap/BeatMap.h"

static constexpr int TOTAL_IMD_CHUNKS = 5;

int main(int argc, char* argv[])
{
    if ( argc < 3 ) return 1;
    std::filesystem::path input  = argv[1];
    std::filesystem::path output = argv[2];
    // 确保输出文件名包含轨道数信息，以便 LoadRMMap 能正确识别
    std::string orig_name = input.filename().string();
    output = output.parent_path() / orig_name;

    XINFO("========================================");
    XINFO("  IMD Consistency Test: {}", input.filename().string());
    XINFO("========================================");

    // ── 第一轮：原始二进制分块对比 (Load → Save 后直接对比原始文件流) ──
    MMM::BeatMap m1 = MMM::BeatMap::loadFromFile(input);
    m1.sync();

    if ( !m1.saveToFile(output) ) return 1;

    MMM::BeatMap m2 = MMM::BeatMap::loadFromFile(output);
    m2.sync();

    // 二进制分块对比
    int binaryPassed = MMM::Test::compareIMDChunks(input, output);
    XINFO("IMD Binary Chunk Comparison: {}/{} chunks passed",
          binaryPassed,
          TOTAL_IMD_CHUNKS);

    // ── 第二轮：逻辑一致性对比 ──
    bool logicPassed = MMM::Test::compareBeatMaps(m1, m2);
    if ( logicPassed ) {
        XINFO("[IMD Logical Consistency]: PASS");
    } else {
        XERROR("[IMD Logical Consistency]: FAIL");
    }

    // ── 汇总 ──
    int totalPassed = binaryPassed + (logicPassed ? 1 : 0);
    int totalTests  = TOTAL_IMD_CHUNKS + 1;
    XINFO("========================================");
    bool isConsideredPassed = (binaryPassed == TOTAL_IMD_CHUNKS && logicPassed) || (binaryPassed >= 3 && logicPassed);
    if ( isConsideredPassed ) {
        XINFO("  IMD Consistency: PASSED (Binary: {}/{}, Logic: PASS)", binaryPassed, TOTAL_IMD_CHUNKS);
        return 0;
    } else {
        XERROR("  IMD Consistency: {}/{} passed, {} failed", totalPassed, totalTests, totalTests - totalPassed);
        return 1;
    }
}
