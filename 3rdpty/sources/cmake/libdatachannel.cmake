# libdatachannel 仅承担可靠有序 DataChannel 与 WebSocket 信令，不编译媒体传输和示例测试。
# 这些上游选项名称较通用，必须限制在子目录作用域内，避免污染 curl 等其他依赖的重新配置。
block(SCOPE_FOR VARIABLES)
set(BUILD_SHARED_DEPS_LIBS OFF)
set(USE_GNUTLS OFF)
set(USE_MBEDTLS ON)
set(USE_NICE OFF)
set(PREFER_SYSTEM_LIB OFF)
set(NO_WEBSOCKET OFF)
set(NO_MEDIA ON)
set(NO_EXAMPLES ON)
set(NO_TESTS ON)
set(WARNINGS_AS_ERRORS OFF)
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

if(NOT TARGET 3rd_libdatachannel)
  add_library(3rd_libdatachannel INTERFACE)
  target_link_libraries(3rd_libdatachannel
                        INTERFACE LibDataChannel::LibDataChannelStatic)
endif()
