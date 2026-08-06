# 导入 Mbed TLS 三个静态库，并导出源码构建同名包装目标：3rd_mbedtls。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_mbedtls_include_dir mbedtls)
prebuilt_target_configs(_mbedtls_configs)

if(NOT TARGET 3rd_mbedtls_everest_library)
  add_library(3rd_mbedtls_everest_library UNKNOWN IMPORTED GLOBAL)
  set(_mbedtls_everest_imported_configs "")
  set(_mbedtls_everest_default_library "")
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    prebuilt_find_library(_mbedtls_everest_library mbedtls "${_mbedtls_config}"
                          everest)
    list(APPEND _mbedtls_everest_imported_configs "${_mbedtls_config_upper}")
    set_target_properties(
      3rd_mbedtls_everest_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedtls_everest_library}")
    if(_mbedtls_everest_default_library STREQUAL "")
      set(_mbedtls_everest_default_library "${_mbedtls_everest_library}")
    endif()
  endforeach()
  set_target_properties(
    3rd_mbedtls_everest_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedtls_everest_imported_configs}"
               IMPORTED_LOCATION "${_mbedtls_everest_default_library}")
endif()

if(NOT TARGET 3rd_mbedtls_p256m_library)
  add_library(3rd_mbedtls_p256m_library UNKNOWN IMPORTED GLOBAL)
  set(_mbedtls_p256m_imported_configs "")
  set(_mbedtls_p256m_default_library "")
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    prebuilt_find_library(_mbedtls_p256m_library mbedtls "${_mbedtls_config}"
                          p256m)
    list(APPEND _mbedtls_p256m_imported_configs "${_mbedtls_config_upper}")
    set_target_properties(
      3rd_mbedtls_p256m_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedtls_p256m_library}")
    if(_mbedtls_p256m_default_library STREQUAL "")
      set(_mbedtls_p256m_default_library "${_mbedtls_p256m_library}")
    endif()
  endforeach()
  set_target_properties(
    3rd_mbedtls_p256m_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedtls_p256m_imported_configs}"
               IMPORTED_LOCATION "${_mbedtls_p256m_default_library}")
endif()

if(NOT TARGET 3rd_mbedcrypto_library)
  add_library(3rd_mbedcrypto_library UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    3rd_mbedcrypto_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                      "${_mbedtls_include_dir}")
  set(_mbedcrypto_imported_configs "")
  set(_mbedcrypto_default_library "")
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    prebuilt_find_library(_mbedcrypto_library mbedtls "${_mbedtls_config}"
                          mbedcrypto)
    list(APPEND _mbedcrypto_imported_configs "${_mbedtls_config_upper}")
    set_target_properties(
      3rd_mbedcrypto_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedcrypto_library}")
    if(_mbedcrypto_default_library STREQUAL "")
      set(_mbedcrypto_default_library "${_mbedcrypto_library}")
    endif()
  endforeach()
  set_target_properties(
    3rd_mbedcrypto_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedcrypto_imported_configs}"
               IMPORTED_LOCATION "${_mbedcrypto_default_library}"
               INTERFACE_LINK_LIBRARIES
               "3rd_mbedtls_everest_library;3rd_mbedtls_p256m_library")
endif()

if(NOT TARGET 3rd_mbedx509_library)
  add_library(3rd_mbedx509_library UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    3rd_mbedx509_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                    "${_mbedtls_include_dir}")
  set(_mbedx509_imported_configs "")
  set(_mbedx509_default_library "")
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    prebuilt_find_library(_mbedx509_library mbedtls "${_mbedtls_config}"
                          mbedx509)
    list(APPEND _mbedx509_imported_configs "${_mbedtls_config_upper}")
    set_target_properties(
      3rd_mbedx509_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedx509_library}")
    if(_mbedx509_default_library STREQUAL "")
      set(_mbedx509_default_library "${_mbedx509_library}")
    endif()
  endforeach()
  set_target_properties(
    3rd_mbedx509_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedx509_imported_configs}"
               IMPORTED_LOCATION "${_mbedx509_default_library}"
               INTERFACE_LINK_LIBRARIES 3rd_mbedcrypto_library)
endif()

if(NOT TARGET 3rd_mbedtls_library)
  add_library(3rd_mbedtls_library UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    3rd_mbedtls_library PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                   "${_mbedtls_include_dir}")
  set(_mbedtls_imported_configs "")
  set(_mbedtls_default_library "")
  foreach(_mbedtls_config IN LISTS _mbedtls_configs)
    string(TOUPPER "${_mbedtls_config}" _mbedtls_config_upper)
    prebuilt_find_library(_mbedtls_library mbedtls "${_mbedtls_config}" mbedtls)
    list(APPEND _mbedtls_imported_configs "${_mbedtls_config_upper}")
    set_target_properties(
      3rd_mbedtls_library
      PROPERTIES "IMPORTED_LOCATION_${_mbedtls_config_upper}"
                 "${_mbedtls_library}")
    if(_mbedtls_default_library STREQUAL "")
      set(_mbedtls_default_library "${_mbedtls_library}")
    endif()
  endforeach()
  set_target_properties(
    3rd_mbedtls_library
    PROPERTIES IMPORTED_CONFIGURATIONS "${_mbedtls_imported_configs}"
               IMPORTED_LOCATION "${_mbedtls_default_library}"
               INTERFACE_LINK_LIBRARIES 3rd_mbedx509_library)
endif()

if(NOT TARGET 3rd_mbedtls)
  add_library(3rd_mbedtls INTERFACE)
  target_link_libraries(3rd_mbedtls INTERFACE 3rd_mbedtls_library)
  target_compile_definitions(
    3rd_mbedtls INTERFACE MBEDTLS_USER_CONFIG_FILE="mbedtls-user-config.h")
  if(WIN32)
    target_link_libraries(3rd_mbedtls INTERFACE ws2_32 bcrypt)
  endif()
endif()

set(mbedtls_FOUND TRUE)
