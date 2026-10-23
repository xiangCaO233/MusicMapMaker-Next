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

/// @brief 为零点首红线资源夹具建立独立的初始化期望。
/// @param model 来源模型，转换为包含必要前导段的期望模型。
/// @param input 原始 MC 文件，只读。
/// @param expectedPath 构建目录中写出的预期 JSON。
/// @return 夹具符合约定且预期文件写入成功时为真。
/// @note 此组真实资源均从零点红线开始；非零点和回卷由矩阵测试负责。
/// @note 只允许规则明确规定的初始化配对和整拍平移，其余分区继续完整比较。
/// @details 预期 JSON 从原始资源按独立规则生成，不调用生产首拍归一化 helper。
/// 一份预期用于原始字段分区比较，另一份值模型用于物件和采样的绝对时间比较。
/// 这两层不能互相替代：JSON 拍位正确不代表 delay、SOUND offset 一定同步。
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
    if ( !data.contains("effect") ) data["effect"] = nlohmann::json::array();
    const bool userGreen = std::any_of(
        data["effect"].begin(), data["effect"].end(), [](const auto& effect) {
            return effect.contains("scroll") &&
                   effect["beat"] == nlohmann::json::array({ 0, 0, 1 });
        });
    // 零点有任何用户绿线都腾出一拍，包括用户明确写入的一倍速绿线。
    // 无绿线的另一份真实资源只新增初始化绿线，不改变谱面绝对时间。
    const bool   prefix   = !sameBpm || userGreen;
    const double shift    = prefix ? 60000.0 / reference : 0;
    const auto   songFile = data["meta"]["song"].value("file", std::string{});
    if ( prefix ) {
        // 严格保留原来的全部 timing，不通过丢弃首条用户绿线达标。
        // 本夹具参考 BPM 与首 BPM 相同，后移一拍可直接增加整数拍号；
        // 长条尾拍也属绝对拍位，Slide seg 则必须保持相对间距。
        if ( !sameBpm ) return false;
        for ( auto& timing : data["time"] )
            timing["beat"][0] = timing["beat"][0].get<int>() + 1;
        for ( auto& effect : data["effect"] )
            effect["beat"][0] = effect["beat"][0].get<int>() + 1;
        for ( auto& node : data["note"] ) {
            // Main 文件补静音承担音乐位移，SOUND 的原始 offset
            // 不随谱面叠加一拍。
            if ( node.value("sound", std::string{}) == songFile &&
                 node.contains("type") )
                continue;
            node["beat"][0] = node["beat"][0].get<int>() + 1;
            if ( node.contains("endbeat") )
                node["endbeat"][0] = node["endbeat"][0].get<int>() + 1;
        }
        data["time"].insert(
            data["time"].begin(),
            nlohmann::json{ { "beat", nlohmann::json::array({ 0, 0, 1 }) },
                            { "bpm", reference },
                            { "delay", 0 } });
        for ( auto& timing : model.m_timings ) timing.m_timestamp += shift;
        // 模型层使用绝对毫秒，独立于 JSON 层的一拍整数变换。
        // 遍历各值容器一次，避免折线引用使同一个子物件被重复平移。
        for ( auto& note : model.m_noteData.notes ) note.m_timestamp += shift;
        for ( auto& note : model.m_noteData.holds ) note.m_timestamp += shift;
        for ( auto& note : model.m_noteData.flicks ) note.m_timestamp += shift;
        for ( auto& note : model.m_noteData.polylines )
            note.m_timestamp += shift;
        for ( auto& sample : model.m_audioSamples )
            if ( sample.m_audioResourceId != songFile )
                sample.m_timestamp += shift;
        MMM::Timing red;
        // 只补初始化事件，不修改用户原红线的 BPM、delay 或来源元数据。
        red.m_timingEffect = MMM::TimingEffect::BPM;
        red.m_bpm = red.m_timingEffectParameter = reference;
        red.m_beat_length                       = shift;
        model.m_timings.push_back(red);
    }
    // 新绿线恰好增加一次，原来的 Scroll 仍由完整分区比较确认值和顺序。
    data["effect"].insert(
        data["effect"].begin(),
        nlohmann::json{ { "beat", nlohmann::json::array({ 0, 0, 1 }) },
                        { "scroll", 1.0 } });
    MMM::Timing green;
    // 绿线参数是一倍 Scroll，其附带 BPM 使用参考值，与输出零点红线一致。
    green.m_timingEffect = MMM::TimingEffect::SCROLL;
    green.m_bpm          = reference;
    green.m_beat_length = green.m_timingEffectParameter = 1;
    model.m_timings.push_back(green);
    // 相同时间保持原顺序，额外初始化红线先于初始化绿线。
    // sync 建立派生索引但不承诺重排来源容器；测试显式对齐加载器的时间顺序。
    std::stable_sort(model.m_timings.begin(),
                     model.m_timings.end(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.m_timestamp < rhs.m_timestamp;
                     });
    model.sync();
    std::ofstream output(expectedPath);
    // 预期文档也写入构建目录，继续复用现有完整分区比较器和失败日志。
    output << data.dump();
    return output.good();
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
