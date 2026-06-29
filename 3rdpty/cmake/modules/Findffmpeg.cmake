# 导入 FFmpeg 预编译组件库，并聚合为源码构建同名目标：3rd_ffmpeg。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
if(PROJECT_LINKAGE STREQUAL "static")
  find_package(lame REQUIRED)
  find_package(zlib REQUIRED)
endif()
prebuilt_include_dir(_ffmpeg_include_dir ffmpeg)

foreach(_ffmpeg_component avformat avcodec swscale swresample avutil)
  if(NOT TARGET FFmpeg::${_ffmpeg_component})
    add_library(FFmpeg::${_ffmpeg_component} UNKNOWN IMPORTED GLOBAL)
    set_target_properties(
      FFmpeg::${_ffmpeg_component} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_ffmpeg_include_dir}")

    prebuilt_target_configs(_ffmpeg_configs)
    set(_ffmpeg_imported_configs "")
    set(_ffmpeg_default_library "")
    foreach(_ffmpeg_config IN LISTS _ffmpeg_configs)
      string(TOUPPER "${_ffmpeg_config}" _ffmpeg_config_upper)
      prebuilt_find_library(_ffmpeg_library ffmpeg "${_ffmpeg_config}"
                            ${_ffmpeg_component} lib${_ffmpeg_component})
      list(APPEND _ffmpeg_imported_configs "${_ffmpeg_config_upper}")
      set_target_properties(
        FFmpeg::${_ffmpeg_component}
        PROPERTIES "IMPORTED_LOCATION_${_ffmpeg_config_upper}"
                   "${_ffmpeg_library}")
      if(_ffmpeg_default_library STREQUAL "")
        set(_ffmpeg_default_library "${_ffmpeg_library}")
      endif()
    endforeach()
    if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
      set_target_properties(
        FFmpeg::${_ffmpeg_component} PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                "${_ffmpeg_default_library}")
    endif()
    set_target_properties(
      FFmpeg::${_ffmpeg_component}
      PROPERTIES IMPORTED_CONFIGURATIONS "${_ffmpeg_imported_configs}"
                 IMPORTED_LOCATION "${_ffmpeg_default_library}")
  endif()
endforeach()

if(NOT TARGET 3rd_ffmpeg)
  add_library(3rd_ffmpeg INTERFACE)
  target_link_libraries(
    3rd_ffmpeg
    INTERFACE FFmpeg::avformat
              FFmpeg::avcodec
              FFmpeg::swscale
              FFmpeg::swresample
              FFmpeg::avutil)
  if(PROJECT_LINKAGE STREQUAL "static")
    target_link_libraries(3rd_ffmpeg INTERFACE 3rd_lame 3rd_zlib)
  endif()
  if(WIN32)
    target_link_libraries(
      3rd_ffmpeg
      INTERFACE bcrypt
                user32
                ole32
                strmiids
                uuid
                ws2_32
                secur32
                ncrypt
                crypt32
                advapi32
                shell32
                vfw32
                mfuuid)
  elseif(APPLE)
    target_link_libraries(
      3rd_ffmpeg
      INTERFACE "-framework CoreFoundation"
                "-framework CoreVideo"
                "-framework CoreMedia"
                "-framework AudioToolbox"
                "-framework VideoToolbox"
                "-framework Security"
                bz2
                m
                iconv)
  else()
    target_link_libraries(3rd_ffmpeg INTERFACE m pthread lzma bz2 dl)
  endif()
endif()

set(ffmpeg_FOUND TRUE)
