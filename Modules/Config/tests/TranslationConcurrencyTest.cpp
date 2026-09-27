#include "config/skin/translation/Translation.h"

#include "log/colorful-log.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{

/// @brief 验证 UI 与逻辑线程并发翻译及语言清空不会破坏缓存迭代器。
/// @return 所有线程均得到稳定回退文本时返回 true。
/// @warning 压力测试会创建九个线程并执行大量让步，不属于性能基准。
bool testConcurrentTranslateAndClear()
{
    // 键数量足以迫使缓存扩容，验证稳定字符串不依赖容器桶地址。
    constexpr std::size_t KEY_COUNT = 512;
    // 多个读取线程模拟 UI、逻辑和后台服务同时查询翻译。
    constexpr std::size_t WORKER_COUNT = 8;
    // 读取轮次远高于键数量，使每个键在并发清理期间反复命中。
    constexpr std::size_t READ_ROUNDS = 12000;
    // 清理线程连续重建指针缓存，放大迭代器和悬空指针竞态。
    constexpr std::size_t CLEAR_ROUNDS = 2000;

    // fallback 字符串在所有线程结束前保持地址稳定，作为预期文本来源。
    std::vector<std::string> fallbacks;
    // 哈希预先计算，压力循环只覆盖 Translator 的并发路径。
    std::vector<uint64_t> hashes;
    fallbacks.reserve(KEY_COUNT);
    hashes.reserve(KEY_COUNT);
    for ( std::size_t index = 0; index < KEY_COUNT; ++index ) {
        // 每个键文本唯一，错误命中其他缓存项会被字符串比较发现。
        fallbacks.push_back("translation.concurrent." + std::to_string(index));
        hashes.push_back(MMM::Hash::hashString(fallbacks.back()));
    }

    // 独立 Translator 避免全局语言状态和其他测试缓存影响统计值。
    MMM::Translation::Translator translator;
    // 保存首次返回的裸字符指针，专门验证 clear 后稳定池仍保持其生命周期。
    const char* stableBeforeClear =
        translator.translate(hashes.front(), fallbacks.front().c_str()).data();
    const auto initialStats = translator.getCacheStats();
    // 首次缺失翻译应同时创建一个指针缓存项和一个稳定字符串。
    if ( initialStats.pointerCacheEntryCount != 1 ||
         initialStats.stableStringCount != 1 ||
         initialStats.stableStringBytes != fallbacks.front().size() ) {
        // 初始统计失败表示夹具前提不成立，不继续启动并发线程。
        XERROR("Translation cache statistics did not record initial fallback");
        return false;
    }

    // start 构成统一起跑屏障，failed 允许任一读取线程提前报告内容错误。
    std::atomic<bool>        start{ false };
    std::atomic<bool>        failed{ false };
    std::vector<std::thread> workers;
    // 额外一个槽位留给与所有读取者并发的 clear 线程。
    workers.reserve(WORKER_COUNT + 1);
    for ( std::size_t worker = 0; worker < WORKER_COUNT; ++worker ) {
        workers.emplace_back([&, worker] {
            // acquire 与主线程 release 配对，所有测试夹具在开始前已完全构造。
            while ( !start.load(std::memory_order_acquire) ) {
                // 测试屏障只短暂让出时间片，不用固定休眠改变竞争形态。
                std::this_thread::yield();
            }
            for ( std::size_t round = 0; round < READ_ROUNDS; ++round ) {
                // 互质步长让不同工作线程以不同顺序覆盖完整键空间。
                const std::size_t index =
                    (round * 37U + worker * 53U) % KEY_COUNT;
                const auto translated = translator.translate(
                    hashes[index], fallbacks[index].c_str());
                // 没有加载语言时结果必须始终等于对应
                // fallback，而非其他缓存文本。
                if ( translated.view() != fallbacks[index] ) {
                    // release 让主线程 join
                    // 后可靠观察失败，随后当前线程立即结束。
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }
    // 清理线程只重置可重建缓存，稳定字符串池按 API 契约必须继续存在。
    workers.emplace_back([&] {
        while ( !start.load(std::memory_order_acquire) ) {
            std::this_thread::yield();
        }
        for ( std::size_t round = 0; round < CLEAR_ROUNDS; ++round ) {
            // 重复 clear 与读取交错，覆盖空缓存和刚填充缓存两种状态。
            translator.clear();
        }
    });

    // 所有线程创建完成后一次发布起跑信号，最大化真实并发重叠。
    start.store(true, std::memory_order_release);
    for ( auto& worker : workers ) {
        // join 是测试收尾同步点，之后可以无锁读取线程写入的原子结果。
        worker.join();
    }
    // 所有 join 完成后 clear 线程也已停止，不再与统计快照读取并发。

    // 并发阶段结束后先记录已填充统计，再显式清理一次观察两类缓存差异。
    const auto populatedStats = translator.getCacheStats();
    // 最后一次 clear 明确建立断言所需的空指针缓存状态。
    translator.clear();
    const auto clearedStats = translator.getCacheStats();
    // 首次指针必须仍指向原 fallback 内容，任何稳定池释放都会在此暴露。
    if ( failed.load(std::memory_order_acquire) ||
         std::strcmp(stableBeforeClear, fallbacks.front().c_str()) != 0 ) {
        // 同时覆盖内容错误和早期返回指针失效，两者都属于生命周期回归。
        XERROR("Concurrent translation invalidated a cached string pointer");
        return false;
    }
    // 所有键都应进入稳定池，而 clear 只清空可失效的指针查找缓存。
    if ( populatedStats.stableStringCount != KEY_COUNT ||
         clearedStats.pointerCacheEntryCount != 0 ||
         clearedStats.stableStringCount != populatedStats.stableStringCount ||
         clearedStats.stableStringBytes != populatedStats.stableStringBytes ) {
        // 统计关系失败说明 clear 错误影响了稳定池或未清空可重建缓存。
        XERROR("Translation stable pool statistics changed unexpectedly");
        return false;
    }
    // 字节统计保持一致进一步证明 clear 未替换或收缩稳定字符串存储。
    return true;
}

}  // namespace

/// @brief 运行翻译缓存并发回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    // 编译期固定哈希值，防止算法变化让持久化翻译键失去兼容性。
    static_assert(MMM::Hash::hashString("hello") == 0xA430D84680AABD0BULL);
    // TRResult 不得隐式分配 std::string，避免热路径意外复制翻译文本。
    static_assert(
        !std::is_convertible_v<MMM::Translation::TRResult, std::string>);
    // 同样禁止退化为无生命周期信息的裸指针隐式转换。
    static_assert(
        !std::is_convertible_v<MMM::Translation::TRResult, const char*>);
    // 运行期压力断言失败时统一返回非零，详细原因由测试内部日志给出。
    return testConcurrentTranslateAndClear() ? 0 : 1;
}
