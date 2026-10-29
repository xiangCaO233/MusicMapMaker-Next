#include "log/colorful-log.h"
#include "network/CurlHandle.h"
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

namespace
{
/// @brief TLS 探针只验证握手与响应码，不保存服务器内容。
/// @return 消费全部数据，使传输不会因测试回调而提前失败。
std::size_t discardBody(char*, std::size_t size, std::size_t count, void*)
{
    return size * count;
}

/// @brief 通过业务共用句柄工厂执行一次真实 HTTPS 请求。
/// @param expected 可信站点要求成功，无效证书站点要求验证错误。
/// @return 失败类型不符时返回 false，超时或 DNS 失败不能算拒绝证书成功。
/// @warning 只在人工启用的网络验收中阻塞等待握手，不进入普通离线 CTest。
bool checkHttps(const char* url, CURLcode expected)
{
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(
        MMM::Network::createCurlHandle(), curl_easy_cleanup);
    if ( !handle ) return false;
    // 此处不提供 CA 覆盖或验证关闭参数，验证真正来自业务工厂的设置。
    // 探针使用与业务相同的 CA 和验证选项，只增加有界测试超时。
    curl_easy_setopt(handle.get(), CURLOPT_URL, url);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, discardBody);
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, 20L);
    const auto result = curl_easy_perform(handle.get());
    // TLS 握手被拒绝时通常没有 HTTP 状态；不能将 HTTP 错误误认作证书拒绝。
    long response = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response);
    XINFO("TLS probe: {} result={} HTTP={}",
          url,
          static_cast<int>(result),
          response);
    // 成功必须取得预期 HTTP 响应；无效站点必须明确因证书验证失败而拒绝。
    return result == expected && (expected != CURLE_OK || response == 200);
}
}  // namespace

/// @brief 默认离线检查初始化边界；显式参数启用设备端真实 TLS 回归。
/// @param argv 可选 PEM 文件、可信 HTTPS URL、一个或多个无效证书 HTTPS URL。
/// @return 任一信任边界或 TLS 结果不符合预期则失败。
/// @details 网络模式由人工设备验收调用，CTest 默认不依赖公网服务。
/// Android 离线用例覆盖缺失平台初始化，桌面用例覆盖保留原生后端默认配置。
/// 网络参数可分别提供自签名、过期和主机名不符样例，要求逐项拒绝。
/// @warning 网络模式只由显式命令启用，禁止在每帧交互中执行此探针。
int main(int argc, char** argv)
{
    using namespace MMM::Network;
    // 空集合不应发布为有效信任来源，后续真正的初始化必须仍可执行。
    if ( initializeCurlTrust({}) ) return 1;
    if ( argc == 1 ) {
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(
            createCurlHandle(), curl_easy_cleanup);
#ifdef __ANDROID__
        // Android 在未接入平台证书前必须拒绝创建可用请求。
        return handle ? 1 : 0;
#else
        // 桌面不要求 Android 初始化，原有平台默认信任配置保持可用。
        return handle ? 0 : 1;
#endif
    }
    if ( argc < 4 ) return 2;
    // 显式网络模式读取受控测试文件，不依赖主机 SDK 的证书或用户全局环境变量。
    std::ifstream input(argv[1], std::ios::binary);
    if ( !input ) return 2;
    // 输入完全读取后才发布，读取失败或空文件不能变成无约束的信任集合。
    std::string certificates((std::istreambuf_iterator<char>(input)), {});
    if ( !initializeCurlTrust(std::move(certificates)) ) return 1;
    // 已发布的信任锚不可被第二次调用替换，避免请求间产生不同安全策略。
    if ( initializeCurlTrust("replacement must be rejected") ) return 1;
    // 先证明同一套信任锚能访问正常 HTTPS，避免所有连接都失败时产生假阳性。
    if ( !checkHttps(argv[2], CURLE_OK) ) return 1;
    for ( int index = 3; index < argc; ++index ) {
        // 每个无效站点使用独立句柄，禁止复用已成功握手的 TLS 会话掩盖错误。
        if ( !checkHttps(argv[index], CURLE_PEER_FAILED_VERIFICATION) )
            return 1;
    }
    return 0;
}
