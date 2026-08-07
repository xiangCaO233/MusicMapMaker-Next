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
std::atomic_bool EXIT_REQUESTED{ false };

/// @brief 处理 SIGINT/SIGTERM 并请求主循环退出。
void requestExit(int)
{
    EXIT_REQUESTED.store(true, std::memory_order_relaxed);
}

/// @brief 转发 libdatachannel 日志到项目日志系统。
void logRtcMessage(rtcLogLevel level, const char* message)
{
    if ( !message ) return;
    if ( level == RTC_LOG_FATAL || level == RTC_LOG_ERROR ) {
        XERROR("libdatachannel: {}", message);
    } else if ( level == RTC_LOG_WARNING ) {
        XWARN("libdatachannel: {}", message);
    } else {
        XINFO("libdatachannel: {}", message);
    }
}

/// @brief 从 JSON 对象读取字符串字段。
bool readString(const nlohmann::json& object, std::string_view key,
                std::string& value)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) return false;
    value = iterator->get_ref<const std::string&>();
    return true;
}

/// @brief 校验 ICE 地址和长期凭据可安全嵌入 libdatachannel URI。
bool isSafeIceToken(std::string_view value, bool allowAddressPunctuation)
{
    if ( value.empty() || value.size() > 253U ) return false;
    return std::all_of(
        value.begin(), value.end(), [allowAddressPunctuation](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return std::isalnum(byte) != 0 || character == '-' ||
                   character == '_' || character == '.' || character == '~' ||
                   (allowAddressPunctuation &&
                    (character == ':' || character == '[' || character == ']'));
        });
}

/// @brief 从结构化地址和端口配置生成 STUN/TURN URI。
bool loadStructuredIceConfig(
    const nlohmann::json& object,
    MMM::Network::CollaborationServer::CollaborationSignalingServerConfig&
        config)
{
    if ( !object.is_object() ) return false;
    std::string address;
    if ( !readString(object, "address", address) ||
         !isSafeIceToken(address, true) ) {
        return false;
    }
    bool enableStun    = true;
    bool enableTurn    = false;
    bool enableTurnTcp = false;
    if ( const auto iterator = object.find("enableStun");
         iterator != object.end() ) {
        if ( !iterator->is_boolean() ) return false;
        enableStun = iterator->get<bool>();
    }
    if ( const auto iterator = object.find("enableTurn");
         iterator != object.end() ) {
        if ( !iterator->is_boolean() ) return false;
        enableTurn = iterator->get<bool>();
    }
    if ( const auto iterator = object.find("enableTurnTcp");
         iterator != object.end() ) {
        if ( !iterator->is_boolean() ) return false;
        enableTurnTcp = iterator->get<bool>();
    }
    const auto readPort = [&object](std::string_view key,
                                    std::uint16_t&   value) {
        const auto iterator = object.find(key);
        if ( iterator == object.end() || !iterator->is_number_unsigned() ) {
            return false;
        }
        const auto port = iterator->get<std::uint64_t>();
        if ( port == 0 || port > 65535 ) return false;
        value = static_cast<std::uint16_t>(port);
        return true;
    };
    std::uint16_t stunPort = 3478;
    std::uint16_t turnPort = 3478;
    if ( enableStun && !readPort("stunPort", stunPort) ) return false;
    if ( enableTurn && !readPort("turnPort", turnPort) ) return false;

    if ( enableStun ) {
        config.iceServers.push_back("stun:" + address + ':' +
                                    std::to_string(stunPort));
    }
    if ( enableTurn ) {
        std::string username;
        std::string password;
        if ( !readString(object, "turnUsername", username) ||
             !readString(object, "turnPassword", password) ||
             !isSafeIceToken(username, false) ||
             !isSafeIceToken(password, false) ) {
            return false;
        }
        const std::string base = "turn:" + username + ':' + password + '@' +
                                 address + ':' + std::to_string(turnPort);
        config.iceServers.push_back(base + "?transport=udp");
        if ( enableTurnTcp ) {
            config.iceServers.push_back(base + "?transport=tcp");
        }
    }
    return enableStun || enableTurn;
}

/// @brief 读取服务 JSON 配置。
bool loadConfig(
    const std::string& path,
    MMM::Network::CollaborationServer::CollaborationSignalingServerConfig&
        config)
{
    std::ifstream stream(path, std::ios::binary);
    if ( !stream ) return false;
    const auto json = nlohmann::json::parse(stream, nullptr, false);
    if ( json.is_discarded() || !json.is_object() ) return false;

    if ( const auto iterator = json.find("port");
         iterator != json.end() && iterator->is_number_unsigned() ) {
        const auto value = iterator->get<std::uint64_t>();
        if ( value > 65535 ) return false;
        config.port = static_cast<std::uint16_t>(value);
    }
    static_cast<void>(readString(json, "bindAddress", config.bindAddress));
    if ( const auto iterator = json.find("enableTls");
         iterator != json.end() && iterator->is_boolean() ) {
        config.enableTls = iterator->get<bool>();
    }
    static_cast<void>(
        readString(json, "certificatePemFile", config.certificatePemFile));
    static_cast<void>(readString(json, "keyPemFile", config.keyPemFile));
    if ( const auto iterator = json.find("maxRooms");
         iterator != json.end() && iterator->is_number_unsigned() ) {
        config.maxRooms = iterator->get<std::size_t>();
    }
    if ( const auto iterator = json.find("maxClients");
         iterator != json.end() && iterator->is_number_unsigned() ) {
        config.maxClients = iterator->get<std::size_t>();
    }
    if ( const auto iterator = json.find("iceServers");
         iterator != json.end() ) {
        if ( !iterator->is_array() ) return false;
        for ( const auto& value : *iterator ) {
            if ( !value.is_string() ) return false;
            const auto& uri = value.get_ref<const std::string&>();
            if ( uri.empty() || uri.size() > 512 ) return false;
            config.iceServers.push_back(uri);
        }
    } else if ( const auto iterator = json.find("ice");
                iterator != json.end() ) {
        if ( !loadStructuredIceConfig(*iterator, config) ) return false;
    }
    return !config.enableTls ||
           (!config.certificatePemFile.empty() && !config.keyPemFile.empty());
}
}  // namespace

int main(int argc, char** argv)
{
    XLogger::init("MusicMapMaker-CollaborationServer");
    rtcInitLogger(RTC_LOG_WARNING, &logRtcMessage);

    std::string configPath;
    for ( int index = 1; index < argc; ++index ) {
        const std::string_view argument = argv[index] ? argv[index] : "";
        if ( argument == "--config" && index + 1 < argc && argv[index + 1] ) {
            configPath = argv[++index];
        }
    }
    if ( configPath.empty() ) {
        XERROR("Missing required --config <path> argument");
        XLogger::shutdown();
        return 2;
    }

    MMM::Network::CollaborationServer::CollaborationSignalingServerConfig
        config;
    if ( !loadConfig(configPath, config) ) {
        XERROR("Failed to load collaboration signaling config: {}", configPath);
        XLogger::shutdown();
        return 3;
    }

    MMM::Network::CollaborationServer::CollaborationSignalingServer server;
    if ( !server.start(std::move(config)) ) {
        XERROR("Failed to start collaboration signaling server");
        XLogger::shutdown();
        return 4;
    }

    std::signal(SIGINT, &requestExit);
    std::signal(SIGTERM, &requestExit);
    XINFO("Collaboration signaling server listening on port {}",
          server.listeningPort());

    while ( !EXIT_REQUESTED.load(std::memory_order_relaxed) ) {
        server.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    server.stop();
    rtcCleanup();
    XINFO("Collaboration signaling server stopped");
    XLogger::shutdown();
    return 0;
}
