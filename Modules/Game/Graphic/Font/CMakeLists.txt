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
add_test(
  NAME AsciiFontRasterizerTest
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
elseif(APPLE)
  # CoreText 负责系统字体匹配，CoreFoundation 管理其字符串与 URL 元数据。
  find_library(MMM_CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(MMM_CORE_TEXT_FRAMEWORK CoreText REQUIRED)
  target_link_libraries(Font PRIVATE "${MMM_CORE_FOUNDATION_FRAMEWORK}"
                                     "${MMM_CORE_TEXT_FRAMEWORK}")
elseif(UNIX)
  # 其余 Unix 平台统一依赖 Fontconfig 返回实际字体文件和集合索引。
  find_package(Fontconfig REQUIRED)
  target_link_libraries(Font PRIVATE Fontconfig::Fontconfig)
endif()
