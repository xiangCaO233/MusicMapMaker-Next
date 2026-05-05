set(CURL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/curl")

# 禁用一切不需要的特性，只保留 HTTP/HTTPS
set(BUILD_CURL_EXE
    OFF
    CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS
    ON
    CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES
    OFF
    CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL
    ON
    CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS
    OFF
    CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2
    OFF
    CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL
    OFF
    CACHE BOOL "" FORCE)
set(USE_NGHTTP2
    OFF
    CACHE BOOL "" FORCE)

# 仅需 HTTP/HTTPS 协议
set(CURL_DISABLE_HTTP
    OFF
    CACHE BOOL "" FORCE)
set(HTTP_ONLY
    ON
    CACHE BOOL "" FORCE)

# 启用 SSL（使用系统 OpenSSL/SSL 后端）
set(CURL_ENABLE_SSL
    ON
    CACHE BOOL "" FORCE)

add_subdirectory(${CURL_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/curl_build
                 EXCLUDE_FROM_ALL SYSTEM)

add_library(3rd_curl INTERFACE)
target_link_libraries(3rd_curl INTERFACE libcurl_static)

# curl 的静态库需要这些系统链接
if(WIN32)
    target_link_libraries(3rd_curl INTERFACE ws2_32 crypt32 bcrypt)
elseif(APPLE)
    target_link_libraries(3rd_curl INTERFACE "-framework CoreFoundation" "-framework SystemConfiguration")
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(3rd_curl INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()
