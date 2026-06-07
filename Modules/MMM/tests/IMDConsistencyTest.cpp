#include "FormatTestHelpers.hpp"
#include "TestHelper.hpp"
#include "beatmap/LoadRMMap.hpp"
#include "mmm/beatmap/BeatMap.h"

#include <fstream>
#include <system_error>

static constexpr int TOTAL_IMD_CHUNKS = 5;

/// @brief 验证 RM/imd 同名前缀音频查找支持 FLAC。
/// @return 查找到 FLAC 时返回 true。
bool checkRMFlacAudioResolution()
{
    std::error_code ec;
    auto            tempDir =
        std::filesystem::temp_directory_path(ec) / "mmm_imd_flac_probe";
    if ( ec ) {
        XERROR("[IMD FLAC Audio Resolution]: temp directory unavailable");
        return false;
    }

    std::filesystem::create_directories(tempDir, ec);
    if ( ec ) {
        XERROR("[IMD FLAC Audio Resolution]: create temp directory failed");
        return false;
    }

    const auto flacPath = tempDir / "FlacOnly.flac";
    {
        std::ofstream file(flacPath, std::ios::binary);
        if ( !file ) {
            XERROR("[IMD FLAC Audio Resolution]: create probe file failed");
            return false;
        }
        file << "fLaC";
    }

    const auto resolved = MMM::resolveRMAudioPath(tempDir, "FlacOnly");
    std::filesystem::remove(flacPath, ec);
    std::filesystem::remove(tempDir, ec);

    const bool passed = resolved == std::filesystem::path("FlacOnly.flac");
    if ( passed ) {
        XINFO("[IMD FLAC Audio Resolution]: PASS");
    } else {
        XERROR("[IMD FLAC Audio Resolution]: FAIL");
    }
    return passed;
}

int main(int argc, char* argv[])
{
    if ( argc < 3 ) return 1;
    std::filesystem::path input  = argv[1];
    std::filesystem::path output = argv[2];
    // 确保输出文件名包含轨道数信息，以便 LoadRMMap 能正确识别
    std::string orig_name = input.filename().string();
    output                = output.parent_path() / orig_name;

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
    const bool flacAudioResolutionPassed = checkRMFlacAudioResolution();

    // ── 汇总 ──
    int totalPassed = binaryPassed + (logicPassed ? 1 : 0) +
                      (flacAudioResolutionPassed ? 1 : 0);
    int totalTests  = TOTAL_IMD_CHUNKS + 2;
    XINFO("========================================");
    bool isConsideredPassed =
        ((binaryPassed == TOTAL_IMD_CHUNKS && logicPassed) ||
         (binaryPassed >= 3 && logicPassed)) &&
        flacAudioResolutionPassed;
    if ( isConsideredPassed ) {
        XINFO("  IMD Consistency: PASSED (Binary: {}/{}, Logic: PASS)",
              binaryPassed,
              TOTAL_IMD_CHUNKS);
        return 0;
    } else {
        XERROR("  IMD Consistency: {}/{} passed, {} failed",
               totalPassed,
               totalTests,
               totalTests - totalPassed);
        return 1;
    }
}
