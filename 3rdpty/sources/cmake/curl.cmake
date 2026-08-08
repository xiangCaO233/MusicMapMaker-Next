set(CURL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/curl")

# 禁止其他依赖的同名选项泄漏到 curl，TLS 后端仅由下方 CURL_USE_* 选项决定。
set(USE_MBEDTLS OFF)
set(USE_GNUTLS OFF)

set(BUILD_CURL_EXE
    OFF
    CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS
    ON
    CACHE BOOL "" FORCE)
if(PROJECT_LINKAGE STREQUAL "shared")
  set(_curl_build_shared ON)
else()
  set(_curl_build_shared OFF)
endif()
set(BUILD_SHARED_LIBS
    ${_curl_build_shared}
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
set(CURL_USE_LIBIDN2
    OFF
    CACHE BOOL "" FORCE)
set(USE_LIBIDN2
    OFF
    CACHE BOOL "" FORCE)
set(USE_NGHTTP2
    OFF
    CACHE BOOL "" FORCE)

set(CURL_DISABLE_HTTP
    OFF
    CACHE BOOL "" FORCE)
set(HTTP_ONLY
    ON
    CACHE BOOL "" FORCE)

# 禁用 ZLIB 等压缩库，防止在 Windows 上产生 zlib1.dll 等动态依赖
set(CURL_ZLIB
    OFF
    CACHE BOOL "" FORCE)
set(CURL_BROTLI
    OFF
    CACHE BOOL "" FORCE)
set(CURL_ZSTD
    OFF
    CACHE BOOL "" FORCE)

if(WIN32)
  set(CURL_USE_OPENSSL
      OFF
      CACHE BOOL "" FORCE)
  set(CURL_USE_SCHANNEL
      ON
      CACHE BOOL "" FORCE)
elseif(APPLE)
  # macOS 预编译包固定使用系统 Secure Transport，禁止拾取 Homebrew OpenSSL。
  set(CURL_USE_OPENSSL
      OFF
      CACHE BOOL "" FORCE)
  set(CURL_USE_SECTRANSP
      ON
      CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL
      ON
      CACHE BOOL "" FORCE)
endif()

add_subdirectory(${CURL_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/curl_build
                 EXCLUDE_FROM_ALL SYSTEM)

add_library(3rd_curl INTERFACE)

if(PROJECT_LINKAGE STREQUAL "shared" AND TARGET libcurl)
  target_link_libraries(3rd_curl INTERFACE libcurl)
elseif(TARGET libcurl_static)
  target_link_libraries(3rd_curl INTERFACE libcurl_static)
else()
  target_link_libraries(3rd_curl INTERFACE libcurl)
endif()

if(WIN32)
  target_link_libraries(3rd_curl INTERFACE ws2_32 crypt32 bcrypt)
endif()

if(APPLE)
  target_link_libraries(
    3rd_curl
    INTERFACE "-framework CoreFoundation" "-framework SystemConfiguration"
              "-framework Security" "-framework CoreServices")
endif()

if(UNIX AND NOT APPLE)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(3rd_curl INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()
