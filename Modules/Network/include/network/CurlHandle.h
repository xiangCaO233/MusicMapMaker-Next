#pragma once

#include <curl/curl.h>
#include <string>

namespace MMM::Network
{
/// @brief 在启动阶段发布平台默认信任锚的 PEM 快照。
/// @param certificates 由平台信任管理器导出的根证书，不接受服务器叶证书替代。
/// @return 空输入或已经初始化时返回 false，不覆盖已发布的证书。
/// @warning 必须在网络工作线程启动前单线程调用，之后证书保持不可变。
bool initializeCurlTrust(std::string certificates);

/// @brief 创建配置平台证书信任的独立 curl easy handle，由调用方清理。
/// @return Android 缺少信任锚或设置失败时返回 nullptr，禁止降低验证要求。
/// @warning 仅用于后台网络请求，不得放入逐帧绘制或逻辑更新路径。
CURL* createCurlHandle();
}  // namespace MMM::Network
