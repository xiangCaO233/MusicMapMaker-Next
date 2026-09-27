#include "FormatTestHelpers.hpp"
#include "TestHelper.hpp"
#include "mmm/beatmap/BeatMap.h"

/// @file MalodyConsistencyTest.cpp
/// @brief 验证 Malody JSON 分区与统一谱面语义的完整往返。
///
/// 文本层按 meta、time、effect、note 四个逻辑分区比较，模型层另行比较
/// Timing、物件和自动采样，防止读写器出现能够相互抵消的对称错误。
/// 输入夹具由 CTest 分别提供普通谱面与折线谱面，输出始终位于构建目录。
/// 该测试不要求 JSON 文本逐字节相等，只要求规范化后的分区内容一致。
/// 自动采样比较在本格式中启用，因为 Malody 能显式表达 sound 事件。
/// 任一层失败都返回非零，分区通过数不能抵扣模型差异。

/// @brief Malody 分区比较器当前检查的顶层逻辑区块数量。
static constexpr int TOTAL_MALODY_SECTIONS = 4;

/// @brief 执行指定 Malody 谱面的格式与逻辑往返验证。
/// @param argc 需要输入与输出路径两个参数。
/// @param argv argv[1] 为基准 MC 文件，argv[2] 为构建目录输出。
/// @return 四个 JSON 区块和统一模型全部一致时返回零。
int main(int argc, char* argv[])
{
    // 路径必须由 CTest 明确提供，避免隐式依赖当前工作目录。
    if ( argc < 3 ) return 1;
    std::filesystem::path input  = argv[1];
    std::filesystem::path output = argv[2];

    XINFO("========================================");
    XINFO("  Malody Consistency Test: {}", input.filename().string());
    XINFO("========================================");

    // 第一层执行加载、同步和保存，得到可与原始 JSON 分区比较的输出。
    MMM::BeatMap m1 = MMM::BeatMap::loadFromFile(input);
    m1.sync();

    // 导出失败时终止，禁止读取上一次运行残留的同名文件。
    if ( !m1.saveToFile(output) ) return 1;

    MMM::BeatMap m2 = MMM::BeatMap::loadFromFile(output);
    // 两份模型都同步后再比较，排除派生索引建立时机造成的假差异。
    m2.sync();

    // 分区比较定位磁盘表示回归，不受 JSON 对象键顺序影响。
    int sectionPassed = MMM::Test::compareMalodySections(input, output);
    XINFO("Malody JSON Section Comparison: {}/{} sections passed",
          sectionPassed,
          TOTAL_MALODY_SECTIONS);

    // 第二层比较统一模型；Malody 可表达自动采样，因此启用该可选层。
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

    // 磁盘分区和逻辑模型必须同时全部通过，不能互相抵扣。
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
