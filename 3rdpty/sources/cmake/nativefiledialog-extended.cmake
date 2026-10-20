# 文件对话框的消费 target 必须存在，但 Android 尚无桌面 NFD 原生实现。
# 只暴露接口头，由项目维护的兼容层返回明确的能力错误。
# 不配置上游桌面源文件，避免拉入 GTK、DBus、X11 等宿主依赖。
# 后续 SAF 属于平台文件访问适配，不在窗口后端接入阶段伪造路径。
if(ANDROID)
  # Android 没有桌面 NFD 后端；兼容层明确返回不支持，不伪造文件选择成功。
  add_library(3rd_nfd INTERFACE)
  target_include_directories(
    3rd_nfd
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/nativefiledialog-extended/src/include")
  return()
endif()
# 桌面仍使用原有 NFD 源码 target，Android 的早退不会改变此路径。
# 对外别名保持稳定，业务无需按依赖来源判断链接目标。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/nativefiledialog-extended SYSTEM)
# nfd 作为 shared 预编译包时使用自动导出，保持业务侧 target 名称不变。
if(PROJECT_LINKAGE STREQUAL "shared"
   AND WIN32
   AND TARGET nfd)
  set_target_properties(nfd PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

add_library(3rd_nfd INTERFACE)
target_link_libraries(3rd_nfd INTERFACE nfd::nfd)
