set(CURL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/curl")

set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)

set(CURL_DISABLE_HTTP OFF CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)

# 禁用 ZLIB 等压缩库，防止在 Windows 上产生 zlib1.dll 等动态依赖
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)

if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
elseif(APPLE)
    set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
else()
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${CURL_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/curl_build
    EXCLUDE_FROM_ALL SYSTEM)

add_library(3rd_curl INTERFACE)

if(TARGET libcurl_static)
    target_link_libraries(3rd_curl INTERFACE libcurl_static)
else()
    target_link_libraries(3rd_curl INTERFACE libcurl)
endif()

if(WIN32)
    target_link_libraries(3rd_curl INTERFACE ws2_32 crypt32 bcrypt)
    if(MINGW OR (NOT MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        target_link_libraries(3rd_curl INTERFACE unistring iconv)
    endif()
endif()

if(APPLE)
    target_link_libraries(3rd_curl INTERFACE
        "-framework CoreFoundation"
        "-framework SystemConfiguration"
        "-framework Security")
endif()

if(UNIX AND NOT APPLE)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(3rd_curl INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()
