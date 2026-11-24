#include "FormatTestHelpers.hpp"
#include "TestHelper.hpp"
#include "mmm/beatmap/BeatMap.h"

#include <fstream>

/**
 * @file OSUConsistencyTest.cpp
 * @brief 对单个 osu! 谱面执行文本章节和领域模型两层往返检查。
 *
 * 第一层比较七个主要文本章节，第二层使用共享帮助器比较重新加载后的通用谱面
 * 数据。两层必须同时通过；生成文件由调用方放入构建测试输出目录，来源只读。
 */

/// @brief 当前文本帮助器检查的 osu! 主要章节数量。
static constexpr int TOTAL_OSU_SECTIONS = 7;

/// @brief 用同一谱面覆盖普通单键和同时含有组合标志的长条。
/// @param output 当前用例的输出文件，用作夹具的命名空间。
/// @return 两类物件各自落入正确容器，长条持续时间保持不变。
/// @details 原始夹具使用整数位标志，长条和单键都有新增组合位，
/// 以证明只有长条位决定分类。
static bool checkHoldTypeFlags(const std::filesystem::path& output)
{
    auto fixture = output;
    // 保留 osu 扩展名，让生产加载器按扩展名进入相同的格式分派。
    fixture.replace_extension(".hold-flags.osu");
    {
        // 每个调用以自己的输出名生成夹具，三个 CTest 可以并行运行。
        // 用合法时间点及四键模式限定场景，避免其他格式回退影响分类。
        std::ofstream file(fixture);
        file << "osu file format v14\n[General]\nMode:3\n"
                "[Metadata]\nTitle:hold-flags\n[Difficulty]\nCircleSize:4\n"
                "[TimingPoints]\n0,500,4,2,0,100,1,0\n[HitObjects]\n"
                "64,192,1000,132,0,2000:0:0:0:0:\n"
                "192,192,1500,5,0,0:0:0:0:\n";
        if ( !file ) return false;
    }
    // 保存前检查模型，防止两次加载都错误地丢弃长条时往返测试仍通过。
    const auto      map = MMM::BeatMap::loadFromFile(fixture);
    std::error_code error;
    // 保留目录内已有测试输出，只移除本用例生成的输入。
    std::filesystem::remove(fixture, error);
    return map.m_noteData.holds.size() == 1 &&
           map.m_noteData.holds.front().m_duration == 1000.0 &&
           map.m_noteData.notes.size() == 1;
}

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

    if ( !checkHoldTypeFlags(output) ) return 1;

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
