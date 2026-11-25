# Mbed TLS 作为 libdatachannel 的统一 TLS 后端，只构建静态库并关闭工具与测试。
# 协作功能只使用库目标，上游命令行工具不会随主程序分发。
set(ENABLE_PROGRAMS
    OFF
    CACHE BOOL "Disable Mbed TLS programs in the project dependency build."
          FORCE)
# 第三方测试有自己的运行环境，关闭后不把它们和项目回归混在一起。
set(ENABLE_TESTING
    OFF
    CACHE BOOL "Disable Mbed TLS tests in the project dependency build." FORCE)
# libdatachannel 静态库通过可传递接口解析 TLS 符号。
set(USE_STATIC_MBEDTLS_LIBRARY
    ON
    CACHE BOOL "Build static Mbed TLS libraries." FORCE)
# 静态闭包不需要额外打包 TLS 运行动态库。
set(USE_SHARED_MBEDTLS_LIBRARY
    OFF
    CACHE BOOL "Disable shared Mbed TLS libraries." FORCE)
# 上游告警不应因项目选用不同工具链而转成配置阻塞。
set(MBEDTLS_FATAL_WARNINGS
    OFF
    CACHE BOOL "Do not promote third-party Mbed TLS warnings to errors." FORCE)
# 预编译目录由父工程定义，上游安装规则不要产生第二个依赖入口。
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL
    ON
    CACHE BOOL "Disable Mbed TLS install targets in the parent project." FORCE)
# 固定双方启用的功能集合，头宏与归档实际编译出的能力需要一致。
set(MBEDTLS_USER_CONFIG_FILE
    "${CMAKE_CURRENT_LIST_DIR}/mbedtls-user-config.h"
    CACHE FILEPATH "Project Mbed TLS features required by libdatachannel."
          FORCE)

# 目标仍由上游创建，项目只设定使用要求与产物边界。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/mbedtls SYSTEM)

if(NOT TARGET MbedTLS::MbedTLS)
  # libdatachannel 的 install export 把该名称视为外部依赖；包装为 IMPORTED target 可避免要求源码 Mbed
  # TLS 进入同一 export set。 仅给上游 install export 提供可见接口，父项目不把包装库加入其导出集合。
  add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
  # 此接口只转发 TLS 库及其依赖，不复制对象或制造第二套 TLS 全局状态。
  set_target_properties(MbedTLS::MbedTLS PROPERTIES INTERFACE_LINK_LIBRARIES
                                                    MbedTLS::mbedtls)
endif()

# 业务依赖使用独立包装名，上游导出别名的兼容处理由上一层负责。
if(NOT TARGET 3rd_mbedtls)
  # 包装目标没有实现对象，避免同一组 TLS 符号被重复编入。
  add_library(3rd_mbedtls INTERFACE)
  # 显式链接 TLS 目标，使证书与密码组件通过其使用要求进入最终链接。
  target_link_libraries(3rd_mbedtls INTERFACE MbedTLS::mbedtls)
endif()
