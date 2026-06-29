# 修补复制到构建目录的 LuaJIT MSVC 构建脚本，避免修改第三方源码。
if(NOT DEFINED LUAJIT_MSVCBUILD OR NOT EXISTS "${LUAJIT_MSVCBUILD}")
  message(FATAL_ERROR "LuaJIT MSVC 构建脚本不存在：${LUAJIT_MSVCBUILD}")
endif()

if(NOT DEFINED LUAJIT_MSVC_COMPILE_FLAGS OR LUAJIT_MSVC_COMPILE_FLAGS
                                           STREQUAL "")
  if(DEFINED LUAJIT_STATIC_RUNTIME_FLAG AND NOT LUAJIT_STATIC_RUNTIME_FLAG
                                            STREQUAL "")
    set(LUAJIT_MSVC_COMPILE_FLAGS "${LUAJIT_STATIC_RUNTIME_FLAG}")
  else()
    message(FATAL_ERROR "缺少 LuaJIT MSVC 编译参数。")
  endif()
endif()

if(NOT DEFINED LUAJIT_MSVC_LINK_FLAGS)
  set(LUAJIT_MSVC_LINK_FLAGS "")
endif()

file(READ "${LUAJIT_MSVCBUILD}" _luajit_msvcbuild_content)
string(REPLACE "@set LJCOMPILE=cl /nologo /c /O2"
               "@set LJCOMPILE=cl /nologo /c ${LUAJIT_MSVC_COMPILE_FLAGS} /O2"
               _luajit_msvcbuild_content "${_luajit_msvcbuild_content}")
if(NOT LUAJIT_MSVC_LINK_FLAGS STREQUAL "")
  string(REPLACE "@set LJLINK=link /nologo"
                 "@set LJLINK=link /nologo ${LUAJIT_MSVC_LINK_FLAGS}"
                 _luajit_msvcbuild_content "${_luajit_msvcbuild_content}")
endif()
file(WRITE "${LUAJIT_MSVCBUILD}" "${_luajit_msvcbuild_content}")
