# 导入 sol2 头文件包，并提供项目包装目标：3rd_sol2。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 绑定模板与编译单元中的 LuaJIT 头共同实例化，因此这里固定分发的 sol2 版本。 LuaJIT 的库和运行时由独立包解析，此接口不替消费者选择
# Lua ABI。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_sol2_include_dir sol2)

# 全局导入保持不同脚本模块看到相同的模板及包含目录。
if(NOT TARGET sol2::sol2)
  add_library(sol2::sol2 INTERFACE IMPORTED GLOBAL)
  set_target_properties(sol2::sol2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_sol2_include_dir}")
endif()

# 包装目标对齐源码构建名称，业务目标可以独立声明所需 Lua 运行时。
if(NOT TARGET 3rd_sol2)
  add_library(3rd_sol2 INTERFACE)
  target_link_libraries(3rd_sol2 INTERFACE sol2::sol2)
endif()

# 报告的是绑定头就绪，LuaJIT 本体仍需由使用者的链接依赖保证。
set(sol2_FOUND TRUE)
