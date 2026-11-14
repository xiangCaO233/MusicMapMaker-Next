# Modules/Game/Graphic/Font/CMakeLists.txt

# 字体模块只负责把字体来源解析和 CPU 侧栅格化能力封装为静态库； Vulkan 图集上传仍由 Graphic 模块完成，避免 Font 反向依赖图形后端。
# 三个实现共享同一公共接口目录，但平台字体解析在预处理阶段只编译当前分支。
add_library(Font STATIC src/FontPool.cpp src/SystemFontResolver.cpp
                        src/AsciiFontRasterizer.cpp)

# 公开目录只暴露与字体数据和解析结果有关的轻量接口。
target_include_directories(Font PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Common 提供跨模块字体度量；日志与 FreeType 仅在实现内部使用。
target_link_libraries(
  Font
  PUBLIC Common
  PRIVATE Log freetype::freetype)

# 独立验证软件 ASCII 字体能被栅格化为画布图集数据。
mmm_add_test_executable(Graphic AsciiFontRasterizerTest
                        tests/AsciiFontRasterizerTest.cpp)
# 测试复用生产字体库，并借助 Config 完成 UTF-8 测试路径转换。
target_link_libraries(AsciiFontRasterizerTest PRIVATE Font Config)
# CTest 参数依次固定 ASCII 字体、CJK 字体与构建输出根目录。
mmm_register_test_case(
  NAME
  AsciiFontRasterizerTest
  COMMAND
  # 使用真实内置字体覆盖 ASCII 与 CJK 两类栅格化输入，并把临时副本放入构建目录。
  AsciiFontRasterizerTest
  "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/font/0xProtoNerdFontPropo-Regular.ttf"
  "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/font/NotoSansMonoCJKsc-Regular.otf"
  "${CMAKE_BINARY_DIR}/test_output")

# 启动期系统字体解析仅链接当前平台的原生字体查询依赖。 Windows 通过 DirectWrite 解析当前 UI 字体和中文回退字体。
if(WIN32)
  # 平台库保持 PRIVATE，调用 Font 的业务模块无需感知 DirectWrite。
  target_link_libraries(Font PRIVATE dwrite user32)
  if(MINGW
     AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
     AND NOT MMM_DISABLE_CLANG_LTO)
    # MinGW 的 COM 声明没有公开 LTO 可见性，系统 DLL 实现不属于本程序。 全程序虚表优化会把 QueryInterface
    # 错误收窄到本地分析源的实现，导致 字体 loader 和 Factory2 均返回 E_NOINTERFACE，中文退化为问号。
    # 只限制启动期原生字体桥接文件；保留 ThinLTO、PGO 及其他文件的优化。 显式保留 LTO
    # 单元拆分，使对象格式与其他启用虚表优化的发布单元一致。 Debug 与关闭 ThinLTO 的构建不需要这组例外参数。
    set_property(
      SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/SystemFontResolver.cpp"
      APPEND
      PROPERTY
        COMPILE_OPTIONS
        "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fno-whole-program-vtables;-fsplit-lto-unit>"
    )
  endif()
  # 使用真实系统字体和集合索引，捕获仅在发布构建优化后出现的 COM 查询回归。 该测试不创建窗口、不联网，也不依赖尚未下载的皮肤字体或个人项目。
  mmm_add_test_executable(Graphic SystemFontResolverTest
                          tests/SystemFontResolverTest.cpp)
  # FreeType 验证实际文件和集合索引，不使用测试替身或 ImGui 的默认字体兜底。
  target_link_libraries(SystemFontResolverTest PRIVATE Font Log
                                                       freetype::freetype)
  # 仅 Windows 注册系统字体回归，不要求无桌面的 Unix Runner 安装语言字体包。
  mmm_register_test_case(NAME SystemFontResolverTest COMMAND
                         SystemFontResolverTest)
elseif(APPLE)
  # CoreText 负责系统字体匹配，CoreFoundation 管理其字符串与 URL 元数据。
  find_library(MMM_CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(MMM_CORE_TEXT_FRAMEWORK CoreText REQUIRED)
  target_link_libraries(Font PRIVATE "${MMM_CORE_FOUNDATION_FRAMEWORK}"
                                     "${MMM_CORE_TEXT_FRAMEWORK}")
elseif(UNIX AND NOT ANDROID)
  # 其余 Unix 平台统一依赖 Fontconfig 返回实际字体文件和集合索引。
  find_package(Fontconfig REQUIRED)
  target_link_libraries(Font PRIVATE Fontconfig::Fontconfig)
endif()
