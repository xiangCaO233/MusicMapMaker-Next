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
bool testConcurrentTranslateAndClear()
{
    constexpr std::size_t KEY_COUNT    = 512;
    constexpr std::size_t WORKER_COUNT = 8;
    constexpr std::size_t READ_ROUNDS  = 12000;
    constexpr std::size_t CLEAR_ROUNDS = 2000;

    std::vector<std::string> fallbacks;
    std::vector<uint64_t>    hashes;
    fallbacks.reserve(KEY_COUNT);
    hashes.reserve(KEY_COUNT);
    for ( std::size_t index = 0; index < KEY_COUNT; ++index ) {
        fallbacks.push_back("translation.concurrent." + std::to_string(index));
        hashes.push_back(MMM::Hash::hashString(fallbacks.back()));
    }

    MMM::Translation::Translator translator;
    const char*                  stableBeforeClear =
        translator.translate(hashes.front(), fallbacks.front().c_str()).data();
    const auto initialStats = translator.getCacheStats();
    if ( initialStats.pointerCacheEntryCount != 1 ||
         initialStats.stableStringCount != 1 ||
         initialStats.stableStringBytes != fallbacks.front().size() ) {
        XERROR("Translation cache statistics did not record initial fallback");
        return false;
    }

    std::atomic<bool>        start{ false };
    std::atomic<bool>        failed{ false };
    std::vector<std::thread> workers;
    workers.reserve(WORKER_COUNT + 1);
    for ( std::size_t worker = 0; worker < WORKER_COUNT; ++worker ) {
        workers.emplace_back([&, worker] {
            while ( !start.load(std::memory_order_acquire) ) {
                std::this_thread::yield();
            }
            for ( std::size_t round = 0; round < READ_ROUNDS; ++round ) {
                const std::size_t index =
                    (round * 37U + worker * 53U) % KEY_COUNT;
                const auto translated = translator.translate(
                    hashes[index], fallbacks[index].c_str());
                if ( translated.view() != fallbacks[index] ) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }
    workers.emplace_back([&] {
        while ( !start.load(std::memory_order_acquire) ) {
            std::this_thread::yield();
        }
        for ( std::size_t round = 0; round < CLEAR_ROUNDS; ++round ) {
            translator.clear();
        }
    });

    start.store(true, std::memory_order_release);
    for ( auto& worker : workers ) {
        worker.join();
    }

    const auto populatedStats = translator.getCacheStats();
    translator.clear();
    const auto clearedStats = translator.getCacheStats();
    if ( failed.load(std::memory_order_acquire) ||
         std::strcmp(stableBeforeClear, fallbacks.front().c_str()) != 0 ) {
        XERROR("Concurrent translation invalidated a cached string pointer");
        return false;
    }
    if ( populatedStats.stableStringCount != KEY_COUNT ||
         clearedStats.pointerCacheEntryCount != 0 ||
         clearedStats.stableStringCount != populatedStats.stableStringCount ||
         clearedStats.stableStringBytes != populatedStats.stableStringBytes ) {
        XERROR("Translation stable pool statistics changed unexpectedly");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行翻译缓存并发回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    static_assert(MMM::Hash::hashString("hello") == 0xA430D84680AABD0BULL);
    static_assert(
        !std::is_convertible_v<MMM::Translation::TRResult, std::string>);
    static_assert(
        !std::is_convertible_v<MMM::Translation::TRResult, const char*>);
    return testConcurrentTranslateAndClear() ? 0 : 1;
}
