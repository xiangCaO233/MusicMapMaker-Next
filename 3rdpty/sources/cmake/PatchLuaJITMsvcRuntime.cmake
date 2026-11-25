# 修补复制到构建目录的 LuaJIT MSVC 构建脚本，避免修改第三方源码。 带有路径校验的入口只允许操作调用方明确准备的副本；错误时立即终止打包。
if(NOT DEFINED LUAJIT_MSVCBUILD OR NOT EXISTS "${LUAJIT_MSVCBUILD}")
  message(FATAL_ERROR "LuaJIT MSVC 构建脚本不存在：${LUAJIT_MSVCBUILD}")
endif()

# 优先完整编译参数，也兼容原先只传入 CRT 参数的调用方。
if(NOT DEFINED LUAJIT_MSVC_COMPILE_FLAGS OR LUAJIT_MSVC_COMPILE_FLAGS STREQUAL
                                            "")
  # 保留旧静态构建入口，在调用方还未迁移到完整参数时沿用同一运行库。
  if(DEFINED LUAJIT_STATIC_RUNTIME_FLAG AND NOT LUAJIT_STATIC_RUNTIME_FLAG
                                            STREQUAL "")
    # 运行库选项进入对象文件，不能仅靠最终链接器参数补救。
    set(LUAJIT_MSVC_COMPILE_FLAGS "${LUAJIT_STATIC_RUNTIME_FLAG}")
    # 拒绝默默采用 cl 默认 CRT，否则可能与宿主业务模块的链接模式不一致。
  else()
    message(FATAL_ERROR "缺少 LuaJIT MSVC 编译参数。")
  endif()
endif()

# 链接侧可无额外选项，静态构建仍需通过上面的编译参数检查。
if(NOT DEFINED LUAJIT_MSVC_LINK_FLAGS)
  set(LUAJIT_MSVC_LINK_FLAGS "")
endif()

# 在内存中完成两处替换后再写回，源树未进入此次文件操作。
file(READ "${LUAJIT_MSVCBUILD}" _luajit_msvcbuild_content)
# 保持上游脚本已有的禁止横幅、生成对象和优化参数，仅追加本次 CRT 配置。
string(REPLACE "@set LJCOMPILE=cl /nologo /c /O2"
               "@set LJCOMPILE=cl /nologo /c ${LUAJIT_MSVC_COMPILE_FLAGS} /O2"
               _luajit_msvcbuild_content "${_luajit_msvcbuild_content}")
# 无链接附加项时保留原命令，避免改变静态构建的命令签名。
if(NOT LUAJIT_MSVC_LINK_FLAGS STREQUAL "")
  # 附加选项由统一的发布配置生成，使后续 PDB 与归档布局由调用方控制。
  string(REPLACE "@set LJLINK=link /nologo"
                 "@set LJLINK=link /nologo ${LUAJIT_MSVC_LINK_FLAGS}"
                 _luajit_msvcbuild_content "${_luajit_msvcbuild_content}")
endif()
# 只有生成目录中的脚本需要被更新，上游源码树仍可用于其他运行库组合。
file(WRITE "${LUAJIT_MSVCBUILD}" "${_luajit_msvcbuild_content}")
