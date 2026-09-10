#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

/**
 * @brief 谱面导入导出一致性测试工具
 *
 * 流程:
 * 1. 读取 <input> 为 map1
 * 2. 将 map1 写出到 <output>
 * 3. 读取 <output> 为 map2
 * 4. 对比 map1 与 map2
 *
 * 此测试以原生 MMM 为输出格式，因此要求来源格式中的公共模型及自动采样都能
 * 完整保存。文本排版、JSON 键顺序和来源格式私有字段不属于比较范围。
 */
/// @file MapConsistencyTest.cpp
/// @brief 从外部格式导入后验证原生 MMM 对公共模型的承接能力。
///
/// 当前比较层级：
/// - 轨道元数据严格一致；
/// - Timing 数量和逐项基础字段严格一致；
/// - 统一物件数量一致，并抽样首个物件身份；
/// - 全部自动采样在规范排序后逐项一致。
///
/// 自动采样排序键依次为：
/// - 时间戳；
/// - 轨道；
/// - 音频资源标识；
/// - 资源内偏移。
///
/// 排序只作用于测试创建的观察指针数组，不修改两份 BeatMap。这样既能忽略
/// 来源格式的数组顺序差异，也不会让比较辅助逻辑改变被测对象。
/// 比较器在第一处不一致时记录索引并返回，避免继续访问已经错位的数组。
/// 当前物件内容只做抽样，这是既有测试边界，不应误解为完整格式验收。
/// 更细的节点字段由来源格式一致性测试和原生元数据测试覆盖。

/// @brief 比较来源谱面与原生 MMM 重载谱面的公共语义。
/// @param m1 来源格式加载后的基准模型。
/// @param m2 写为 MMM 后重新加载的模型。
/// @return 轨道、Timing、物件抽样与全部自动采样一致时返回 true。
bool compareBeatMaps(const MMM::BeatMap& m1, const MMM::BeatMap& m2)
{
    // 玩家轨道数影响物件位置解释，必须精确保留。
    if ( m1.m_baseMapMetadata.track_count !=
         m2.m_baseMapMetadata.track_count ) {
        XERROR("Track count mismatch: {} vs {}",
               m1.m_baseMapMetadata.track_count,
               m2.m_baseMapMetadata.track_count);
        return false;
    }

    // 原生格式能保存完整 Timing，因此数量、时间、BPM 和效果类型都逐项比较。
    if ( m1.m_timings.size() != m2.m_timings.size() ) {
        XERROR("Timing count mismatch: {} vs {}",
               m1.m_timings.size(),
               m2.m_timings.size());
        return false;
    }
    for ( size_t i = 0; i < m1.m_timings.size(); ++i ) {
        // 原生 MMM 保存 double，使用极小容差只吸收序列化表示误差。
        const auto& t1 = m1.m_timings[i];
        const auto& t2 = m2.m_timings[i];
        if ( std::abs(t1.m_timestamp - t2.m_timestamp) > 1e-3 ||
             std::abs(t1.m_bpm - t2.m_bpm) > 1e-3 ||
             t1.m_timingEffect != t2.m_timingEffect ) {
            XERROR(
                "Timing mismatch at index {}: t1={}, bpm1={} | t2={}, bpm2={}",
                i,
                t1.m_timestamp,
                t1.m_bpm,
                t2.m_timestamp,
                t2.m_bpm);
            return false;
        }
    }

    // 统一物件视图数量可发现类型容器遗漏或折线节点重复同步。
    if ( m1.m_allNotes.size() != m2.m_allNotes.size() ) {
        XERROR("Note count mismatch: {} vs {}",
               m1.m_allNotes.size(),
               m2.m_allNotes.size());
        return false;
    }

    // 当前测试对物件内容采用首项抽样；数量检查仍覆盖整体增减。
    // 抽样只验证基础类型和时间，不替代各格式专用的完整物件区块比较。
    if ( !m1.m_allNotes.empty() ) {
        // 使用值副本避免比较期间受引用容器别名影响。
        auto n1_first = m1.m_allNotes.front().get();
        auto n2_first = m2.m_allNotes.front().get();
        if ( n1_first.m_type != n2_first.m_type ||
             std::abs(n1_first.m_timestamp - n2_first.m_timestamp) > 1e-3 ) {
            XERROR("First note mismatch: type1={}, t1={} | type2={}, t2={}",
                   (int)n1_first.m_type,
                   n1_first.m_timestamp,
                   (int)n2_first.m_type,
                   n2_first.m_timestamp);
            return false;
        }
    }

    // 原生 MMM 必须完整承接来源格式中的自动采样对象。
    if ( m1.m_audioSamples.size() != m2.m_audioSamples.size() ) {
        XERROR("Audio sample count mismatch: {} vs {}",
               m1.m_audioSamples.size(),
               m2.m_audioSamples.size());
        return false;
    }
    // 比较前建立观察指针视图，不复制资源元数据，也不改变 BeatMap 所有权。
    std::vector<const MMM::AudioSampleEvent*> samples1;
    std::vector<const MMM::AudioSampleEvent*> samples2;
    for ( const auto& sample : m1.m_audioSamples ) {
        samples1.push_back(&sample);
    }
    for ( const auto& sample : m2.m_audioSamples ) {
        samples2.push_back(&sample);
    }
    // 自动采样输入顺序可能因来源格式不同而变化，按全部身份字段建立确定顺序。
    auto sortSamples = [](auto& samples) {
        std::sort(
            samples.begin(),
            samples.end(),
            [](const auto* lhs, const auto* rhs) {
                // 时间显著不同时优先按播放位置排序；近似相同则使用离散字段决胜。
                if ( std::abs(lhs->m_timestamp - rhs->m_timestamp) > 1e-6 ) {
                    return lhs->m_timestamp < rhs->m_timestamp;
                }
                if ( lhs->m_track != rhs->m_track ) {
                    return lhs->m_track < rhs->m_track;
                }
                if ( lhs->m_audioResourceId != rhs->m_audioResourceId ) {
                    return lhs->m_audioResourceId < rhs->m_audioResourceId;
                }
                return lhs->m_offsetMs < rhs->m_offsetMs;
            });
    };
    sortSamples(samples1);
    sortSamples(samples2);
    // 两边使用相同全序后，相同索引应代表同一个逻辑自动采样。
    // 排序后逐项比较，无需依赖来源格式的 JSON 数组顺序。
    for ( std::size_t index = 0; index < samples1.size(); ++index ) {
        const auto& lhs = *samples1[index];
        const auto& rhs = *samples2[index];
        if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-3 ||
             lhs.m_offsetMs != rhs.m_offsetMs || lhs.m_track != rhs.m_track ||
             lhs.m_audioResourceId != rhs.m_audioResourceId ||
             std::abs(lhs.m_volume - rhs.m_volume) > 1e-6F ) {
            XERROR("Audio sample mismatch at index {}", index);
            return false;
        }
    }

    return true;
}

/// @brief 执行任意来源格式到原生 MMM 的往返一致性验证。
/// @param argc 需要输入文件和输出文件两个参数。
/// @param argv argv[1] 为来源谱面，argv[2] 为构建目录中的 MMM 输出。
/// @return 加载、保存、重载与逻辑比较全部成功时返回零。
int main(int argc, char* argv[])
{
    if ( argc < 3 ) {
        XERROR("Usage: MapConsistencyTest <input_file> <output_file>");
        return 1;
    }

    fs::path inputPath  = argv[1];
    fs::path outputPath = argv[2];
    // 输出位置应由 CTest 指向构建树，程序本身不重写文件名或父目录。

    XINFO("Testing map consistency: {}", inputPath.string());

    // 基准模型允许纯 Timing、纯物件或纯自动采样，三者全空才视为加载失败。
    MMM::BeatMap map1 = MMM::BeatMap::loadFromFile(inputPath);
    if ( map1.m_timings.empty() && map1.m_allNotes.empty() &&
         map1.m_audioSamples.empty() ) {
        XERROR("Failed to load map1 or map is empty: {}", inputPath.string());
        return 1;
    }

    // 保存失败时不继续读取可能存在的旧输出文件。
    if ( !map1.saveToFile(outputPath) ) {
        XERROR("Failed to save map to: {}", outputPath.string());
        return 1;
    }

    // 重载使用公开入口，确保验证覆盖实际格式分派而非内部辅助函数。
    MMM::BeatMap map2 = MMM::BeatMap::loadFromFile(outputPath);
    // 空谱面判断包含自动采样，允许专门验证只有 BGM 事件的最小输入。
    if ( map2.m_timings.empty() && map2.m_allNotes.empty() &&
         map2.m_audioSamples.empty() ) {
        XERROR("Failed to load map2 (re-imported): {}", outputPath.string());
        return 1;
    }

    // 最后只比较两份内存模型，将 JSON 表示差异与语义回归分离。
    if ( !compareBeatMaps(map1, map2) ) {
        XERROR("Consistency check failed for {}",
               inputPath.filename().string());
        return 1;
    }

    XINFO("Consistency test PASSED for {}", inputPath.filename().string());
    return 0;
}
