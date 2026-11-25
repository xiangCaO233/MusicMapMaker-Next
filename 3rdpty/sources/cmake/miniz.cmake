# 压缩源文件按 C ABI 编译，C++ 调用者通过公开头解析符号。
enable_language(C)

# miniz 是项目自维护包装目标，shared 偏好下生成 DLL 而不是静态归档。
set(MINIZ_LIBRARY_TYPE STATIC)
# 统一打包入口同时支撑静态消费与 DLL 分发。
if(PROJECT_LINKAGE STREQUAL "shared")
  set(MINIZ_LIBRARY_TYPE SHARED)
endif()

# 压缩、解压及 ZIP 归档操作共享同一目标，避免调用者各自编译成重复实现。
add_library(
  3rd_miniz
  ${MINIZ_LIBRARY_TYPE}
  "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz_tdef.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz_tinfl.c"
  # 提供资源包加载所需 ZIP 接口，避免消费者再自行链接另一套归档实现。
  "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz_zip.c")

# 生成头记录本次库类型和导出规则，不在源码目录保存某一平台的版本。
include(GenerateExportHeader)
# 二进制目录中的头和本次编译产物配对，调用者通过目标公开路径获取。
generate_export_header(3rd_miniz BASE_NAME miniz EXPORT_FILE_NAME
                       "${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h")

# 系统包含作用域同时覆盖依赖源码头和生成的导出头。
target_include_directories(
  3rd_miniz SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/miniz"
                          "${CMAKE_CURRENT_BINARY_DIR}")

# 静态归档也可能进入共享宿主，位置无关代码避免在运行时文本段重定位。
set_target_properties(3rd_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
# 只为 Windows 共享包生成自动导出规则，不改变静态消费者的符号。
if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
  set_target_properties(3rd_miniz PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
