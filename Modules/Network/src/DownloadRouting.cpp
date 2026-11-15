#include "network/DownloadRouting.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

#ifdef _WIN32
#    include <io.h>
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <netdb.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace MMM::Network
{
namespace
{
/// @brief 测速只取小前缀；选择结果短期复用，不形成永久、全用户共用的 IP 排名。
constexpr std::size_t      PROBE_BYTES = 1024 * 1024;
constexpr std::string_view PREFIX = "https://downloads.xiang233.top/download/";
/// @brief UI 写入、后台回调读取的会话选线，不承载其他状态的可见性。
/// @warning UI 每帧只读、用户操作时写；后台回调 relaxed
/// 读取以取消旧线路，避免跨线程长锁。
std::atomic<DownloadRoute> selectedRoute{ DownloadRoute::Automatic };
/// @brief 单一官方主机的进程内缓存；只有后台下载线程访问这把短临界区锁。
std::mutex routeMutex;
/// @brief 缓存不持有句柄、回调或用户网络标识，失效和慢速后重新测量。
std::string cachedAddress;
/// @brief 使用单调时间，不受系统时钟校正影响。
std::chrono::steady_clock::time_point cachedUntil;

/// @brief 请求内的非拥有状态，生命周期覆盖对应 easy handle 的 perform。
struct Transfer {
    /// @brief 本次请求对应的线路快照，变化后终止当前正文而不是混用两条路径。
    DownloadRoute m_route{ DownloadRoute::Automatic };
    /// @brief 只有官方文件响应会话选线，第三方请求不应被无关 UI 操作取消。
    bool m_monitorRoute{};
    /// @brief 调用方在同步期间保持有效。
    const DownloadRoutingOptions* m_options{};
    /// @brief 实际落盘目标；探测时为空。
    FILE* m_file{};
    /// @brief 探测前缀有固定上限，不保存完整资源包。
    std::vector<char> m_bytes;
    /// @brief 当前请求开始前已保留的文件字节数。
    std::uint64_t m_offset{};
    /// @brief 当前请求真正落盘的字节数。
    std::uint64_t m_written{};
    /// @brief 最后一个响应头块声明的总长。
    std::uint64_t m_total{};
    std::uint64_t m_rangeStart{ std::numeric_limits<std::uint64_t>::max() };
    /// @brief 重定向响应体不能写入最终文件。
    long m_status{};
    /// @brief 探测只接受从零开始的部分响应。
    bool m_probe{};
    /// @brief 服务器忽略 Range 时必须截断重来，不能盲目追加。
    bool m_rejected{};
    /// @brief 强 ETag 用于 If-Range；弱 ETag 不用于续传。
    std::string m_etag;
    /// @brief 续传必须保持已保留数据的强实体标识。
    std::string m_expectedTag;
    /// @brief 续传前观察到的完整总长。
    std::uint64_t m_expectedTotal{};
};

/**
 * @brief 解析严格的非负十进制整数，拒绝尾随字符和溢出。
 * @param text 已去除空白的十进制字段，不接受符号或未知总长星号。
 * @param result 成功时接收数值，失败时不能使用中间解析结果。
 * @return 整个字段被消费且无溢出时才返回成功。
 * @details 使用整数解析保留大文件偏移精度，也不依赖 macOS 浮点解析 API。
 */
bool number(std::string_view text, std::uint64_t& result)
{
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), result);
    return !text.empty() && parsed.ec == std::errc{} &&
           parsed.ptr == text.data() + text.size();
}

/**
 * @brief 丢弃头部两侧的 ASCII 空白，视图不跨越当前回调生命周期。
 * @param value 当前头部值，不包含字段名和冒号。
 * @return 指向原缓冲的只读子视图，不分配副本。
 * @details 不对实体标识做 Unicode 归一化，ETag 字节必须原样保留。
 */
std::string_view trim(std::string_view value)
{
    while ( !value.empty() && (value.front() == ' ' || value.front() == '\t') )
        value.remove_prefix(1);
    while ( !value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                               value.back() == ' ' || value.back() == '\t') )
        value.remove_suffix(1);
    return value;
}

/**
 * @brief 每个 HTTP 响应头块重新建模，避免把重定向或 100 响应的信息带入正文。
 * @param data curl 提供的显式长度字节，未承诺以零结尾。
 * @param context 当前同步请求的 Transfer，不接管其所有权。
 * @return 已消费字节数；非法字段不会被标记为可安全续传。
 * @details Content-Length 仅描述 200 完整响应，不能覆盖 206 的对象总长。
 * Content-Range 同时验证闭区间和总长，拒绝星号及越界区间。
 * 100 或重定向之后重新建模，不能混用不同响应块的元数据。
 * 强 ETag 才可用于 If-Range，弱标识没有逐字节复用契约。
 * 此前保留数据的身份位于 expected 字段，不能被新响应覆盖。
 */
std::size_t header(char* data, std::size_t size, std::size_t count,
                   void* context)
{
    auto&                  state  = *static_cast<Transfer*>(context);
    const std::size_t      length = size * count;
    const std::string_view line(data, length);
    if ( line.starts_with("HTTP/") ) {
        const auto    space = line.find(' ');
        std::uint64_t code  = 0;
        state.m_status =
            space != line.npos && number(line.substr(space + 1, 3), code)
                ? static_cast<long>(code)
                : 0;
        state.m_rangeStart = std::numeric_limits<std::uint64_t>::max();
        state.m_total      = 0;
        state.m_etag.clear();
        return length;
    }
    const auto colon = line.find(':');
    if ( colon == line.npos ) return length;
    std::string key(line.substr(0, colon));
    for ( char& c : key )
        if ( c >= 'A' && c <= 'Z' ) c += 'a' - 'A';
    const auto value = trim(line.substr(colon + 1));
    if ( key == "content-range" && value.starts_with("bytes ") ) {
        const auto    dash = value.find('-', 6), slash = value.find('/');
        std::uint64_t first{}, last{}, total{};
        // 只有闭区间和完整总长都合法时，正文才有资格追加到现有文件。
        if ( dash != value.npos && slash != value.npos && dash < slash &&
             number(value.substr(6, dash - 6), first) &&
             number(value.substr(dash + 1, slash - dash - 1), last) &&
             number(value.substr(slash + 1), total) && first <= last &&
             last < total ) {
            state.m_rangeStart = first;
            state.m_total      = total;
        }
    } else if ( key == "content-length" && state.m_status == 200 ) {
        number(value, state.m_total);
    } else if ( key == "etag" && value.starts_with('"') &&
                value.ends_with('"') && value.size() <= 256 ) {
        state.m_etag.assign(value);
    }
    return length;
}

/**
 * @brief 在正文进入文件前验证续传响应；错误页和完整 200
 * 响应不能追加到部分文件。
 * @param context 已接收最后一个头块的请求状态。
 * @return 实际接收字节数；零要求 curl 停止当前请求。
 * @details 探测只写有限前缀内存，正式阶段始终流式落盘。
 * 偏移、完整总长和实体标识必须同时满足原有续传约束。
 * 错误页与重定向正文不能进入最终制品。
 * written 采用 fwrite 的真实结果，磁盘短写不能被网络重试掩盖。
 * @warning 此函数不计算最终 SHA256，资源或更新调用方必须继续验证。
 */
std::size_t write(char* data, std::size_t size, std::size_t count,
                  void* context)
{
    auto&      state  = *static_cast<Transfer*>(context);
    const auto length = size * count;
    if ( state.m_status >= 300 && state.m_status < 400 ) return length;
    if ( state.m_probe || state.m_offset != 0 ) {
        if ( state.m_status != 206 || state.m_rangeStart != state.m_offset ||
             (state.m_offset != 0 && (!state.m_expectedTag.empty() &&
                                      state.m_etag != state.m_expectedTag)) ||
             (state.m_expectedTotal != 0 &&
              state.m_total != state.m_expectedTotal) ||
             (state.m_options->m_expectedSize != 0 &&
              state.m_total != state.m_options->m_expectedSize) ) {
            state.m_rejected = true;
            return 0;
        }
    }
    if ( state.m_probe ) {
        if ( length > PROBE_BYTES - state.m_bytes.size() ) return 0;
        state.m_bytes.insert(state.m_bytes.end(), data, data + length);
        return length;
    }
    const auto written = std::fwrite(data, 1, length, state.m_file);
    state.m_written += written;
    return written;
}

/**
 * @brief 探测期间只检查取消，正式传输发布包括已保留前缀的连续进度。
 * @param context 对应当前 perform 的非拥有请求状态。
 * @return 非零表示用户取消，取消不能变成尝试下一镜像的理由。
 * @details 探测流量不能计入正式文件进度，只有保留前缀被复用后才计入。
 * 总量优先使用完整对象头，不能把分片剩余大小展示为整包大小。
 * 从零重下时允许进度归零，不能为视觉单调而伪报已落盘字节。
 * 回调只借用 options，同步返回后不保存调用方地址。
 */
int progress(void* context, curl_off_t total, curl_off_t now, curl_off_t,
             curl_off_t)
{
    auto& state = *static_cast<Transfer*>(context);
    if ( state.m_options->m_cancelled && state.m_options->m_cancelled() )
        return 1;
    // 切换只是终止当前请求，外层在原任务截止时间内重新下载当前文件。
    // 不把它发布成用户取消或错误，也不等待渲染线程确认。
    if ( state.m_monitorRoute && getDownloadRoute() != state.m_route ) return 1;
    if ( !state.m_probe && state.m_options->m_progress ) {
        const auto full = state.m_total
                              ? state.m_total
                              : (state.m_options->m_expectedSize
                                     ? state.m_options->m_expectedSize
                                     : static_cast<std::uint64_t>(
                                           std::max<curl_off_t>(0, total)) +
                                           state.m_offset);
        state.m_options->m_progress(
            static_cast<std::int64_t>(state.m_offset +
                                      std::max<curl_off_t>(0, now)),
            static_cast<std::int64_t>(full));
    }
    return 0;
}

/**
 * @brief 为探测和正式请求安装同一套正文边界与取消契约。
 * @param handle 本任务独占的 easy handle，不支持外部同时执行。
 * @param state 至少保持到本次 perform 返回的栈上状态。
 * @details clone 继承信任锚、代理和 User-Agent，但必须覆盖原有数据回调。
 * 否则测速会写入调用方文件，或把探测字节发布成更新进度。
 * 此入口不修改协议或证书验证选项，选路不能改变信任边界。
 */
void callbacks(CURL* handle, Transfer& state)
{
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
}

/**
 * @brief 在栈上请求状态离开作用域前撤销句柄对回调上下文的借用。
 * @param handle 已结束 perform、仍由调用方负责销毁的 easy handle。
 * @details 部分协议在 cleanup 的关闭握手中仍可能调用头部或进度回调。
 * 原调用方不会持有 Transfer，因而不能把上下文留到其 cleanup 时再处理。
 * 清理只重置数据入口，不清空 TLS 信任、响应统计或可供诊断的连接信息。
 * 无状态丢弃写入避免复用期间意外落到 curl 的默认标准输出行为。
 * 新一次传输会重新安装自己的回调，不能沿用此前任务的文件或取消状态。
 */
void detachCallbacks(CURL* handle)
{
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, nullptr);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, nullptr);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, nullptr);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, nullptr);
    curl_easy_setopt(
        handle,
        CURLOPT_WRITEFUNCTION,
        +[](char*, std::size_t size, std::size_t count, void*) -> std::size_t {
            return size * count;
        });
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, nullptr);
}

/**
 * @brief fake-IP、回环、局域网和保留地址不参与公网优选，保留
 * TUN/系统解析行为。
 * @param value 系统解析器或可信调用方提供的数值地址。
 * @return 可以作为公网下载候选时为 true。
 * @details 私有解析不能被强制替换成公网地址，否则会破坏拆分 DNS。
 * IPv6 链路本地地址还依赖 scope，不能按无 scope 字面值缓存。
 * 文档和基准测试前缀不代表真实公网连接，尤其不能优选 TUN fake-IP。
 * 过滤不是身份认证，最终仍须用原主机名进行 TLS 校验。
 */
bool publicAddress(const std::string& value)
{
    std::array<unsigned char, 16> bytes{};
    if ( inet_pton(AF_INET, value.c_str(), bytes.data()) == 1 ) {
        const auto a = bytes[0], b = bytes[1];
        return a > 0 && a < 224 && a != 10 && a != 127 &&
               !(a == 169 && b == 254) && !(a == 172 && b >= 16 && b <= 31) &&
               !(a == 100 && b >= 64 && b <= 127) &&
               !(a == 192 &&
                 (b == 168 || (b == 0 && (bytes[2] == 0 || bytes[2] == 2)))) &&
               !(a == 198 &&
                 (b == 18 || b == 19 || (b == 51 && bytes[2] == 100))) &&
               !(a == 203 && b == 0 && bytes[2] == 113);
    }
    // 仅接受全局单播；排除文档与基准测试前缀，包括常见的 TUN fake-IPv6。
    return inet_pton(AF_INET6, value.c_str(), bytes.data()) == 1 &&
           (bytes[0] & 0xe0) == 0x20 &&
           !(bytes[0] == 0x20 && bytes[1] == 1 &&
             ((bytes[2] == 0x0d && bytes[3] == 0xb8) ||
              (bytes[2] == 0 && bytes[3] == 2)));
}

/**
 * @brief 显式代理由代理负责连接或解析，不能为了优选绕开用户的代理策略。
 * @return 有 HTTPS 或通用代理配置时跳过主动地址测速。
 * @details HTTP CONNECT 或远端解析 SOCKS 可能忽略本地 DNS 覆盖。
 * 此时对不同地址的请求不一定经过不同线路，不能产生伪排名。
 * 保守跳过不修改 NO_PROXY、代理认证或 libcurl 的连接策略。
 * 环境只在后台任务读取，不扫描浏览器或系统中的代理凭据。
 */
bool explicitProxy()
{
    for ( const char* name :
          { "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY" } ) {
        const char* value = std::getenv(name);
        if ( value && *value ) return true;
    }
    return false;
}

/**
 * @brief
 * 只解析官方主机当前的地址，不扫描地址段或采用服务器统一下发的永久排名。
 * @param options 可选的可信预解析结果，普通应用调用留空。
 * @return 去重且有上限的候选集，空集合表示保持系统连接路径。
 * @details sockaddr 缓冲只在当前解析调用内有效，不保存其中的裸指针。
 * 检测到 fake-IP 时整组退出，不把私有与公网解析混为一个排名。
 * IPv4 与 IPv6 按正文吞吐量比较，解析排序不是永久优先级。
 * 候选只来自该域名当前记录，不使用扫描得到的其他客户地址。
 * @warning getaddrinfo 可能等待操作系统 DNS 超时，不能持有 UI 或会话锁。
 */
std::vector<std::string> addresses(const DownloadRoutingOptions& options)
{
    std::vector<std::string> result = options.m_addresses;
    if ( result.empty() ) {
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list    = nullptr;
        if ( getaddrinfo("downloads.xiang233.top", "443", &hints, &list) != 0 )
            return {};
        for ( auto* entry = list; entry; entry = entry->ai_next ) {
            std::array<char, NI_MAXHOST> host{};
            if ( getnameinfo(entry->ai_addr,
                             static_cast<socklen_t>(entry->ai_addrlen),
                             host.data(),
                             static_cast<socklen_t>(host.size()),
                             nullptr,
                             0,
                             NI_NUMERICHOST) == 0 )
                result.emplace_back(host.data());
        }
        freeaddrinfo(list);
    }
    // 含 fake-IP 时整组交还系统，不能把 TUN 地址与真实 IP 混合后绕过路由语义。
    if ( std::any_of(result.begin(), result.end(), [](const auto& ip) {
             return !publicAddress(ip);
         }) )
        return {};
    std::vector<std::string> unique;
    for ( const auto& ip : result )
        if ( std::find(unique.begin(), unique.end(), ip) == unique.end() &&
             unique.size() < 4 )
            unique.push_back(ip);
    return unique;
}

/**
 * @brief 设置临时 DNS 映射；默认线路显式移除本 handle 中以前的覆盖记录。
 * @param handle 即将同步执行请求的独立句柄。
 * @param ip 单个数值候选；空字符串恢复系统解析。
 * @return 列表由调用方在 perform 结束后释放，分配失败返回空。
 * @details CURLOPT_RESOLVE 不复制列表，其生命周期不能短于传输。
 * 恢复默认需要删除记录，仅设置空列表不会清除已有 DNS 覆盖。
 * 只替换 socket 的目的地址，URL、Host、SNI 和证书主机名保持不变。
 */
curl_slist* pin(CURL* handle, const std::string& ip)
{
    const std::string rule =
        ip.empty() ? "-downloads.xiang233.top:443"
                   : "downloads.xiang233.top:443:" +
                         (ip.find(':') == ip.npos ? ip : "[" + ip + "]");
    auto* list = curl_slist_append(nullptr, rule.c_str());
    if ( list ) curl_easy_setopt(handle, CURLOPT_RESOLVE, list);
    return list;
}

/**
 * @brief 短临界区只发布地址值，不跨 DNS、网络、磁盘或回调持锁。
 * @param ip 已选地址，空字符串使后续任务重新探测。
 * @details 缓存只存在于本进程，不记录用户 IP、SSID 或网络名称。
 * 单调时间避免系统校时延长结果寿命，五分钟后自然失效。
 * 网络失败或持续慢速会提前清除，不能把旧地址当作永久配置。
 * 锁内只复制一个短字符串，不等待 DNS、文件或其他线程的网络请求。
 */
void remember(const std::string& ip)
{
    std::lock_guard lock(routeMutex);
    const auto      now = std::chrono::steady_clock::now();
    // 同地址复用不能不断延长寿命，否则连续下载会永久跳过网络重新评估。
    // 清空立即使结果失效；只有新地址开启独立的五分钟有效期。
    // 仍只在短临界区内处理值，不跨文件、DNS 或网络操作持锁。
    if ( !ip.empty() && cachedAddress == ip && now < cachedUntil ) return;
    cachedAddress = ip;
    cachedUntil   = ip.empty() ? std::chrono::steady_clock::time_point{}
                               : now + std::chrono::minutes(5);
}

/**
 * @brief Range 被忽略或身份不确定时截断重来，不把不同响应拼接成伪完整文件。
 * @param file 调用方打开的普通二进制文件，不支持管道或不可定位设备。
 * @return 缓冲刷新、截断和定位全部成功时返回 true。
 * @details 必须先刷新 stdio 缓冲，再截断底层文件。
 * 顺序相反会让旧缓冲在之后重新扩大文件，留下错误尾部。
 * 仅定位到头部也不够，新内容较短时同样可能残留旧字节。
 * 调用方继续拥有句柄，失败时由其关闭并删除不可用部分文件。
 */
bool restart(FILE* file)
{
    if ( std::fflush(file) != 0 ) return false;
#ifdef _WIN32
    if ( _chsize_s(_fileno(file), 0) != 0 ) return false;
#else
    if ( ftruncate(fileno(file), 0) != 0 ) return false;
#endif
    std::clearerr(file);
    return std::fseek(file, 0, SEEK_SET) == 0;
}
}  // namespace

/**
 * @brief 只允许默认 HTTPS 端口、精确主机和下载命名空间进入镜像策略。
 * @param url 清单中的绝对 HTTPS 下载 URL。
 * @return 官方下载命名空间时返回 true，其他来源沿用原传输。
 * @details 主机后固定斜杠拒绝相似后缀域名和用户信息形式。
 * 版本摘要查询参数不能因换线路而丢失，否则可能取到其他发布对象。
 */
bool isR2DownloadUrl(const std::string& url)
{
    if ( !url.starts_with(PREFIX) || url.find_first_of("\r\n\\") != url.npos )
        return false;
    // curl 与源站都可能归一化路径，不能仅凭原始字符串前缀授权镜像。
    // 这里只检查路径段，SHA256 查询或片段中的点不会改变对象位置。
    // 拒绝原始点段及其单次百分号编码形式，防止离开下载命名空间。
    std::string_view path(url);
    path.remove_prefix(PREFIX.size());
    path = path.substr(0, path.find_first_of("?#"));
    while ( !path.empty() ) {
        const auto  slash = path.find('/');
        std::string segment(path.substr(0, slash));
        // 百分号编码大小写等价；只规范化此检查涉及的 E，不改写真实 URL。
        for ( char& c : segment )
            if ( c == 'E' ) c = 'e';
        if ( segment == "." || segment == ".." || segment == "%2e" ||
             segment == "%2e%2e" || segment == ".%2e" || segment == "%2e." )
            return false;
        if ( slash == path.npos ) break;
        // 普通文件名中的点合法，只有完整路径段具有目录归一化语义。
        path.remove_prefix(slash + 1);
    }
    return true;
}

/**
 * @brief 同一后台任务中完成测速、候选切换和受 Range/ETag 约束的续传。
 * @warning 包含操作系统 DNS 解析、同步网络与磁盘操作；不得在热路径或持 UI
 * 锁调用。
 * @param handle 已配置平台信任与 User-Agent 的请求独占句柄。
 * @param output 调用方打开且负责关闭的二进制普通文件。
 * @param options 在同步任务期间保持有效的回调和策略。
 * @return curl 状态；成功后仍须由调用方验证完整 SHA256。
 * @details 测速比较实际正文速率，不能让较快握手掩盖较慢数据传输。
 * 只有带强实体标识的前缀才可复用，否则只复用排名并从零下载。
 * 每条线路忽略 Range 时最多截断重试一次，避免异常响应引发死循环。
 * 默认 DNS 与既有官方镜像提供有限回退，不进行无限尝试。
 * 任务截止时间包含测速与切换，不能把超时在每次重试时重新计满。
 * @warning 不得在 UI/逻辑热路径或持有会话锁时调用，取消回调也不能等待本任务。
 */
static CURLcode performDownloadAttempt(
    CURL* handle, const std::string& url, FILE* output,
    const DownloadRoutingOptions& options, DownloadRoute route,
    std::chrono::steady_clock::time_point deadline)
{
    if ( !handle || !output ) return CURLE_BAD_FUNCTION_ARGUMENT;
    if ( options.m_cancelled && options.m_cancelled() )
        return CURLE_ABORTED_BY_CALLBACK;
    const bool official = isR2DownloadUrl(url);
    const bool optimize =
        official && route != DownloadRoute::Origin && !explicitProxy();
    const auto changed = [&] {
        return official && getDownloadRoute() != route;
    };
    const std::string selectedUrl = downloadUrlForRoute(url, route);
    // 非官方地址沿用调用方证书、代理与协议能力，不重写到官方镜像。
    // 缓存只记录一个候选值，不与 UI 或会话状态共用所有权。
    // 临界区结束后才进行 DNS 和网络 I/O，不能跨整个任务持锁。
    // 显式代理可能负责远端解析，此时不能把本地候选误认为实际目的地址。
    // 透明 TUN 的 fake-IP 由后续整组过滤识别，失败时仍走系统原路径。
    std::vector<std::string> candidates;
    std::string              preferred;
    if ( optimize ) {
        std::lock_guard lock(routeMutex);
        if ( std::chrono::steady_clock::now() < cachedUntil &&
             (options.m_addresses.empty() ||
              std::find(options.m_addresses.begin(),
                        options.m_addresses.end(),
                        cachedAddress) != options.m_addresses.end()) )
            preferred = cachedAddress;
    }
    Transfer best;
    best.m_options  = &options;
    double bestRate = 0;
    if ( optimize && preferred.empty() && options.m_probeRoutes &&
         (options.m_expectedSize == 0 ||
          options.m_expectedSize >= PROBE_BYTES) ) {
        candidates = addresses(options);
        for ( const auto& ip : candidates ) {
            if ( changed() ) return CURLE_ABORTED_BY_CALLBACK;
            if ( options.m_cancelled && options.m_cancelled() )
                return CURLE_ABORTED_BY_CALLBACK;
            CURL* probe = curl_easy_duphandle(handle);
            if ( !probe ) continue;
            const auto probeRemaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count();
            if ( probeRemaining <= 0 ) {
                curl_easy_cleanup(probe);
                break;
            }
            Transfer state;
            state.m_options      = &options;
            state.m_probe        = true;
            state.m_route        = route;
            state.m_monitorRoute = official;
            // clone 继承平台信任锚与 User-Agent，不能用未配置证书的句柄替代。
            // 写入和进度回调必须覆盖，否则测速会污染最终文件或伪报下载进度。
            // 此处仅持有一个候选前缀，内存上限不随资源包大小增长。
            // 探测超时也计入总截止时间，不能每换一个地址就重新计满任务超时。
            // 取消在每个探针前复核，取消后不得继续访问下一候选。
            callbacks(probe, state);
            // 同一句柄可能刚从源站切回 R2，不能沿用上一条线路的 URL
            // 或续传偏移。 探针必须重新从原始 R2 对象零偏移开始，排名才代表候选
            // IP 的实际吞吐。
            curl_easy_setopt(probe, CURLOPT_URL, url.c_str());
            curl_easy_setopt(
                probe, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(0));
            auto* mapping = pin(probe, ip);
            curl_easy_setopt(probe, CURLOPT_RANGE, "0-1048575");
            curl_easy_setopt(probe,
                             CURLOPT_TIMEOUT_MS,
                             static_cast<long>(
                                 std::min<std::int64_t>(2500, probeRemaining)));
            curl_easy_setopt(probe, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
            const auto code =
                mapping ? curl_easy_perform(probe) : CURLE_OUT_OF_MEMORY;
            double elapsed{}, firstByte{};
            curl_easy_getinfo(probe, CURLINFO_TOTAL_TIME, &elapsed);
            curl_easy_getinfo(probe, CURLINFO_STARTTRANSFER_TIME, &firstByte);
            curl_easy_cleanup(probe);
            curl_slist_free_all(mapping);
            // 完整小文件可直接复用；部分探针必须收到足量正文，不能只凭握手排名。
            const bool completeSmall =
                state.m_total != 0 && state.m_total == state.m_bytes.size();
            if ( (code == CURLE_OK || code == CURLE_OPERATION_TIMEDOUT) &&
                 !state.m_rejected && state.m_status == 206 &&
                 state.m_rangeStart == 0 &&
                 (state.m_bytes.size() >= 64 * 1024 || completeSmall) ) {
                const double rate =
                    state.m_bytes.size() / std::max(0.001, elapsed - firstByte);
                if ( rate > bestRate ) {
                    bestRate  = rate;
                    preferred = ip;
                    best      = std::move(state);
                }
                if ( completeSmall ) break;
            }
        }
    }
    if ( !preferred.empty() ) {
        candidates.erase(
            std::remove(candidates.begin(), candidates.end(), preferred),
            candidates.end());
        candidates.insert(candidates.begin(), preferred);
        XINFO("Download route selected: {}", preferred);
    }
    // 默认 DNS 作为候选失败后的兼容路径；最后才使用现有官方直连镜像。
    // 只保留胜出样本，不把各候选的正文累计成完整包副本。
    // 有强 ETag 的部分前缀才能复用，没有身份的样本只用于排名。
    // 完整小文件可以直接复用，但上层仍会验证原清单的 SHA256。
    // 镜像路径保留不可变版本目录与摘要查询，不能降级成其他版本的文件。
    // 候选列表有限，默认 DNS 或镜像失败后返回错误，不进行无限重试。
    candidates.emplace_back();
    const std::size_t directIndex = candidates.size();
    // 手动 R2 必须遵守用户选源；只有自动模式允许跨源站回退。
    if ( optimize && route == DownloadRoute::Automatic &&
         options.m_allowOriginFallback )
        candidates.emplace_back();
    std::uint64_t offset     = 0;
    std::uint64_t knownTotal = best.m_total;
    std::string   etag;
    if ( changed() ) return CURLE_ABORTED_BY_CALLBACK;
    if ( !best.m_bytes.empty() &&
         (!best.m_etag.empty() || best.m_bytes.size() == best.m_total) ) {
        if ( std::fwrite(best.m_bytes.data(), 1, best.m_bytes.size(), output) !=
             best.m_bytes.size() )
            return CURLE_WRITE_ERROR;
        offset = best.m_bytes.size();
        etag   = best.m_etag;
        if ( offset == best.m_total ) return CURLE_OK;
    }
    CURLcode result = CURLE_COULDNT_CONNECT;
    for ( std::size_t i = 0; i < candidates.size(); ++i ) {
        const std::string activeUrl =
            i == directIndex ? "https://mmm.xiang233.top/download/" +
                                   url.substr(PREFIX.size())
                             : selectedUrl;
        // 忽略 Range 只允许原线路重启一次，避免服务器行为异常导致重试死循环。
        for ( int attempt = 0; attempt < 2; ++attempt ) {
            if ( changed() ) return CURLE_ABORTED_BY_CALLBACK;
            if ( options.m_cancelled && options.m_cancelled() )
                return CURLE_ABORTED_BY_CALLBACK;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count();
            if ( remaining <= 0 ) return CURLE_OPERATION_TIMEDOUT;
            Transfer state;
            state.m_options       = &options;
            state.m_route         = route;
            state.m_monitorRoute  = official;
            state.m_file          = output;
            state.m_offset        = offset;
            state.m_expectedTag   = etag;
            state.m_expectedTotal = knownTotal;
            auto* mapping         = pin(handle, candidates[i]);
            if ( !mapping ) return CURLE_OUT_OF_MEMORY;
            callbacks(handle, state);
            curl_slist* headers = nullptr;
            if ( offset != 0 && !etag.empty() )
                headers =
                    curl_slist_append(nullptr, ("If-Range: " + etag).c_str());
            curl_easy_setopt(handle, CURLOPT_URL, activeUrl.c_str());
            curl_easy_setopt(handle, CURLOPT_RANGE, nullptr);
            curl_easy_setopt(handle,
                             CURLOPT_RESUME_FROM_LARGE,
                             static_cast<curl_off_t>(offset));
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(handle,
                             CURLOPT_TIMEOUT_MS,
                             static_cast<long>(std::min<std::int64_t>(
                                 remaining, std::numeric_limits<long>::max())));
            curl_easy_setopt(handle,
                             CURLOPT_LOW_SPEED_LIMIT,
                             optimize && i < directIndex ? 128L * 1024 : 0L);
            curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 5L);
            result = curl_easy_perform(handle);
            // 句柄由调用方在返回后销毁，不能把栈上 Transfer 地址留在其中。
            // curl 的协议关闭阶段也可能调用头部或进度回调，必须先撤销借用。
            detachCallbacks(handle);
            // libcurl 也可能在调用正文回调前拒绝被忽略的
            // Range，统一走截断重启。
            if ( state.m_offset != 0 &&
                 (result == CURLE_RANGE_ERROR || state.m_status == 200) )
                state.m_rejected = true;
            curl_easy_setopt(handle, CURLOPT_RESOLVE, nullptr);
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, nullptr);
            curl_slist_free_all(mapping);
            curl_slist_free_all(headers);
            // written 只包含实际落盘量，不包括头部、测速或被拒绝的正文。
            // 新响应的身份与此前保留数据分开建模，不能互相覆盖。
            // 正常 200 只允许写入空文件；206 还必须满足原有偏移与实体标识。
            // 默认 DNS 使用删除记录恢复，单纯清空列表不会移除旧 DNS 覆盖。
            // 句柄已经停止访问后才释放 RESOLVE 和 If-Range 的列表。
            // 先退出旧策略，外层才有机会截断并使用用户刚选的新线路。
            if ( changed() ) {
                // 存储短写优先于同时发生的选线操作，不能靠重下掩盖磁盘错误。
                // 错误 Range 主动拒绝正文产生的 WRITE_ERROR
                // 则仍属于可换路退出。
                if ( result == CURLE_WRITE_ERROR && !state.m_rejected )
                    return result;
                return CURLE_ABORTED_BY_CALLBACK;
            }
            offset += state.m_written;
            if ( state.m_total != 0 ) knownTotal = state.m_total;
            if ( result == CURLE_OK && (options.m_expectedSize == 0 ||
                                        offset == options.m_expectedSize) ) {
                if ( !candidates[i].empty() ) remember(candidates[i]);
                return result;
            }
            // curl 成功仍不能替代清单总长检查，短文件必须视为未完成。
            // 用户取消优先于故障恢复，关闭下载不能再触发其他线路。
            // 磁盘短写不是线路问题，继续切换只会浪费流量或掩盖存储故障。
            // HTTP 或网络中断只有具备强身份时才能继续保留现有字节。
            // 截断重来时同时清除旧总长和标识，避免继承前一个对象的协议状态。
            // 成功文件最终仍由业务调用方进行完整摘要认证。
            if ( result == CURLE_OK ) result = CURLE_PARTIAL_FILE;
            if ( options.m_cancelled && options.m_cancelled() )
                return CURLE_ABORTED_BY_CALLBACK;
            if ( result == CURLE_WRITE_ERROR && !state.m_rejected )
                return result;
            // 只有强 ETag
            // 才允许保留部分字节；其他错误从零开始，完整摘要由上层复核。
            if ( state.m_rejected || state.m_etag.empty() ) {
                if ( !restart(output) ) return CURLE_WRITE_ERROR;
                offset     = 0;
                knownTotal = 0;
                etag.clear();
            } else
                etag = state.m_etag;
            if ( state.m_rejected ) continue;
            break;
        }
        if ( optimize ) remember("");
    }
    return result;
}
/// @brief 会话选线只传递一个值，后台线程不借用任何 UI 或配置对象。
/// @warning 渲染热路径读取和网络回调轮询均使用 relaxed，不承担其他数据发布。
DownloadRoute getDownloadRoute()
{
    return selectedRoute.load(std::memory_order_relaxed);
}

/// @brief 发布用户选择；不执行网络、文件操作或等待旧请求结束。
/// @warning UI 低频写、后台读取同一原子枚举，不依赖附加数据的同步。
void setDownloadRoute(DownloadRoute route)
{
    // 防御外部调用传入非法枚举，不让 UI 的选项下标越界。
    if ( route != DownloadRoute::R2 && route != DownloadRoute::Origin )
        route = DownloadRoute::Automatic;
    selectedRoute.store(route, std::memory_order_relaxed);
}

/// @brief 手动直连使用已发布的官方同路径副本，其他 URL 完全保留。
/// @note 摘要查询仍来自原清单，最终身份验证不因选源而改变。
std::string downloadUrlForRoute(const std::string& url, DownloadRoute route)
{
    if ( route == DownloadRoute::Origin && isR2DownloadUrl(url) )
        return "https://mmm.xiang233.top/download/" + url.substr(PREFIX.size());
    return url;
}

/**
 * @brief 允许用户在下载期间切换线路，同时保持原任务的取消、超时和校验契约。
 * @details 切换后仅截断当前文件，已经完成的资源由上层同步器继续复用。
 * 旧线路退出前不能让另一请求同时写入同一 FILE，所有重试顺序执行。
 * 原子选项只表示会话意图，不跨进程持久化或修改系统网络设置。
 * 用户退出始终优先于换路，原任务截止时间不能随切换无限延长。
 * @warning 低频后台同步调用，含 DNS、文件和网络 I/O，不得在 UI 热路径执行。
 */
CURLcode performRoutedDownload(CURL* handle, const std::string& url,
                               FILE*                         output,
                               const DownloadRoutingOptions& options)
{
    if ( !handle || !output ) return CURLE_BAD_FUNCTION_ARGUMENT;
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(std::max(1L, options.m_timeoutSeconds));
    for ( ;; ) {
        if ( options.m_cancelled && options.m_cancelled() )
            return CURLE_ABORTED_BY_CALLBACK;
        if ( std::chrono::steady_clock::now() >= deadline )
            return CURLE_OPERATION_TIMEDOUT;
        const auto route  = getDownloadRoute();
        const auto result = performDownloadAttempt(
            handle, url, output, options, route, deadline);
        if ( options.m_cancelled && options.m_cancelled() )
            return CURLE_ABORTED_BY_CALLBACK;
        if ( !isR2DownloadUrl(url) || getDownloadRoute() == route )
            return result;
        // 只有明确的线路变化触发重来；普通网络错误仍交给原有有限回退策略。
        // 磁盘短写即使与用户选线同时发生，也不能被重新下载掩盖。
        if ( result == CURLE_WRITE_ERROR || !restart(output) )
            return CURLE_WRITE_ERROR;
        if ( options.m_progress )
            options.m_progress(
                0, static_cast<std::int64_t>(options.m_expectedSize));
    }
}
}  // namespace MMM::Network
