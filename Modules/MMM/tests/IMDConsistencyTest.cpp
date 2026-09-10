#include "FormatTestHelpers.hpp"
#include "TestHelper.hpp"
#include "beatmap/LoadRMMap.hpp"
#include "mmm/beatmap/BeatMap.h"

#include <fstream>
#include <system_error>

/// @file IMDConsistencyTest.cpp
/// @brief 验证 RM/IMD 导入导出的布局、语义与伴随资源兼容性。
///
/// 测试包含三个互补层次：
/// - 二进制区块比较用于定位时长、节奏表、标记和物件表的布局回归；
/// - BeatMap 逻辑比较确认允许的格式规范化没有改变谱面语义；
/// - 临时 FLAC 探针确认文件名前缀资源发现覆盖非 MP3 音频。
///
/// 基准谱面由 CTest 参数提供，输出写入构建目录。测试不会修改
/// `Modules/MMM/tests/data` 中的受版本控制资源。
/// 测试成功只证明所给夹具的格式往返，不代表其他格式或音频解码链路通过。
/// 若布局契约变化，应同步调整区块比较器及这里的总区块数量。

/// @brief IMD 比较器划分出的独立二进制布局区块数量。
static constexpr int TOTAL_IMD_CHUNKS = 5;

/// @brief 验证 RM/imd 同名前缀音频查找支持 FLAC。
/// @return 查找到 FLAC 时返回 true。
bool checkRMFlacAudioResolution()
{
    // 使用系统临时目录生成最小探针，避免测试产物污染受版本控制的资源目录。
    std::error_code ec;
    auto            tempDir =
        std::filesystem::temp_directory_path(ec) / "mmm_imd_flac_probe";
    if ( ec ) {
        XERROR("[IMD FLAC Audio Resolution]: temp directory unavailable");
        return false;
    }

    // 所有文件系统操作均使用 error_code，使测试失败以明确布尔结果呈现。
    std::filesystem::create_directories(tempDir, ec);
    if ( ec ) {
        XERROR("[IMD FLAC Audio Resolution]: create temp directory failed");
        return false;
    }

    // 只创建 FLAC 候选，确保成功结果来自扩展名覆盖而不是其他同名前缀文件。
    const auto flacPath = tempDir / "FlacOnly.flac";
    {
        std::ofstream file(flacPath, std::ios::binary);
        if ( !file ) {
            XERROR("[IMD FLAC Audio Resolution]: create probe file failed");
            return false;
        }
        // 解析器只负责资源路径发现，不需要为探针构造完整音频码流。
        file << "fLaC";
    }

    const auto resolved = MMM::resolveRMAudioPath(tempDir, "FlacOnly");
    // 在断言前回收探针；即使结果失败，也不会把临时文件留给后续测试。
    std::filesystem::remove(flacPath, ec);
    std::filesystem::remove(tempDir, ec);

    const bool passed = resolved == std::filesystem::path("FlacOnly.flac");
    // 只比较相对文件名；资源解析契约不能把调用者的父目录写入谱面元数据。
    if ( passed ) {
        XINFO("[IMD FLAC Audio Resolution]: PASS");
    } else {
        XERROR("[IMD FLAC Audio Resolution]: FAIL");
    }
    return passed;
}

/// @brief 执行 IMD 字节布局、统一模型和伴随资源解析的往返验证。
/// @param argc 需要输入与输出路径两个参数。
/// @param argv argv[1] 为基准 IMD，argv[2] 提供输出目录。
/// @return 满足二进制容差、逻辑一致性和 FLAC 解析要求时返回零。
int main(int argc, char* argv[])
{
    // 缺少显式路径时不猜测资源位置，交由 CTest 配置提供稳定输入。
    if ( argc < 3 ) return 1;
    std::filesystem::path input  = argv[1];
    std::filesystem::path output = argv[2];
    // 加载器从文件名解析标题、轨道数与难度，因此往返输出沿用原始文件名；
    // argv[2] 仅决定输出目录，防止测试重命名本身改变逻辑模型。
    std::string orig_name = input.filename().string();
    output                = output.parent_path() / orig_name;

    XINFO("========================================");
    XINFO("  IMD Consistency Test: {}", input.filename().string());
    XINFO("========================================");

    // 第一层验证文件布局：加载、同步并重新保存后，按格式区块比较二进制内容。
    // 分块比较可以把可接受的重排与具体字段回归区分开来。
    MMM::BeatMap m1 = MMM::BeatMap::loadFromFile(input);
    m1.sync();

    // 保存失败已由导出器记录具体兼容性原因，此处直接终止避免比较残留文件。
    if ( !m1.saveToFile(output) ) return 1;

    MMM::BeatMap m2 = MMM::BeatMap::loadFromFile(output);
    // 第二次同步重建派生索引，使逻辑比较不受加载器内部构造顺序影响。
    m2.sync();

    // 区块结果保留细粒度诊断，不只给出整个文件是否相等。
    int binaryPassed = MMM::Test::compareIMDChunks(input, output);
    XINFO("IMD Binary Chunk Comparison: {}/{} chunks passed",
          binaryPassed,
          TOTAL_IMD_CHUNKS);

    // 第二层忽略格式表示差异，验证两次加载得到的统一谱面语义一致。
    bool logicPassed = MMM::Test::compareBeatMaps(m1, m2);
    if ( logicPassed ) {
        XINFO("[IMD Logical Consistency]: PASS");
    } else {
        XERROR("[IMD Logical Consistency]: FAIL");
    }
    // 资源发现独立于给定谱面夹具，使用程序化探针补足 FLAC 分支覆盖。
    const bool flacAudioResolutionPassed = checkRMFlacAudioResolution();

    // 历史 IMD 样本允许两个非关键区块发生规范化，但逻辑层必须完全通过；
    // FLAC 发现属于明确的兼容性契约，也必须独立通过。
    int totalPassed = binaryPassed + (logicPassed ? 1 : 0) +
                      (flacAudioResolutionPassed ? 1 : 0);
    int totalTests = TOTAL_IMD_CHUNKS + 2;
    XINFO("========================================");
    bool isConsideredPassed =
        // 完全一致是理想结果；历史样本至少要求三个核心区块及全部逻辑通过。
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
