# 导入 OpenAL 预编译库，导出源码构建同名目标：OpenAL::OpenAL。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_openal_include_dir openal)
set(_openal_include_dirs "${_openal_include_dir}" "${_openal_include_dir}/AL")

if(NOT TARGET OpenAL::OpenAL)
  add_library(OpenAL::OpenAL UNKNOWN IMPORTED GLOBAL)
  set_target_properties(OpenAL::OpenAL PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_openal_include_dirs}")

  prebuilt_target_configs(_openal_configs)
  set(_openal_imported_configs "")
  set(_openal_default_library "")
  foreach(_openal_config IN LISTS _openal_configs)
    string(TOUPPER "${_openal_config}" _openal_config_upper)
    prebuilt_find_library(
      _openal_library
      openal
      "${_openal_config}"
      OpenAL
      OpenAL32
      openal
      soft_oal)
    list(APPEND _openal_imported_configs "${_openal_config_upper}")
    set_target_properties(
      OpenAL::OpenAL PROPERTIES "IMPORTED_LOCATION_${_openal_config_upper}"
                                "${_openal_library}")
    if(_openal_default_library STREQUAL "")
      set(_openal_default_library "${_openal_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      OpenAL::OpenAL PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_openal_default_library}")
  endif()
  set_target_properties(
    OpenAL::OpenAL
    PROPERTIES IMPORTED_CONFIGURATIONS "${_openal_imported_configs}"
               IMPORTED_LOCATION "${_openal_default_library}")

  if(PROJECT_LINKAGE STREQUAL "static")
    # OpenAL 头文件默认按 DLL 导入声明符号；静态预编译库必须显式关闭 dllimport，否则 MinGW 会查找 __imp_al*
    # 导入符号。
    target_compile_definitions(OpenAL::OpenAL
                               INTERFACE AL_LIBTYPE_STATIC
                                         ICE_OPENALSOFT_STATIC_LINKAGE)
  endif()

  if(WIN32)
    target_link_libraries(OpenAL::OpenAL INTERFACE winmm ole32 avrt)
  elseif(APPLE)
    target_link_libraries(
      OpenAL::OpenAL INTERFACE "-framework CoreAudio" "-framework AudioToolbox"
                               "-framework AudioUnit")
  else()
    target_link_libraries(OpenAL::OpenAL INTERFACE dl)
  endif()
endif()

set(OpenAL_FOUND TRUE)
