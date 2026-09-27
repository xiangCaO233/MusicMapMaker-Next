# 导入 FFmpeg 预编译组件库，并聚合为源码构建同名目标：3rd_ffmpeg。 依赖路径由 PrebuiltLayout
# 限定，不从系统安装或源码目录补齐缺失归档。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 主项目的包根独立于 IonCachyEngine 内部的包根。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 静态 FFmpeg 不携带 LAME 和压缩库实现，最终可执行文件必须显式链接。
if(PROJECT_LINKAGE STREQUAL "static")
  # MP3 编码由 LAME 提供；缺包时应在配置阶段失败。
  find_package(lame REQUIRED)
  # zlib 服务于 FFmpeg 容器与压缩路径，不能只链接 avcodec。
  find_package(zlib REQUIRED)
endif()
# 公共 FFmpeg 头由已发布包提供，不能误用宿主机头文件。
prebuilt_include_dir(_ffmpeg_include_dir ffmpeg)

# 五个组件与源码模式的 3rd_ffmpeg 接口一致，业务模块不区分来源。
foreach(_ffmpeg_component avformat avcodec swscale swresample avutil)
  # 上层若已经导入组件，则保留原目标，不更改其配置映射。
  if(NOT TARGET FFmpeg::${_ffmpeg_component})
    # 每个归档单独导入，防止一个组件的库路径被其他组件复用。
    add_library(FFmpeg::${_ffmpeg_component} UNKNOWN IMPORTED GLOBAL)
    # 头目录通过目标传递，消费者不应自行拼接预编译包路径。
    set_target_properties(
      FFmpeg::${_ffmpeg_component} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_ffmpeg_include_dir}")

    # 多配置生成器要分别解析每个配置，Debug 不可偷用发布归档。
    prebuilt_target_configs(_ffmpeg_configs)
    # 循环变量必须按组件清空，避免跨组件共享默认路径。
    set(_ffmpeg_imported_configs "")
    set(_ffmpeg_default_library "")
    foreach(_ffmpeg_config IN LISTS _ffmpeg_configs)
      # CMake 属性名称使用大写配置，磁盘目录仍由布局 helper 决定。
      string(TOUPPER "${_ffmpeg_config}" _ffmpeg_config_upper)
      # helper 同时验证归档存在，避免把缺包错误延迟到链接阶段。
      prebuilt_find_library(_ffmpeg_library ffmpeg "${_ffmpeg_config}"
                            ${_ffmpeg_component} lib${_ffmpeg_component})
      # 已解析配置才可以登记为 IMPORTED_CONFIGURATIONS。
      list(APPEND _ffmpeg_imported_configs "${_ffmpeg_config_upper}")
      # 每个配置的真实文件路径单独写入，供单配置和多配置生成器使用。
      set_target_properties(
        FFmpeg::${_ffmpeg_component}
        PROPERTIES "IMPORTED_LOCATION_${_ffmpeg_config_upper}"
                   "${_ffmpeg_library}")
      # 通用位置只取首个有效路径，不覆盖已经绑定的配置路径。
      if(_ffmpeg_default_library STREQUAL "")
        set(_ffmpeg_default_library "${_ffmpeg_library}")
      endif()
    endforeach()
    # 未显式指定配置时仍需可解析导入目标。
    if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
      set_target_properties(
        FFmpeg::${_ffmpeg_component} PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                "${_ffmpeg_default_library}")
    endif()
    # 配置清单与通用位置同时发布，避免消费者依赖当前生成器类型。
    set_target_properties(
      FFmpeg::${_ffmpeg_component}
      PROPERTIES IMPORTED_CONFIGURATIONS "${_ffmpeg_imported_configs}"
                 IMPORTED_LOCATION "${_ffmpeg_default_library}")
  endif()
endforeach()

# 各平台新静态 avcodec 均引用外部 Vorbis、Ogg 和 Opus 实现。
if(PROJECT_LINKAGE STREQUAL "static")
  # Vorbis 编码、Vorbis 公共实现、Ogg 封装和 Opus 编码都须进入链接闭包。
  foreach(_xiph_component vorbisenc vorbis ogg opus)
    # 同一目标可能由引擎包先行创建，避免重复定义全局导入目标。
    if(NOT TARGET Xiph::${_xiph_component})
      add_library(Xiph::${_xiph_component} UNKNOWN IMPORTED GLOBAL)
      # Xiph 归档与 FFmpeg 使用完全相同的平台及配置映射。
      prebuilt_target_configs(_xiph_configs)
      # 每个 codec 组件独立累积配置，防止上一个组件的默认归档泄漏。
      set(_xiph_default_library "")
      set(_xiph_imported_configs "")
      foreach(_xiph_config IN LISTS _xiph_configs)
        # CMake 导入属性使用大写配置名；包布局 helper 接收原名。
        string(TOUPPER "${_xiph_config}" _xiph_config_upper)
        # 允许有无 lib 前缀的归档命名，但不允许搜索系统路径。
        prebuilt_find_library(_xiph_library xiph "${_xiph_config}"
                              ${_xiph_component} lib${_xiph_component})
        # 每个配置都登记实际 Xiph 文件，不能借用另一配置的 CRT 或优化档。
        set_target_properties(
          Xiph::${_xiph_component}
          PROPERTIES "IMPORTED_LOCATION_${_xiph_config_upper}"
                     "${_xiph_library}")
        # 未解析的配置不可对消费者宣称可用。
        list(APPEND _xiph_imported_configs "${_xiph_config_upper}")
        # 第一个配置提供无配置生成器的回退位置。
        if(_xiph_default_library STREQUAL "")
          set(_xiph_default_library "${_xiph_library}")
        endif()
      endforeach()
      # 保留完整配置集合，使 Debug 和 RelWithDebInfo 均选中同构产物。
      set_target_properties(
        Xiph::${_xiph_component}
        PROPERTIES IMPORTED_CONFIGURATIONS "${_xiph_imported_configs}"
                   IMPORTED_LOCATION "${_xiph_default_library}")
    endif()
  endforeach()
endif()

if(NOT TARGET 3rd_ffmpeg)
  # 聚合目标只传播依赖，不再次封装或复制 FFmpeg 二进制。
  add_library(3rd_ffmpeg INTERFACE)
  # 顺序从容器、编码器到基础工具，便于静态链接器解析符号。
  target_link_libraries(
    3rd_ffmpeg
    INTERFACE FFmpeg::avformat
              # avcodec 持有实际音频编码入口，必须早于外部 codec 归档。
              FFmpeg::avcodec
              # 封面和视频仍消费像素转换组件。
              FFmpeg::swscale
              # 音频导出路径消费重采样组件。
              FFmpeg::swresample
              # 基础工具被其他四个组件共同引用。
              FFmpeg::avutil)
  if(PROJECT_LINKAGE STREQUAL "static")
    # 新 avcodec 引用外部编码归档，依赖必须排在 avcodec 后面。
    target_link_libraries(3rd_ffmpeg INTERFACE Xiph::vorbisenc Xiph::vorbis
                                               Xiph::ogg Xiph::opus)
    # LAME 和 zlib 仍属于所有静态平台，不与 Xiph 条件绑定。
    target_link_libraries(3rd_ffmpeg INTERFACE 3rd_lame 3rd_zlib)
  endif()
  # 系统库是 FFmpeg 包 ABI 的一部分，按目标平台分别展开。
  if(WIN32)
    # 这些库为 Windows 媒体、网络与安全 API 提供最终符号。
    target_link_libraries(
      3rd_ffmpeg
      INTERFACE bcrypt
                # 图形与媒体接口使用 Windows 用户态句柄。
                user32
                ole32
                # DirectShow 的接口标识符由平台库定义。
                strmiids
                uuid
                # FFmpeg 的网络协议支持需要 Winsock。
                ws2_32
                secur32
                # TLS 和证书路径由系统安全库提供。
                ncrypt
                crypt32
                advapi32
                shell32
                # 视频设备与 Media Foundation 的符号来自系统 SDK。
                vfw32
                mfuuid)
  elseif(APPLE)
    # Apple 平台解码路径依赖系统媒体框架及字符编码库。
    target_link_libraries(
      3rd_ffmpeg
      INTERFACE "-framework CoreFoundation"
                # 视频像素和时间戳类型分别由两个媒体框架提供。
                "-framework CoreVideo"
                "-framework CoreMedia"
                # 音频处理与视频硬件路径使用不同的系统框架。
                "-framework AudioToolbox"
                "-framework VideoToolbox"
                "-framework Security"
                # FFmpeg 归档中的压缩和字符集路径保留独立系统依赖。
                bz2
                m
                iconv)
  else()
    # Linux 静态构建需要数学、线程、压缩和动态加载实现。
    target_link_libraries(3rd_ffmpeg INTERFACE m pthread lzma bz2 dl)
  endif()
endif()

# 只有上面所有必需归档都成功导入后才声明包可用。
set(ffmpeg_FOUND TRUE)
