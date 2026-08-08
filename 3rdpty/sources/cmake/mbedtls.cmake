# Mbed TLS 作为 libdatachannel 的统一 TLS 后端，只构建静态库并关闭工具与测试。
set(ENABLE_PROGRAMS
    OFF
    CACHE BOOL "Disable Mbed TLS programs in the project dependency build."
          FORCE)
set(ENABLE_TESTING
    OFF
    CACHE BOOL "Disable Mbed TLS tests in the project dependency build." FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY
    ON
    CACHE BOOL "Build static Mbed TLS libraries." FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY
    OFF
    CACHE BOOL "Disable shared Mbed TLS libraries." FORCE)
set(MBEDTLS_FATAL_WARNINGS
    OFF
    CACHE BOOL "Do not promote third-party Mbed TLS warnings to errors." FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL
    ON
    CACHE BOOL "Disable Mbed TLS install targets in the parent project." FORCE)
set(MBEDTLS_USER_CONFIG_FILE
    "${CMAKE_CURRENT_LIST_DIR}/mbedtls-user-config.h"
    CACHE FILEPATH "Project Mbed TLS features required by libdatachannel."
          FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/mbedtls SYSTEM)

if(NOT TARGET MbedTLS::MbedTLS)
  # libdatachannel 的 install export 把该名称视为外部依赖；包装为 IMPORTED target 可避免要求源码 Mbed
  # TLS 进入同一 export set。
  add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
  set_target_properties(MbedTLS::MbedTLS PROPERTIES INTERFACE_LINK_LIBRARIES
                                                    MbedTLS::mbedtls)
endif()

if(NOT TARGET 3rd_mbedtls)
  add_library(3rd_mbedtls INTERFACE)
  target_link_libraries(3rd_mbedtls INTERFACE MbedTLS::mbedtls)
endif()
