list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# * 引擎最先建立公共音频依赖，避免后续模块重复创建目标。
include("IonCachyEngine")
# * LuaJIT 先于 sol2，脚本绑定使用项目固定的 Lua ABI。
include("luajit")
include("sol2")
# * 窗口系统先于 UI 后端配置。
if(NOT ANDROID)
  # Android 使用 SDL3 窗口，不编译桌面 GLFW。
  include("GLFW")
endif()
include("imgui")
# * 绘图库共享前面创建的 ImGui 目标。
include("implot")
# * 字体与几何基础库对上层仅暴露包装目标。
include("freetype")
include("glm")
include("concurrentqueue")
# * 文件选择组件与本地对话框保持各自平台实现。
include("ImGuiFileDialog")
include("nativefiledialog-extended")
include("entt")
include("clay")
include("stb")
# * ZIP 写出与既有 Unicode 路径兼容仍使用 miniz。
include("miniz")
include("lunasvg")
include("nlohmann_json")
# * 网络依赖包装自行限定可选功能，避免依赖宿主缓存。
include("mbedtls")
include("curl")
# * 联机传输在 TLS 库配置完成后再建立依赖关系。
include("libdatachannel")

# 通用归档独立配置，不影响其他依赖的缓存选项。
include("libarchive")
