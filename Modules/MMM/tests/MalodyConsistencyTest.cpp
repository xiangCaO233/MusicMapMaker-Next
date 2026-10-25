#include "FormatTestHelpers.hpp"
#include "TestHelper.hpp"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

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

/// @brief 为零点且同参考 BPM 的资源建立不增加初始化事件的独立期望。
/// @param model 来源模型，只读取其参考 BPM，不移动内容。
/// @param input 原始 MC 文件，只读。
/// @param expectedPath 构建目录中写出的预期 JSON。
/// @return 夹具符合约定且原始分区完整写入期望文件时为真。
/// @note 此组真实资源均从零点同 BPM 开始；不同 BPM 和负时间由矩阵负责。
/// @details 相同 BPM 不应再为同刻用户绿线腾出一拍；JSON 与模型两层比较
/// 同时约束这一点，不能靠改写期望模型来掩盖新增位移。
static bool prepareInitialTimingExpectation(
    MMM::BeatMap& model, const std::filesystem::path& input,
    const std::filesystem::path& expectedPath)
{
    std::ifstream stream(input);
    auto          data = nlohmann::json::parse(stream, nullptr, false);
    // 此函数对夹具形态明确设限；以后增加非零点资源时须补独立期望，不能
    // 在未知输入上直接用当前导出器的结果充当测试基准。
    if ( data.is_discarded() || data["time"].empty() ||
         data["time"][0]["beat"] != nlohmann::json::array({ 0, 0, 1 }) ||
         std::abs(data["time"][0].value("delay", 0.0)) > 1e-6 )
        return false;
    const double reference = model.m_baseMapMetadata.preference_bpm;
    // 此处只使用原始模型给出的参考 BPM，不采用待比较输出中的红线数值。
    // 因而导出器误写参考 BPM 时，分区检查和逻辑检查都会保持独立失败。
    // 浮点 BPM 使用容差判断语义相等，资源中的 329.99999999999966 仍是 330。
    const bool sameBpm =
        std::abs(data["time"][0]["bpm"].get<double>() - reference) < 1e-6;
    if ( sameBpm ) {
        // 相同 BPM 不新增红绿线，不再为用户零点 Scroll 腾出一拍。
        std::ofstream output(expectedPath);
        output << data.dump();
        return output.good();
    }
    // 新策略下这两份同 BPM 夹具完全不变；未知夹具必须另给独立期望。
    return false;
}

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
    // 实际输出经过正式加载器，不能只把构建中的 JSON 直接转成期望模型。
    // 两份模型都同步后再比较，排除派生索引建立时机造成的假差异。
    m2.sync();

    // 分区比较定位磁盘表示回归，不受 JSON 对象键顺序影响。
    auto expectedPath = output;
    expectedPath += ".expected.mc";
    // 额外后缀使预期文档与实际产物分离，失败后可分别人工核对。
    if ( !prepareInitialTimingExpectation(m1, input, expectedPath) ) return 1;
    // 比较器仍检查四个完整分区，不能因新增参考段而跳过 time/effect。
    // 逻辑比较继续检查所有物件、长条时长与自动采样字段。
    int sectionPassed = MMM::Test::compareMalodySections(expectedPath, output);
    XINFO("Malody JSON Section Comparison: {}/{} sections passed",
          sectionPassed,
          TOTAL_MALODY_SECTIONS);

    // 第二层比较统一模型；Malody 可表达自动采样，因此启用该可选层。
    bool logicPassed = MMM::Test::compareBeatMaps(m1, m2, true);
    // 全量比较包含所有物件，初始化事件之外不得出现遗漏。
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
    // 任一分区或模型失败都使测试失败，不能按多数通过判定格式兼容。
    int totalTests = TOTAL_MALODY_SECTIONS + 1;
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
