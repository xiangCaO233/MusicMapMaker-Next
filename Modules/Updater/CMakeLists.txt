# 更新器是最小独立辅助程序，仅使用标准库和平台 API，不链接主项目业务模块。 独立链接保证主程序退出并释放自身映像后，更新器仍可继续完成文件替换。

if(WIN32 AND NOT MSVC)
  # MinGW 构建显式编译 manifest 资源；MSVC 由其它平台打包规则提供清单。
  set(UPDATER_RESOURCE MusicMapMaker-Updater.rc)
endif()

# 平台资源变量在非匹配平台为空，只编译同一份更新流程入口。
add_executable(MusicMapMaker-Updater main.cpp ${UPDATER_RESOURCE})

# 输出到与主程序相同的 bin 目录，便于主程序用稳定相对路径启动更新器。
set_target_properties(
  MusicMapMaker-Updater PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                   "${CMAKE_BINARY_DIR}/bin")

# 发布类配置剥离运行文件符号；GCC 先通过 add_gcc_debug_extract 保留外置调试信息。 MinGW Clang 的 PDB
# 生成与主程序采用相同维护入口。
add_gcc_debug_extract(MusicMapMaker-Updater)
add_mingw_clang_pdb(MusicMapMaker-Updater)
add_strip_command_for_release(MusicMapMaker-Updater)

if(WIN32)
  # shell32 提供 ShellExecute，ws2_32 保持 Windows 独立目标的平台链接约定。
  target_link_libraries(MusicMapMaker-Updater PRIVATE shell32 ws2_32)
  # MSVC CRT 可根据 wmain 自动选择入口，无需覆盖链接选项。
  if(MINGW)
    # MinGW 默认使用窄字符启动对象，显式选用负责构造 wmain 参数的 CRT。
    target_link_options(MusicMapMaker-Updater PRIVATE -municode)
  endif()
endif()

# 通过系统 PowerShell 和实际更新器进程验证 Windows 路径在替换及重启后保持完整。 这里只登记原生 Windows
# CTest。交叉编译产物可在现有 Windows VM 中直接运行脚本。
if(BUILD_TESTING
   AND WIN32
   AND NOT CMAKE_CROSSCOMPILING)
  # PowerShell 是夹具运行时，实际产品不链接脚本或 C# 运行依赖。 路径完全由生成器提供，多配置构建使用当前配置的更新器实例。 脚本进程忽略个人
  # profile，策略参数仅作用于当前测试调用。 每轮在构建目录下建立独立夹具，允许 CTest 并行执行。
  add_test(
    NAME WindowsUpdaterUnicodeTest
    COMMAND
      powershell.exe -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows-updater-unicode.ps1"
      "$<TARGET_FILE:MusicMapMaker-Updater>"
      "${CMAKE_BINARY_DIR}/test_output/windows-updater-unicode")
endif()

# Linux 集成测试向独立更新器注入短写并检查真实文件替换行为。 故障注入依赖 POSIX 资源限制，夹具可用时从 /dev/shm 提供跨卷下载。
# Windows 与 macOS 安装各自采用平台事务，不运行 Linux 进程夹具。
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # 解释器只用于故障注入测试，独立更新器和发布产物无需 Python。
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    # 测试使用构建目录记录目标文件；不接触用户安装或源码测试资源。
    add_test(
      NAME LinuxUpdaterAtomicReplaceTest
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux-updater-atomic-replace.py"
        "$<TARGET_FILE:MusicMapMaker-Updater>"
        "${CMAKE_BINARY_DIR}/test_output/linux-updater-atomic-replace")
  endif()
endif()
