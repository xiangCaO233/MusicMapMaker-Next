# libdatachannel 仅承担可靠有序 DataChannel 与 WebSocket 信令，不编译媒体传输和示例测试。
# 这些上游选项名称较通用，必须限制在子目录作用域内，避免污染 curl 等其他依赖的重新配置。
block(SCOPE_FOR VARIABLES)
# 静态传输目标不需要在运行时另外解析第三方依赖 DLL。
set(BUILD_SHARED_DEPS_LIBS OFF)
set(USE_GNUTLS OFF)
# 使用项目打包的 Mbed TLS 后端，避免不同机器凭系统包选到不同 TLS 实现。
set(USE_MBEDTLS ON)
# 候选地址协商继续走 libjuice，保持与预编译依赖集合一致。
set(USE_NICE OFF)
# 统一依赖来源，Runner 之外碰巧安装的同名库不应改写预编译布局。
set(PREFER_SYSTEM_LIB OFF)
set(NO_WEBSOCKET OFF)
set(NO_MEDIA ON)
set(NO_EXAMPLES ON)
set(NO_TESTS ON)
set(WARNINGS_AS_ERRORS OFF)
# 禁止配置过程写回第三方版本头，保留源码树可复用。
set(RTC_UPDATE_VERSION_HEADER OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libdatachannel SYSTEM)

# mbedTLS 通过 C 回调调用 libdatachannel 的 C++ 传输层。Windows 默认的 /EHsc 隐含
# /EHc，会错误假定该边界绝不抛出异常并省略必要的栈展开状态；断线竞争下异常将无法到达 TlsTransport::doRecv 的保护边界。仅为
# libdatachannel 取消该假定，保留标准 C++ 异常模型。
if(MSVC)
  target_compile_options(datachannel PRIVATE /EHs /EHc-)
  target_compile_options(datachannel-static PRIVATE /EHs /EHc-)
endif()

# usrsctp 的 Windows C 实现仍使用 Win32 min/max 宏；MinGW 严格 C99 编译时需覆盖项目全局 NOMINMAX。
if(MINGW AND TARGET usrsctp)
  target_compile_options(usrsctp PRIVATE -UNOMINMAX)
endif()
endblock()

# 包装接口选择静态上游目标并保持源码与预编译构建的消费者入口一致。
if(NOT TARGET 3rd_libdatachannel)
  add_library(3rd_libdatachannel INTERFACE)
  target_link_libraries(3rd_libdatachannel
                        INTERFACE LibDataChannel::LibDataChannelStatic)
endif()
