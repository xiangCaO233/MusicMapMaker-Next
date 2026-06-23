# 修补复制到构建目录的 LuaJIT MSVC 构建脚本，避免修改第三方源码。
if(NOT DEFINED LUAJIT_MSVCBUILD OR NOT EXISTS "${LUAJIT_MSVCBUILD}")
  message(FATAL_ERROR "LuaJIT MSVC 构建脚本不存在：${LUAJIT_MSVCBUILD}")
endif()

if(NOT DEFINED LUAJIT_STATIC_RUNTIME_FLAG OR LUAJIT_STATIC_RUNTIME_FLAG
                                             STREQUAL "")
  message(FATAL_ERROR "缺少 LuaJIT 静态运行时参数。")
endif()

file(READ "${LUAJIT_MSVCBUILD}" _luajit_msvcbuild_content)
string(REPLACE "@set LJCOMPILE=cl /nologo /c /O2"
               "@set LJCOMPILE=cl /nologo /c ${LUAJIT_STATIC_RUNTIME_FLAG} /O2"
               _luajit_msvcbuild_content "${_luajit_msvcbuild_content}")
file(WRITE "${LUAJIT_MSVCBUILD}" "${_luajit_msvcbuild_content}")
