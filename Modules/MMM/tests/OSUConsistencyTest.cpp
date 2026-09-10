#include "FormatTestHelpers.hpp"
#include "TestHelper.hpp"
#include "mmm/beatmap/BeatMap.h"

/**
 * @file OSUConsistencyTest.cpp
 * @brief 对单个 osu! 谱面执行文本章节和领域模型两层往返检查。
 *
 * 第一层比较七个主要文本章节，第二层使用共享帮助器比较重新加载后的通用谱面
 * 数据。两层必须同时通过；生成文件由调用方放入构建测试输出目录，来源只读。
 */

/// @brief 当前文本帮助器检查的 osu! 主要章节数量。
static constexpr int TOTAL_OSU_SECTIONS = 7;

/// @brief 运行指定 osu! 输入与输出路径的一致性测试。
/// @param argc 参数数量，必须包含输入和输出路径。
/// @param argv 参数数组。
/// @return 文本七节与领域模型全部一致时返回 0，否则返回 1。
int main(int argc, char* argv[])
{
    // 路径不完整时不猜测默认资源，直接返回参数错误。
    if ( argc < 3 ) return 1;
    std::filesystem::path input  = argv[1];
    std::filesystem::path output = argv[2];

    XINFO("========================================");
    XINFO("  OSU Consistency Test: {}", input.filename().string());
    XINFO("========================================");

    // 第一轮先走真实加载和保存入口，生成待比较的 osu! 文本。
    MMM::BeatMap m1 = MMM::BeatMap::loadFromFile(input);
    m1.sync();

    // 写出拒绝表示来源包含 osu! 无法无损表达的数据，后续比较没有意义。
    if ( !m1.saveToFile(output) ) return 1;

    MMM::BeatMap m2 = MMM::BeatMap::loadFromFile(output);
    m2.sync();

    // 文本比较验证章节级字段，不用文件整体字节相等限制合法排版变化。
    int sectionPassed = MMM::Test::compareOSUSections(input, output);
    XINFO("OSU Text Section Comparison: {}/{} sections passed",
          sectionPassed,
          TOTAL_OSU_SECTIONS);

    // 第二轮比较通用模型，覆盖文本层无法直接判断的数值和对象语义。
    bool logicPassed = MMM::Test::compareBeatMaps(m1, m2);
    if ( logicPassed ) {
        XINFO("[OSU Logical Consistency]: PASS");
    } else {
        XERROR("[OSU Logical Consistency]: FAIL");
    }

    // 汇总把逻辑比较作为第八项，最终仍要求七个章节全部通过。
    int totalPassed = sectionPassed + (logicPassed ? 1 : 0);
    int totalTests  = TOTAL_OSU_SECTIONS + 1;
    XINFO("========================================");
    if ( sectionPassed == TOTAL_OSU_SECTIONS && logicPassed ) {
        XINFO("  OSU Consistency: ALL {}/{} PASSED", totalPassed, totalTests);
        return 0;
    } else {
        XERROR("  OSU Consistency: {}/{} passed, {} failed",
               totalPassed,
               totalTests,
               totalTests - totalPassed);
        return 1;
    }
}
