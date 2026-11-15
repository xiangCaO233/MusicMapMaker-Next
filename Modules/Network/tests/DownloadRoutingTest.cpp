#include "network/DownloadRouting.h"
#include "log/colorful-log.h"
#include "network/AssetSyncService.h"
#include "network/CurlHandle.h"
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace
{
/**
 * @brief 在测试日志中保留失败条件，不依赖被 NDEBUG 禁用的 assert。
 * @param value 当前行为是否满足断言。
 * @param message 在错误日志中定位失败场景。
 * @return 传回原条件，调用方累计错误数而不是依赖 assert。
 * @details 测试可在发布配置执行，不能被 NDEBUG 静默禁用。
 */
bool expect(bool value, const char* message)
{
    if ( !value ) XERROR("DownloadRoutingTest: {}", message);
    return value;
}

/**
 * @brief 测试始终使用宽路径打开 Windows 文件，避免路径编码掩盖下载错误。
 * @param path 构建树中的隔离输出文件。
 * @return 可写二进制句柄，由当前测试关闭。
 * @details Windows 使用原生宽路径，避免 Unicode 目录导致与网络无关的失败。
 * 测试文件不进入源码资源目录，也不依赖用户真实下载文件。
 */
FILE* openOutput(const std::filesystem::path& path)
{
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

/**
 * @brief 把本地绝对路径编码成 file URL，空格和非 ASCII 字节不能影响离线用例。
 * @param path 已在构建树创建的绝对源路径。
 * @return 保留盘符和路径分隔符的 file URI。
 * @details 非 ASCII 字节按 UTF-8 百分号编码，避免把测试限制在纯英文目录。
 * 协议是否可用仍由当前 curl 构建决定，测试不能擅自启用被禁用的协议。
 */
std::string fileUrl(const std::filesystem::path& path)
{
    const auto  utf8  = path.generic_u8string();
    std::string value = "file://";
    if ( utf8.empty() || utf8.front() != u8'/' ) value += '/';
    constexpr char HEX[] = "0123456789ABCDEF";
    for ( auto raw : utf8 ) {
        const auto c = static_cast<unsigned char>(raw);
        // 仅路径结构和 URI 非保留字符可以原样传递给 curl。
        if ( (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '-' ||
             c == '_' || c == '.' )
            value += static_cast<char>(c);
        else {
            value += '%';
            value += HEX[c >> 4];
            value += HEX[c & 15];
        }
    }
    return value;
}

/**
 * @brief 为本地 TLS 夹具和人工公网验收提供真实生产下载器入口。
 * @details CA 与 CONNECT_TO 只属于测试 executable 的显式参数，不进入应用配置。
 * 这样既能验证 Range 错误与中断恢复，也无需降低生产 TLS 校验。
 * @param argc 测试驱动传入的完整参数数量。
 * @param argv 依次包含 URL、目标、预期大小、CA、连接映射、网卡和候选地址。
 * @return 只有传输完整且文件关闭成功才返回零。
 * @details 该入口只存在于测试套件，不加入编辑器的运行时配置。
 * 本地夹具显式信任测试 CA，同时继续验证官方主机名与 SNI。
 * CONNECT_TO 让请求只进入回环测试端口，不访问候选的真实公网服务。
 * 公网验收则不传连接映射，使用同一生产下载函数验证完整 SHA256。
 * 调用者通过实际文件、请求序列和退出码共同判定，不能只看日志成功。
 */
int fetch(int argc, char** argv)
{
    // 参数不足时不创建文件，不允许夹具无意落到正式网络或默认路径。
    if ( argc < 8 ) return 2;
    auto* curl = MMM::Network::createCurlHandle();
    FILE* file = openOutput(argv[3]);
    if ( !curl || !file ) {
        if ( curl ) curl_easy_cleanup(curl);
        if ( file ) std::fclose(file);
        return 3;
    }
    MMM::Network::DownloadRoutingOptions options;
    const std::string_view               size(argv[4]);
    const auto                           parsed = std::from_chars(
        size.data(), size.data() + size.size(), options.m_expectedSize);
    if ( parsed.ec != std::errc{} || parsed.ptr != size.data() + size.size() ) {
        curl_easy_cleanup(curl);
        std::fclose(file);
        return 2;
    }
    options.m_timeoutSeconds = 40;
    // 故障夹具绝不回退到真实官网；所有恢复都应由本地请求记录证明。
    options.m_allowOriginFallback = false;
    // 夹具的选线参数只存在测试进程，实际 UI 调用相同的会话 setter。
    // 测试环境只影响本次进程，不能触碰用户偏好或系统网络设置。
    // 每个 TLS 场景独立启动，因此无序运行也不会继承另一个场景的选择。
    // 暖缓存例外在同一进程执行两次，仍保留真实地址缓存的生命周期。
    const char*            modeValue = std::getenv("MMM_TEST_DOWNLOAD_ROUTE");
    const std::string_view mode      = modeValue ? modeValue : "";
    using Route                      = MMM::Network::DownloadRoute;
    MMM::Network::setDownloadRoute((mode == "origin" || mode == "switch-r2")
                                       ? Route::Origin
                                       : (mode == "r2" || mode == "r2-failure"
                                              ? Route::R2
                                              : Route::Automatic));
    // 故障回退场景也映射源站到同一回环服务，不能向公网发送恢复请求。
    if ( mode == "r2-failure" || mode == "auto-fallback" )
        options.m_allowOriginFallback = true;
    // 动态切换只由业务进度产生，不把 setter 放进探测的内部回调。
    // 即便一次正文很快结束，返回前的选线复核也应保证新选择得到执行。
    // 回调不关闭文件、销毁 CURL 或重入下载器，防止并发写入同一输出。
    if ( mode == "switch-origin" || mode == "switch-r2" ) {
        // 只在正式正文已经写入后换路，探测阶段不会调用业务进度回调。
        // 服务端请求序列与最终摘要共同证明当前文件安全截断后重下。
        const Route target = mode == "switch-r2" ? Route::R2 : Route::Origin;
        options.m_progress = [target](std::int64_t bytes, std::int64_t) {
            if ( bytes > 1024 * 1024 ) MMM::Network::setDownloadRoute(target);
        };
    }
    // 测试夹具只应访问回环端口，不能因恢复失败而访问真实官网。
    // 公网验收也要求显式关闭镜像，以免把镜像速度误认为 IP 优选收益。
    for ( int i = 8; i < argc; ++i ) options.m_addresses.emplace_back(argv[i]);
    curl_easy_setopt(curl, CURLOPT_URL, argv[2]);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    // 映射只改变连接目的地，不改变 TLS 中的原始主机名。
    // CA 参数是测试夹具证书，不应关闭证书或主机名验证来让测试通过。
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if ( std::string_view(argv[5]) != "-" )
        curl_easy_setopt(curl, CURLOPT_CAINFO, argv[5]);
    curl_slist* connect = nullptr;
    // 本地夹具仍以官方主机名做 SNI 和证书验证，仅连接到测试进程自己的端口。
    if ( std::string_view(argv[6]) != "-" ) {
        connect = curl_slist_append(nullptr, argv[6]);
        // 源站测试继续保留各自真实主机名与 SAN，只有目的端口相同。
        // 两个域名都有独立 TLS 主机名，夹具证书明确同时列出这两个 SAN。
        // 不能使用忽略证书主机名的方法让手动直连测试通过。
        // 目的端口来自测试服务器，原 URL 路径和查询仍由生产函数构造。
        const std::string primaryMapping(argv[6]);
        const auto        colon = primaryMapping.find(':');
        const auto        originMapping =
            "mmm.xiang233.top" + primaryMapping.substr(colon);
        connect = curl_slist_append(connect, originMapping.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_TO, connect);
    }
    if ( std::string_view(argv[7]) != "-" )
        curl_easy_setopt(curl, CURLOPT_INTERFACE, argv[7]);
    const auto result =
        MMM::Network::performRoutedDownload(curl, argv[2], file, options);
    curl_easy_cleanup(curl);
    curl_slist_free_all(connect);
    const auto closed = std::fclose(file);
    // 调用方继续检查完整摘要；这里不把一次成功 perform 当作文件身份正确。
    XINFO("DownloadRoutingTest result={} sha256={}",
          static_cast<int>(result),
          MMM::Network::AssetSyncService::sha256File(argv[3]));
    return result == CURLE_OK && closed == 0 ? 0 : 1;
}
}  // namespace

/**
 * @brief 默认用例完全离线；真实 HTTPS 故障场景由 Python 的本地 TLS 夹具驱动。
 * @param argc 套件入口已恢复为当前用例自己的参数布局。
 * @param argv 默认模式只接收构建树输出目录，--fetch 为受控 TLS 驱动入口。
 * @return 任一断言失败时非零，不吞掉传输或文件系统错误。
 * @details 默认用例覆盖主机授权边界和取消传播，不要求外网可用。
 * 本地 HTTPS 场景由独立脚本在每个场景启动新进程，隔离全局选路缓存。
 * curl_global_init 必须先于 Winsock
 * 解析或网络句柄创建，清理在所有句柄关闭后进行。
 * 生成文件仅位于显式测试输出目录，不能修改 tests/data 下的资源。
 */
int main(int argc, char** argv)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if ( argc > 1 && (std::string_view(argv[1]) == "--fetch" ||
                      std::string_view(argv[1]) == "--fetch-twice") ) {
        int result = fetch(argc, argv);
        // 第二次使用新句柄和同一进程缓存，验证复用不会再次发送探针。
        // 仍从空目标文件下载完整内容，不能借用第一次的输出蒙混通过。
        if ( result == 0 && std::string_view(argv[1]) == "--fetch-twice" )
            result = fetch(argc, argv);
        curl_global_cleanup();
        return result;
    }
    if ( argc < 2 ) return 2;
    namespace Network = MMM::Network;
    int errors        = 0;
    // 三种会话选项有确定默认值，非法值不能流入下拉框索引。
    // setter 是 UI 与下载线程唯一的会话意图入口，测试读取同一个状态。
    // 非法枚举回退后继续正常 URL 验证，不能因防御分支留下失效状态。
    // URL 断言包含摘要查询，避免只验证主机名而丢掉发布身份。
    using Route = Network::DownloadRoute;
    errors += !expect(Network::getDownloadRoute() == Route::Automatic,
                      "default route must be automatic");
    Network::setDownloadRoute(Route::Origin);
    errors += !expect(Network::getDownloadRoute() == Route::Origin,
                      "manual choice must reach network layer");
    Network::setDownloadRoute(static_cast<Route>(255));
    errors += !expect(Network::getDownloadRoute() == Route::Automatic,
                      "invalid route must fall back safely");
    const std::string official =
        "https://downloads.xiang233.top/download/hotfix/app.apk?sha256=abc";
    // 只换官方下载主机，路径、版本和摘要身份不能随手动选源改变。
    errors += !expect(
        Network::downloadUrlForRoute(official, Route::Origin) ==
            "https://mmm.xiang233.top/download/hotfix/app.apk?sha256=abc",
        "origin must retain exact object path and checksum");
    errors +=
        !expect(Network::downloadUrlForRoute(official, Route::R2) == official,
                "manual R2 must retain URL");
    errors += !expect(Network::downloadUrlForRoute(
                          "https://other.invalid/download/a", Route::Origin) ==
                          "https://other.invalid/download/a",
                      "unrelated host must never become official mirror");
    errors += !expect(
        Network::isR2DownloadUrl(
            "https://downloads.xiang233.top/download/assets.zip?sha256=abc"),
        "official URL must be eligible");
    // 精确授权边界不能被后缀域名、用户信息、明文协议或不同端口绕过。
    for ( const char* url :
          { "http://downloads.xiang233.top/download/a",
            "https://downloads.xiang233.top.evil/download/a",
            "https://downloads.xiang233.top@evil/download/a",
            "https://downloads.xiang233.top:444/download/a",
            "https://downloads.xiang233.top/api/a",
            "https://downloads.xiang233.top/download/../private",
            "https://downloads.xiang233.top/download/%2e%2e/private",
            "https://downloads.xiang233.top/download/a\r\nX: y" } )
        errors += !expect(!Network::isR2DownloadUrl(url),
                          "unrelated URL must retain normal transport");
    const std::filesystem::path root(argv[1]);
    std::error_code             error;
    std::filesystem::create_directories(root, error);
    if ( error ) return 3;
    // 源文件包含足够正文，不能用两个空文件的相同摘要掩盖没有下载。
    // 文件名含空格，避免 URI 编码问题被纯英文紧凑路径掩盖。
    const auto source = root / "source with space.bin",
               output = root / "result.bin";
    {
        std::ofstream stream(source, std::ios::binary);
        stream << std::string(65536, 'x');
    }
    auto* curl = Network::createCurlHandle();
    FILE* file = openOutput(output);
    if ( !curl || !file ) return 3;
    curl_easy_setopt(curl, CURLOPT_URL, fileUrl(source).c_str());
    Network::DownloadRoutingOptions options;
    // 用明确总长验证完整写入，不能把短内容误认为成功。
    options.m_expectedSize = 65536;
    // file URL 不触发地址探测或镜像，成功后仍应保留原有二进制内容。
    auto result =
        Network::performRoutedDownload(curl, fileUrl(source), file, options);
    std::fclose(file);
    // 发布预编译 curl 可只启用 HTTP/HTTPS；测试不能为 file URL
    // 擅自放宽协议构建。
    bool supportsFile = false;
    // 协议能力来自实际编译依赖，测试不能为 FILE 擅自修改发布配置。
    // 如果声明支持 FILE，必须真正复制成功，不能把能力检查当作跳过断言。
    const auto* version = curl_version_info(CURLVERSION_NOW);
    for ( const char* const* protocol = version->protocols;
          protocol && *protocol;
          ++protocol )
        if ( std::string_view(*protocol) == "file" ) supportsFile = true;
    errors += !expect(
        supportsFile ? result == CURLE_OK &&
                           Network::AssetSyncService::sha256File(source) ==
                               Network::AssetSyncService::sha256File(output)
                     : result == CURLE_UNSUPPORTED_PROTOCOL,
        "non-R2 transport must retain curl protocol support and errors");
    file                = openOutput(output);
    options.m_cancelled = [] { return true; };
    // 使用确定的已取消状态，不依赖某次进度回调的时间或次数。
    // 目标先截断，确认没有错误复用前一次测试的成功结果。
    // 已取消任务不能改换 IP 或镜像后继续下载。
    result =
        Network::performRoutedDownload(curl, fileUrl(source), file, options);
    std::fclose(file);
    errors += !expect(result == CURLE_ABORTED_BY_CALLBACK &&
                          std::filesystem::file_size(output, error) == 0,
                      "cancellation must stop before transfer");
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    // 失败累计为进程退出码，由 CTest 检查，成功日志不能代替结果。
    return errors == 0 ? 0 : 1;
}
