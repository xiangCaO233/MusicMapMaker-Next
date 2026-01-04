# 告诉 sol2 不要构建它自带的 Lua (默认是 5.4.4)
# 使用 LuaJIT
set(SOL2_BUILD_LUA OFF CACHE BOOL "" FORCE)

# 明确指定版本为 LuaJIT，这样 sol2 会去找 LuaJIT 的头文件
set(SOL2_LUA_VERSION "LuaJIT" CACHE STRING "" FORCE)

# 关掉 sol2 自带的测试、示例和文档
set(SOL2_TESTS OFF CACHE BOOL "" FORCE)
set(SOL2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SOL2_DOCS OFF CACHE BOOL "" FORCE)
set(SOL2_EXAMPLES_SINGLE OFF CACHE BOOL "" FORCE)
set(SOL2_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

# ==========================================
# 引入 sol2
# ==========================================
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/sol2")
