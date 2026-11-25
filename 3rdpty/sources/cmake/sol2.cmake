# 本文件用于源码依赖入口。在预编译模式中，对应查找模块提供同名业务接口目标。

# LuaJIT 由上级先行配置并提供脚本运行时；避免 sol2 再引入第二套 Lua 实现进入链接图。
set(SOL2_BUILD_LUA
    OFF
    CACHE BOOL "" FORCE)

# 让 sol2 按 LuaJIT 查找开发头，绑定层和实际运行时必须使用同一组 Lua API 类型与宏。
set(SOL2_LUA_VERSION
    "LuaJIT"
    CACHE STRING "" FORCE)

# 子项目构建只提供绑定头，不启动上游的额外测试、示例与文档目标。
set(SOL2_TESTS
    OFF
    CACHE BOOL "" FORCE)
# 示例不参与产品安装，避免为教学程序探测额外依赖。
set(SOL2_EXAMPLES
    OFF
    CACHE BOOL "" FORCE)
# 文档生成属于上游发布流程，源码方式消费绑定头不引入文档工具链。
set(SOL2_DOCS
    OFF
    CACHE BOOL "" FORCE)
# 单文件示例具有单独开关，关闭普通示例不应意外启用另一组构建目标。
set(SOL2_EXAMPLES_SINGLE
    OFF
    CACHE BOOL "" FORCE)

# 公共头由项目打包入口统一归档；子项目安装不能再次向发布树写入路径和导出配置。
set(SOL2_ENABLE_INSTALL
    OFF
    CACHE BOOL "" FORCE)

# 上游目标仍提供正确的包含目录与编译约束；SYSTEM 隔离供应商头文件的诊断。
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/sol2" SYSTEM)

# 将上游目标名称隐藏在依赖包装内，业务模块只使用与预编译入口相同的目标名。
add_library(3rd_sol2 INTERFACE)
# 包装目标转发绑定头，LuaJIT 二进制仍由独立的运行时目标负责链接。
target_link_libraries(3rd_sol2 INTERFACE sol2::sol2)
