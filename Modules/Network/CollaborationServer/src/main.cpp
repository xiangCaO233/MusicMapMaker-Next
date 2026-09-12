#include "network/collaboration_server/CollaborationSignalingServer.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
/// @brief 进程退出标志，仅由信号处理器写入。
/// @warning 信号处理器写入、服务主循环读取；只用于请求有序关闭。
std::atomic_bool EXIT_REQUESTED{ false };

/// @brief 处理 SIGINT/SIGTERM 并请求主循环退出。
/// @param signalNumber 操作系统信号编号；退出策略对两种信号相同。
/// @warning 信号上下文只写原子标志，不调用日志、分配或服务端 API。
void requestExit(int)
{
    // relaxed 已足够传递单向退出请求，不承载其他内存的发布关系。
    EXIT_REQUESTED.store(true, std::memory_order_relaxed);
}

/// @brief 转发 libdatachannel 日志到项目日志系统。
/// @param level libdatachannel 提供的严重程度。
/// @param message 仅在回调期间有效的零结尾日志文本。
/// @warning 回调可能来自网络线程，不得访问服务主循环的可变状态。
void logRtcMessage(rtcLogLevel level, const char* message)
{
    // 第三方允许空消息，直接忽略可避免传入格式化层。
    if ( !message ) return;
    // 致命和错误统一进入错误通道，确保部署日志能被告警采集。
    if ( level == RTC_LOG_FATAL || level == RTC_LOG_ERROR ) {
        XERROR("libdatachannel: {}", message);
    } else if ( level == RTC_LOG_WARNING ) {
        // 可恢复的连接异常保持 Warning 级别，不误报为服务进程故障。
        XWARN("libdatachannel: {}", message);
    } else {
        // 其余级别仅在第三方阈值允许时到达，统一映射为 Info。
        XINFO("libdatachannel: {}", message);
    }
}

/// @brief 从 JSON 对象读取字符串字段。
/// @param object 配置根对象或嵌套对象。
/// @param key 需要读取的字段名。
/// @param value 成功时接收字符串副本的输出参数。
/// @return 字段存在且类型为字符串时返回 true。
bool readString(const nlohmann::json& object, std::string_view key,
                std::string& value)
{
    // find 同时避免缺失字段抛出异常，并保留调用方现有默认值。
    const auto iterator = object.find(key);
    // 可选字段由调用方忽略 false；必需字段据此拒绝整份配置。
    if ( iterator == object.end() || !iterator->is_string() ) return false;
    // get_ref 避免中间转换，赋值后 value 不依赖 JSON 对象生命周期。
    value = iterator->get_ref<const std::string&>();
    return true;
}

/// @brief 校验 ICE 地址和长期凭据可安全嵌入 libdatachannel URI。
/// @param value 未转义的地址、用户名或密码片段。
/// @param allowAddressPunctuation 是否额外允许端口分隔和 IPv6 方括号。
/// @return 非空、长度受限且仅含安全字符时返回 true。
bool isSafeIceToken(std::string_view value, bool allowAddressPunctuation)
{
    // 253 字节覆盖完整 DNS 名上限，也限制恶意配置的处理成本。
    if ( value.empty() || value.size() > 253U ) return false;
    // 白名单禁止 @、?、斜杠和空白改变 URI authority 或查询语义。
    return std::all_of(
        value.begin(), value.end(), [allowAddressPunctuation](char character) {
            // cctype 只接受 unsigned char 范围，显式转换避免负值未定义行为。
            const auto byte = static_cast<unsigned char>(character);
            // RFC 3986 非保留字符可用于地址与凭据，无需额外百分号解码。
            return std::isalnum(byte) != 0 || character == '-' ||
                   character == '_' || character == '.' || character == '~' ||
                   // 冒号与方括号只为主机地址开放，凭据中仍严格禁止。
                   (allowAddressPunctuation &&
                    (character == ':' || character == '[' || character == ']'));
        });
}

/// @brief 从结构化地址和端口配置生成 STUN/TURN URI。
/// @param object ice 配置对象。
/// @param config 接收生成 URI 的信令服务配置。
/// @return 至少启用一种服务且全部字段合法时返回 true。
/// @note 失败前可能已追加 STUN URI；调用方只在整体成功时使用 config。
bool loadStructuredIceConfig(
    const nlohmann::json& object,
    MMM::Network::CollaborationServer::CollaborationSignalingServerConfig&
        config)
{
    // 数组、字符串等非对象输入无法承载结构化 ICE 字段。
    if ( !object.is_object() ) return false;
    std::string address;
    // 地址是所有生成 URI 的共同基础，必须存在并通过注入防护。
    if ( !readString(object, "address", address) ||
         !isSafeIceToken(address, true) ) {
        return false;
    }
    // 默认仅启用 STUN；TURN 必须由部署者显式开启并提供凭据。
    bool enableStun    = true;
    bool enableTurn    = false;
    bool enableTurnTcp = false;
    // 可选布尔字段一旦存在就必须类型正确，禁止静默容忍拼写错误值。
    if ( const auto iterator = object.find("enableStun");
         iterator != object.end() ) {
        if ( !iterator->is_boolean() ) return false;
        enableStun = iterator->get<bool>();
    }
    // TURN UDP 与 STUN 相互独立，可按部署网络环境分别开关。
    if ( const auto iterator = object.find("enableTurn");
         iterator != object.end() ) {
        if ( !iterator->is_boolean() ) return false;
        enableTurn = iterator->get<bool>();
    }
    // TCP URI 只有在 TURN 总开关启用时才会实际生成。
    if ( const auto iterator = object.find("enableTurnTcp");
         iterator != object.end() ) {
        if ( !iterator->is_boolean() ) return false;
        enableTurnTcp = iterator->get<bool>();
    }
    // 局部读取器统一执行无符号类型和有效端口范围检查。
    const auto readPort = [&object](std::string_view key,
                                    std::uint16_t&   value) {
        const auto iterator = object.find(key);
        // 启用对应协议后端口是必需字段，缺失不能回退到隐式值。
        if ( iterator == object.end() || !iterator->is_number_unsigned() ) {
            return false;
        }
        const auto port = iterator->get<std::uint64_t>();
        // 零端口和超出 uint16_t 的值均无法作为远端 ICE 服务端口。
        if ( port == 0 || port > 65535 ) return false;
        value = static_cast<std::uint16_t>(port);
        return true;
    };
    // 初始值表达常见默认端口，但只有对应协议关闭时才不会被读取覆盖。
    std::uint16_t stunPort = 3478;
    std::uint16_t turnPort = 3478;
    // 只验证已启用协议的端口，使关闭服务时无需保留无效占位字段。
    if ( enableStun && !readPort("stunPort", stunPort) ) return false;
    if ( enableTurn && !readPort("turnPort", turnPort) ) return false;

    if ( enableStun ) {
        // STUN 不携带长期凭据，只组合经过白名单检查的地址和端口。
        config.iceServers.push_back("stun:" + address + ':' +
                                    std::to_string(stunPort));
    }
    if ( enableTurn ) {
        // TURN authority 需要用户名与密码，两者都必须完整存在。
        std::string username;
        std::string password;
        if ( !readString(object, "turnUsername", username) ||
             !readString(object, "turnPassword", password) ||
             !isSafeIceToken(username, false) ||
             !isSafeIceToken(password, false) ) {
            return false;
        }
        // 凭据字符已禁止 URI 分隔符，可安全按 libdatachannel 格式拼接。
        const std::string base = "turn:" + username + ':' + password + '@' +
                                 address + ':' + std::to_string(turnPort);
        // UDP 始终是启用 TURN 后的基础候选，兼容常规 NAT 穿透。
        config.iceServers.push_back(base + "?transport=udp");
        if ( enableTurnTcp ) {
            // TCP 作为额外候选保留，不替换 UDP 路径。
            config.iceServers.push_back(base + "?transport=tcp");
        }
    }
    // 两种协议都关闭的结构化配置没有实际意义，应拒绝启动。
    return enableStun || enableTurn;
}

/// @brief 读取服务 JSON 配置。
/// @param path UTF-8 配置文件路径。
/// @param config 接收解析结果的服务配置，未出现字段保留默认值。
/// @return 文件、JSON、字段类型及组合约束全部合法时返回 true。
bool loadConfig(
    const std::string& path,
    MMM::Network::CollaborationServer::CollaborationSignalingServerConfig&
        config)
{
    // 二进制读取避免平台文本转换，JSON 解析器自行处理 UTF-8 内容。
    std::ifstream stream(path, std::ios::binary);
    // 无法打开时不区分不存在与权限错误，由入口统一报告配置失败。
    if ( !stream ) return false;
    // 禁用异常并用 discarded 表达语法错误，符合项目禁止异常约束。
    const auto json = nlohmann::json::parse(stream, nullptr, false);
    if ( json.is_discarded() || !json.is_object() ) return false;

    // 端口是可选覆盖；零值允许服务端请求操作系统分配临时端口。
    if ( const auto iterator = json.find("port");
         iterator != json.end() && iterator->is_number_unsigned() ) {
        const auto value = iterator->get<std::uint64_t>();
        // 转换前检查上界，避免截断为另一个有效端口。
        if ( value > 65535 ) return false;
        config.port = static_cast<std::uint16_t>(value);
    }
    // bindAddress 缺失或类型错误时保留配置类型提供的默认监听地址。
    static_cast<void>(readString(json, "bindAddress", config.bindAddress));
    // TLS 开关仅接受明确布尔值；其他类型按缺省值处理以兼容旧配置。
    if ( const auto iterator = json.find("enableTls");
         iterator != json.end() && iterator->is_boolean() ) {
        config.enableTls = iterator->get<bool>();
    }
    // 证书和私钥路径在末尾按 TLS 开关做组合完整性校验。
    static_cast<void>(
        readString(json, "certificatePemFile", config.certificatePemFile));
    static_cast<void>(readString(json, "keyPemFile", config.keyPemFile));
    // 房间与客户端上限由服务实现进一步使用，入口只接受无符号数。
    if ( const auto iterator = json.find("maxRooms");
         iterator != json.end() && iterator->is_number_unsigned() ) {
        config.maxRooms = iterator->get<std::size_t>();
    }
    if ( const auto iterator = json.find("maxClients");
         iterator != json.end() && iterator->is_number_unsigned() ) {
        config.maxClients = iterator->get<std::size_t>();
    }
    // 显式 URI 数组优先于结构化 ice 对象，避免两套来源重复追加。
    if ( const auto iterator = json.find("iceServers");
         iterator != json.end() ) {
        // 数组元素必须逐项为 URI 字符串，不容忍混合类型。
        if ( !iterator->is_array() ) return false;
        for ( const auto& value : *iterator ) {
            if ( !value.is_string() ) return false;
            const auto& uri = value.get_ref<const std::string&>();
            // 限制单项长度，避免异常配置放大内存和第三方解析成本。
            if ( uri.empty() || uri.size() > 512 ) return false;
            // 原始 URI 模式面向受信部署配置，不在此处重写协议内容。
            config.iceServers.push_back(uri);
        }
    } else if ( const auto iterator = json.find("ice");
                iterator != json.end() ) {
        // 未提供原始列表时，才从受约束字段生成 STUN/TURN URI。
        if ( !loadStructuredIceConfig(*iterator, config) ) return false;
    }
    // TLS 启用时证书和私钥必须成对存在，避免启动后才暴露半配置。
    return !config.enableTls ||
           (!config.certificatePemFile.empty() && !config.keyPemFile.empty());
}
}  // namespace

/// @brief 解析启动参数、加载配置并运行协作信令服务主循环。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数数组。
/// @return 正常关闭返回 0，缺少配置、解析失败或启动失败返回对应错误码。
int main(int argc, char** argv)
{
    // 日志必须先于任何配置或 RTC 操作初始化，确保早期错误可诊断。
    XLogger::init("MusicMapMaker-CollaborationServer");
    // 服务端默认只转发 Warning 及以上，控制长期部署日志体积。
    rtcInitLogger(RTC_LOG_WARNING, &logRtcMessage);

    // 当前入口只识别 --config，未知参数留给未来兼容扩展并忽略。
    std::string configPath;
    for ( int index = 1; index < argc; ++index ) {
        // 防御空 argv 项，避免构造 string_view 时解引用空指针。
        const std::string_view argument = argv[index] ? argv[index] : "";
        if ( argument == "--config" && index + 1 < argc && argv[index + 1] ) {
            // 前移 index 消费路径，防止路径自身被再次解释为参数。
            configPath = argv[++index];
        }
    }
    if ( configPath.empty() ) {
        // 参数错误使用独立退出码，便于服务管理器区分配置缺失。
        XERROR("Missing required --config <path> argument");
        // 所有提前返回都显式关闭异步日志后端。
        XLogger::shutdown();
        return 2;
    }

    // 配置对象先保留类型默认值，再用文件中的可选字段覆盖。
    MMM::Network::CollaborationServer::CollaborationSignalingServerConfig
        config;
    if ( !loadConfig(configPath, config) ) {
        // 不输出解析到的凭据或 URI，只记录配置文件位置。
        XERROR("Failed to load collaboration signaling config: {}", configPath);
        XLogger::shutdown();
        return 3;
    }

    // 服务对象留在 main 栈上，确保正常路径的生命周期顺序清晰。
    MMM::Network::CollaborationServer::CollaborationSignalingServer server;
    if ( !server.start(std::move(config)) ) {
        // 配置所有权仅在启动尝试时转移，失败后无需再次访问。
        XERROR("Failed to start collaboration signaling server");
        XLogger::shutdown();
        return 4;
    }

    // 仅在服务成功启动后安装退出处理器，避免早期路径维护额外状态。
    std::signal(SIGINT, &requestExit);
    std::signal(SIGTERM, &requestExit);
    // 记录实际监听端口，覆盖配置请求零端口时的系统分配结果。
    XINFO("Collaboration signaling server listening on port {}",
          server.listeningPort());

    // 主循环串行推进连接事件；退出标志每轮非阻塞读取。
    while ( !EXIT_REQUESTED.load(std::memory_order_relaxed) ) {
        server.update();
        // 10ms 间隔限制空闲轮询占用，不参与客户端 UI 或逻辑同步路径。
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 先停止服务并释放连接，再清理 libdatachannel 进程级资源。
    server.stop();
    rtcCleanup();
    // 关闭完成后记录最终状态，随后刷新并停止项目日志系统。
    XINFO("Collaboration signaling server stopped");
    XLogger::shutdown();
    return 0;
}
