# curl 包装入口只负责依赖配置，源码版本由仓库 gitlink 固定。
# 禁止源码模式在不同平台上静默选择不同宿主网络库。
set(CURL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/curl")

# 禁止其他依赖的同名选项泄漏到 curl，TLS 后端仅由下方 CURL_USE_* 选项决定。
set(USE_MBEDTLS OFF)
set(USE_GNUTLS OFF)

# 不构建第三方命令行与自带测试，主项目的网络回归另行运行。
# 原生 Android 的网络请求仍通过相同 C++ curl 接口执行。
set(BUILD_CURL_EXE
    OFF
    CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS
    ON
    CACHE BOOL "" FORCE)
# 依赖链接偏好影响真实库类型，不依赖其他子项目的通用缓存默认值。
# Android 实验构建使用静态 curl，最终并入 libmain.so。
if(PROJECT_LINKAGE STREQUAL "shared")
  set(_curl_build_shared ON)
else()
  set(_curl_build_shared OFF)
endif()
set(BUILD_SHARED_LIBS
    ${_curl_build_shared}
    CACHE BOOL "" FORCE)
# 这些可选协议、工具和压缩库不进入应用依赖闭包，避免平台探测拾取宿主安装。
# 统一强制缓存值，使重新配置不会继承其他子项目留下的同名开关。
# Android 使用同一功能边界，不引入额外 Java 网络框架或系统 OpenSSL。
foreach(
  _option
  BUILD_EXAMPLES
  BUILD_LIBCURL_DOCS
  CURL_USE_LIBSSH2
  CURL_USE_LIBPSL
  CURL_USE_LIBIDN2
  USE_LIBIDN2
  USE_NGHTTP2
  CURL_DISABLE_HTTP
  CURL_ZLIB
  CURL_BROTLI
  CURL_ZSTD)
  set(${_option}
      OFF
      CACHE BOOL "" FORCE)
endforeach()
# 仅生成可链接库，不安装 curl 的头文件和命令行到用户系统目录。
set(CURL_DISABLE_INSTALL
    ON
    CACHE BOOL "" FORCE)
# HTTP 保留 HTTPS 支持，实际 TLS 后端由目标平台决定。
set(HTTP_ONLY
    ON
    CACHE BOOL "" FORCE)

# TLS 提供者必须明确且互斥；移动平台不能沿用 Linux 默认 OpenSSL。
# Windows 与 macOS 继续使用各自的系统证书和平台 TLS 实现。
if(WIN32)
  set(CURL_USE_OPENSSL
      OFF
      CACHE BOOL "" FORCE)
  set(CURL_USE_SCHANNEL
      ON
      CACHE BOOL "" FORCE)
elseif(ANDROID)
  # 目标 Android 使用 Bionic 与 NDK 标准库，不能沿用构建宿主的链接搜索结果。
  # curl 仅通过 JNI 原生库间接使用网络，不增加 Activity 回调和 Java 网络桥。
  # Mbed TLS 来自当前依赖图，证书读取失败仍须由业务返回明确网络错误。
  # 此配置不申请共享存储权限，也不为 TLS 创建系统目录中的 CA 文件。
  # 业务层接口与桌面保持一致，但平台系统证书集成不由本包装隐式承诺。
  # Bionic 没有桌面 OpenSSL；复用已经配置的 Mbed TLS 源码目标。
  set(CURL_USE_OPENSSL
      OFF
      CACHE BOOL "" FORCE)
  # Mbed TLS 与协作网络共用源码目标，不在 APK 携带另一套 OpenSSL。
  # 此处仅选择 TLS 引擎，证书来源仍须由运行环境或业务请求配置。
  set(CURL_USE_MBEDTLS
      ON
      CACHE BOOL "" FORCE)
  # curl 的 FindMbedTLS 入口接受既有源码目标，不从宿主查找同名归档。
  set(MBEDTLS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mbedtls/include")
  # 导入接口阻止 curl 的 export 收录父工程的 Mbed TLS 源码目标。
  # X509 和 Crypto 的依赖边由导入接口转发，实际归档仍由父构建图生成。
  # 不使用 export 安装跨项目的源码 target，避免生成缺失依赖的导出集合。
  # Android 外壳不执行 curl 安装步骤，这些接口只供本轮原生链接。
  add_library(AndroidMbedTLS::X509 INTERFACE IMPORTED GLOBAL)
  set_target_properties(AndroidMbedTLS::X509 PROPERTIES INTERFACE_LINK_LIBRARIES
                                                        MbedTLS::mbedx509)
  add_library(AndroidMbedTLS::Crypto INTERFACE IMPORTED GLOBAL)
  set_target_properties(AndroidMbedTLS::Crypto
                        PROPERTIES INTERFACE_LINK_LIBRARIES MbedTLS::mbedcrypto)
  # 已有 Mbed TLS 目标承担原生依赖的传递链接，不用宿主 find_library 补齐。
  # X509 与 Crypto 使用导入接口，保持 curl 的安装导出检查可通过。
  set(MBEDTLS_LIBRARY MbedTLS::MbedTLS)
  set(MBEDX509_LIBRARY AndroidMbedTLS::X509)
  set(MBEDCRYPTO_LIBRARY AndroidMbedTLS::Crypto)
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

# 上游配置在独立二进制目录运行，生成头文件和缓存不会进入源码树。
# SYSTEM 标记第三方包含边界，不改变项目自身的编译诊断策略。
# 下载和 ABI 选择由外层配置完成，本入口不执行网络克隆或宿主库回退。
# 同一目标平台重配时缓存值保持明确，库类型不会随其他子项目的默认值漂移。
# 源码输入只读，生成的配置头由独立二进制目录承载。
add_subdirectory(${CURL_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/curl_build
                 EXCLUDE_FROM_ALL SYSTEM)

# 业务只消费稳定的包装 target，预编译和源码模式保持同名接口。
# 必须绑定已经创建的真实库，不能通过空接口掩盖缺失依赖。
# 静态消费时由真实 libcurl target 传播私有依赖，包装层不手工猜测全部对象文件。
# shared 模式仍保留原有桌面库选择，Android 脚本显式采用 static 偏好。
# 预编译模式不包含本包装，因此缺少 Android 预编译包不会悄悄进入这里。
add_library(3rd_curl INTERFACE)

if(PROJECT_LINKAGE STREQUAL "shared" AND TARGET libcurl)
  target_link_libraries(3rd_curl INTERFACE libcurl)
elseif(TARGET libcurl_static)
  target_link_libraries(3rd_curl INTERFACE libcurl_static)
else()
  target_link_libraries(3rd_curl INTERFACE libcurl)
endif()

# 系统依赖按目标平台追加，不能把构建宿主的 Unix 库传播给手机。
# Windows 的网络与加密接口仍由系统库实现，不额外打包这些 DLL。
if(WIN32)
  target_link_libraries(3rd_curl INTERFACE ws2_32 crypt32 bcrypt)
endif()

if(APPLE)
  target_link_libraries(
    3rd_curl
    INTERFACE "-framework CoreFoundation" "-framework SystemConfiguration"
              "-framework Security" "-framework CoreServices")
endif()

# Android 也满足 UNIX，但其 TLS 已由上面的 Mbed TLS 接口提供。
# 这里排除 Android，防止配置成功后又把宿主 OpenSSL 混进最终链接。
if(UNIX
   AND NOT APPLE
   AND NOT ANDROID)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(3rd_curl INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()
