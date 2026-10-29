#include "network/CurlHandle.h"
#include "log/colorful-log.h"
#include <utility>

namespace MMM::Network
{
namespace
{
/// @brief 平台启动时写入一次，网络线程只读；不保存 JNI 或 Activity 引用。
/// @warning 发布必须先于工作线程创建，禁止在请求执行期间重新赋值。
std::string systemCaBundle;
}  // namespace

/// @brief 接管启动阶段生成的证书值，发布后不再修改底层字符串。
/// @return 只有首次且非空的发布成功，失败不破坏原有快照。
/// @warning 调用方必须保证尚未启动网络工作线程，不提供运行期重载入口。
bool initializeCurlTrust(std::string certificates)
{
    // 空快照不表示信任所有证书，重复初始化也不能使已有请求的策略漂移。
    if ( certificates.empty() || !systemCaBundle.empty() ) return false;
    // 移动所有权后 Java 数组与调用方字符串均可释放，不保留观察指针。
    systemCaBundle = std::move(certificates);
    return true;
}

/// @brief 在 easy handle 发布给请求方前完成全部平台 TLS 配置。
/// @return 调用方继续使用 curl_easy_cleanup 管理非空返回值。
/// @warning 后台请求低频调用；不访问文件、JNI 或可变的 UI 配置。
CURL* createCurlHandle()
{
    // 桌面未发布平台快照时仍沿用原有 curl 后端与系统信任配置。
    CURL* handle = curl_easy_init();
    // 底层资源创建失败与信任配置失败具有相同的空句柄所有权契约。
    if ( !handle ) return nullptr;
#ifdef __ANDROID__
    // 平台证书不可用时只拒绝网络请求，不能回退为不验证的 HTTPS。
    if ( systemCaBundle.empty() ) {
        XERROR("Android TLS trust anchors are unavailable");
        curl_easy_cleanup(handle);
        return nullptr;
    }
#endif
    if ( !systemCaBundle.empty() ) {
        // PEM 只在启动阶段由平台导出，这里不会为每个请求再次遍历系统信任库。
        // CURL_BLOB_COPY 要求 libcurl 自己持有字节副本，不依赖本地 blob
        // 描述符。 每个请求复制 PEM，easy handle
        // 的生命周期不依赖启动线程的临时缓冲。
        curl_blob bundle{ systemCaBundle.data(),
                          systemCaBundle.size(),
                          CURL_BLOB_COPY };
        auto result = curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &bundle);
        // 每步沿用上一步错误，不能用后续设置成功覆盖证书装载失败。
        // 加载信任锚只补充验证来源，证书链和主机名检查都必须保持开启。
        if ( result == CURLE_OK )
            result = curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
        if ( result == CURLE_OK )
            result = curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
        if ( result != CURLE_OK ) {
            // 失败句柄不能逃逸给调用者，避免留下仅部分配置的连接。
            XERROR("TLS trust configuration failed: {}",
                   curl_easy_strerror(result));
            curl_easy_cleanup(handle);
            return nullptr;
        }
    }
    // 无平台快照的桌面分支不设置任何额外选项，保留原有后端行为。
    return handle;
}
}  // namespace MMM::Network
