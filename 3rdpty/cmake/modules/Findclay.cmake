# 导入 Clay 头文件包，导出源码构建同名目标：3rd_clay。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# Clay 的实现由项目指定的翻译单元实例化；此入口只传播统一声明。
prebuilt_include_dir(_clay_include_dir clay)

# 预编译分发不编译上游示例，也无需按平台或构建配置选择库。
if(NOT TARGET 3rd_clay)
  add_library(3rd_clay INTERFACE)
  # 将公共头标记为系统包含，业务代码的告警设置不外溢到依赖头。
  target_include_directories(3rd_clay SYSTEM INTERFACE "${_clay_include_dir}")
endif()

# 报告头文件可用，调用端仍须保证实现仅实例化一次。
set(clay_FOUND TRUE)
