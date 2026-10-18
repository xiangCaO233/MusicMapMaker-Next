# 3rdpty/CMakeLists.txt

# SOURCES_BUILD 是显式依赖来源开关：ON 使用源码构建，OFF 只使用预编译库。
if(SOURCES_BUILD)
  add_subdirectory(sources)
else()
  # * 预编译模块目录只在 OFF 分支登记，不能遮蔽第三方源码的 Find 脚本。
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
  # * 纯头依赖也来自打包目录，头与二进制具有相同来源约束。
  find_package(stb REQUIRED)
  find_package(clay REQUIRED)
  find_package(sol2 REQUIRED)
  find_package(concurrentqueue REQUIRED)
  find_package(glm REQUIRED)
  find_package(EnTT REQUIRED)
  find_package(nlohmann_json REQUIRED)
  # * 字体和窗口库先导入，后续 UI 包才能引用已定义的目标。
  find_package(freetype REQUIRED)
  find_package(luajit REQUIRED)
  find_package(glfw REQUIRED)
  # * UI 扩展与 ImGui 使用同一构建配置，避免结构布局不一致。
  find_package(imgui REQUIRED)
  find_package(implot REQUIRED)
  find_package(ImGuiFileDialog REQUIRED)
  # * 对话框是平台库，不能拿宿主平台产物替代交叉编译产物。
  find_package(nfd REQUIRED)
  find_package(miniz REQUIRED)
  # 非 ZIP 归档使用内置读取器，缺失预编译包必须在配置阶段报告。
  find_package(libarchive REQUIRED)
  # * 矢量渲染的传递库由专用 Find 模块显式声明。
  find_package(lunasvg REQUIRED)
  # * 网络库的系统 TLS 依赖由各自包装维护，业务层不判断来源。
  find_package(curl REQUIRED)
  find_package(mbedtls REQUIRED)
  # * WebRTC 辅助库保持独立包，不能仅导入 libdatachannel 即省略校验。
  find_package(libjuice REQUIRED)
  find_package(usrsctp REQUIRED)
  find_package(libdatachannel REQUIRED)
  # * 日志格式化库的编译配置须一致，尤其是 MSVC CRT。
  find_package(fmt REQUIRED)
  find_package(spdlog REQUIRED)
  # * 音频基础库先于引擎导入，确保接口目标已可解析。
  find_package(OpenAL REQUIRED)
  # * 历史 target 名不代表当前实际链接偏好，Find 模块负责选择布局。
  find_package(SDL3Static REQUIRED)
  find_package(libsamplerate REQUIRED)
  # * FFTW 的系统线程需求继续由对应模块传播。
  find_package(fftw REQUIRED)
  if(PROJECT_LINKAGE STREQUAL "static")
    # 静态 FFmpeg 预编译包需要显式传播 LAME 与 zlib；shared 包由 FFmpeg DLL 自身声明运行时依赖。
    find_package(lame REQUIRED)
    find_package(zlib REQUIRED)
  endif()
  # * 拉伸器与解码器接口不感知是否启用了归档容器读取。
  find_package(rubberband REQUIRED)
  find_package(ffmpeg REQUIRED)
  # * 引擎最后聚合其依赖，不使用缺包时回退源码的隐式逻辑。
  find_package(IonCachyEngine REQUIRED)
endif()
