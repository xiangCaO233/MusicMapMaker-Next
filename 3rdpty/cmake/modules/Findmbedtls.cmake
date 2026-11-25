# 导入 Mbed TLS 三个静态库，并导出源码构建同名包装目标：3rd_mbedtls。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 证书、握手和密码学组件按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 共用头记录 TLS 与加密功能集合，各组件必须来自同一个工具链与配置布局。
prebuilt_include_dir(_mbedtls_include_dir mbedtls)
# 根据生成器启用的配置验证同一配置各组件归档，避免切换配置时才发现分发遗漏。
prebuilt_target_configs(_mbedtls_configs)

# * Everest 是密码实现中的辅助归档，不作为业务公开入口。
# * 与 mbedcrypto 一起链接，确保协作 TLS 看见相同密码后端。
# * 如大系统已提供同名目标，保留其绑定，避免两个实现进入闭包。
if(NOT TARGET 3rd_mbedtls_everest_library)
  # 将证书、握手和密码学组件作为全局导入发布，兄弟目录使用同一选择。
  add_library(3rd_mbedtls_everest_library UNKNOWN IMPORTED GLOBAL)
  # 记录实际完成解析的证书、握手和密码学组件配置，以便给缺失产物报错。
  set(_mbedtls_everest_imported_configs "")
  # 协作 TLS 传输可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_mbedtls_everest_default_library "")
  # 证书、握手和密码学组件每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    # 3rd_mbedtls_everest_library 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    # 只在已选工具链和配置目录尝试同一配置各组件归档，不会命中系统安装。
    prebuilt_find_library(_mbedtls_everest_library mbedtls "${_mbedtls_config}"
                          everest)
    # 处理成功才登记 3rd_mbedtls_everest_library 当前配置，未知产物不产生空导入条目。
    list(APPEND _mbedtls_everest_imported_configs "${_mbedtls_config_upper}")
    # 记录 3rd_mbedtls_everest_library 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_mbedtls_everest_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedtls_everest_library}")
    # 3rd_mbedtls_everest_library 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_mbedtls_everest_default_library STREQUAL "")
      set(_mbedtls_everest_default_library "${_mbedtls_everest_library}")
    endif()
  endforeach()
  # 将 3rd_mbedtls_everest_library 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_mbedtls_everest_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedtls_everest_imported_configs}"
               IMPORTED_LOCATION "${_mbedtls_everest_default_library}")
endif()

# * P-256 辅助实现单独打包；仅当上游未提供对应目标时由此导入。
# * 查找在 mbedtls 包内完成，不能拾取同名系统归档。
# * 辅助库不发布自己的包含目录，公开密码接口仍属于 mbedcrypto。
if(NOT TARGET 3rd_mbedtls_p256m_library)
  add_library(3rd_mbedtls_p256m_library UNKNOWN IMPORTED GLOBAL)
  set(_mbedtls_p256m_imported_configs "")
  set(_mbedtls_p256m_default_library "")
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    # 3rd_mbedtls_p256m_library 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    prebuilt_find_library(_mbedtls_p256m_library mbedtls "${_mbedtls_config}"
                          p256m)
    # 处理成功才登记 3rd_mbedtls_p256m_library 当前配置，未知产物不产生空导入条目。
    list(APPEND _mbedtls_p256m_imported_configs "${_mbedtls_config_upper}")
    # 记录 3rd_mbedtls_p256m_library 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_mbedtls_p256m_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedtls_p256m_library}")
    # 3rd_mbedtls_p256m_library 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_mbedtls_p256m_default_library STREQUAL "")
      set(_mbedtls_p256m_default_library "${_mbedtls_p256m_library}")
    endif()
  endforeach()
  # 将 3rd_mbedtls_p256m_library 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_mbedtls_p256m_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedtls_p256m_imported_configs}"
               IMPORTED_LOCATION "${_mbedtls_p256m_default_library}")
endif()

# * 算法与基础密码接口由 mbedcrypto 提供，并依赖两个辅助归档。
# * mbedx509 通过其公开接口取得证书验证所需算法。
# * 将辅助依赖保留在此导入层，最终应用不手工调整三个归档的顺序。
if(NOT TARGET 3rd_mbedcrypto_library)
  add_library(3rd_mbedcrypto_library UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取固定的 Mbed TLS 功能集合，不必手写额外包含目录。
  set_target_properties(
    3rd_mbedcrypto_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                      "${_mbedtls_include_dir}")
  set(_mbedcrypto_imported_configs "")
  set(_mbedcrypto_default_library "")
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    # 3rd_mbedcrypto_library 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    prebuilt_find_library(_mbedcrypto_library mbedtls "${_mbedtls_config}"
                          mbedcrypto)
    # 处理成功才登记 3rd_mbedcrypto_library 当前配置，未知产物不产生空导入条目。
    list(APPEND _mbedcrypto_imported_configs "${_mbedtls_config_upper}")
    # 记录 3rd_mbedcrypto_library 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_mbedcrypto_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedcrypto_library}")
    # 3rd_mbedcrypto_library 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_mbedcrypto_default_library STREQUAL "")
      set(_mbedcrypto_default_library "${_mbedcrypto_library}")
    endif()
  endforeach()
  # 将 3rd_mbedcrypto_library 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_mbedcrypto_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedcrypto_imported_configs}"
               IMPORTED_LOCATION "${_mbedcrypto_default_library}"
               INTERFACE_LINK_LIBRARIES
               "3rd_mbedtls_everest_library;3rd_mbedtls_p256m_library")
endif()

# * X.509 组件负责证书链处理，与 TLS 协议对象分开发布。
# * 验证证书所需密码算法通过 mbedcrypto 传递。
# * 同包公共头确保读取证书和握手控制看到一致的配置宏。
if(NOT TARGET 3rd_mbedx509_library)
  add_library(3rd_mbedx509_library UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    3rd_mbedx509_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                    "${_mbedtls_include_dir}")
  set(_mbedx509_imported_configs "")
  set(_mbedx509_default_library "")
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    # 3rd_mbedx509_library 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    prebuilt_find_library(_mbedx509_library mbedtls "${_mbedtls_config}"
                          mbedx509)
    # 处理成功才登记 3rd_mbedx509_library 当前配置，未知产物不产生空导入条目。
    list(APPEND _mbedx509_imported_configs "${_mbedtls_config_upper}")
    # 记录 3rd_mbedx509_library 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_mbedx509_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedx509_library}")
    # 3rd_mbedx509_library 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_mbedx509_default_library STREQUAL "")
      set(_mbedx509_default_library "${_mbedx509_library}")
    endif()
  endforeach()
  # 将 3rd_mbedx509_library 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_mbedx509_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedx509_imported_configs}"
               IMPORTED_LOCATION "${_mbedx509_default_library}"
               INTERFACE_LINK_LIBRARIES 3rd_mbedcrypto_library)
endif()

# * 顶层协议归档只提供 TLS 会话和握手对象，证书和密码能力来自下层。
# * 先从同一消费配置取得其库文件，再发布传递到 X.509 的依赖。
# * 应用不会因只调用握手入口而漏掉依赖的证书符号。
if(NOT TARGET 3rd_mbedtls_library)
  add_library(3rd_mbedtls_library UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    3rd_mbedtls_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                   "${_mbedtls_include_dir}")
  set(_mbedtls_imported_configs "")
  set(_mbedtls_default_library "")
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    # 3rd_mbedtls_library 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    prebuilt_find_library(_mbedtls_library mbedtls "${_mbedtls_config}" mbedtls)
    # 处理成功才登记 3rd_mbedtls_library 当前配置，未知产物不产生空导入条目。
    list(APPEND _mbedtls_imported_configs "${_mbedtls_config_upper}")
    # 记录 3rd_mbedtls_library 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      3rd_mbedtls_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedtls_library}")
    # 3rd_mbedtls_library 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_mbedtls_default_library STREQUAL "")
      set(_mbedtls_default_library "${_mbedtls_library}")
    endif()
  endforeach()
  # 将 3rd_mbedtls_library 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    3rd_mbedtls_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedtls_imported_configs}"
               IMPORTED_LOCATION "${_mbedtls_default_library}"
               INTERFACE_LINK_LIBRARIES 3rd_mbedx509_library)
endif()

if(NOT TARGET 3rd_mbedtls)
  # 包装接口供协作 TLS 传输获得 TLS、X.509、密码学及辅助算法的闭包，无需在该处重新声明系统库。
  add_library(3rd_mbedtls INTERFACE)
  # 应用最终链接还需底层证书、密码学和辅助归档；Windows 套接字及系统随机接口补在聚合层。
  target_link_libraries(3rd_mbedtls INTERFACE 3rd_mbedtls_library)
  # * 确保传输层看到预编译时固定的 MBEDTLS_USER_CONFIG_FILE。
  # * 头声明中的条件特性必须和归档编译时的功能集一致。
  # * 静态依赖链的最外层传播此声明，而不是为每个业务目标手工复刻。
  target_compile_definitions(
    3rd_mbedtls INTERFACE MBEDTLS_USER_CONFIG_FILE="mbedtls-user-config.h")
  # * Windows 的传输实现使用系统套接字。
  # * 系统随机源通过 BCrypt 接口访问，消费者不从上层模块补这组符号。
  # * 只给包装目标附加这组平台系统依赖，各密码组件可独立定义。
  if(WIN32)
    target_link_libraries(3rd_mbedtls INTERFACE ws2_32 bcrypt)
  endif()
endif()

# 协作 TLS 传输使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(mbedtls_FOUND TRUE)
