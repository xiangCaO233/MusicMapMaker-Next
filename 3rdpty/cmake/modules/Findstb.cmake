# 导入 stb 头文件包，导出源码构建同名目标：3rd_stb。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 动态或静态构建都使用同一版声明，预编译布局只需公共头树。
prebuilt_include_dir(_stb_include_dir stb)

# 各格式的实现宏由项目选定的翻译单元定义，避免接口目标传播定义并产生重复符号。
if(NOT TARGET 3rd_stb)
  add_library(3rd_stb INTERFACE)
  # 系统包含属性限制外部头的告警范围，业务目标的告警仍保持开启。
  target_include_directories(3rd_stb SYSTEM INTERFACE "${_stb_include_dir}")
endif()

# 找到该包只表示声明可用，各实现仍由调用项目按需编译。
set(stb_FOUND TRUE)
