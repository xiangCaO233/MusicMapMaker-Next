# DEB 发布与普通安装的边界：

# * 只有 DebianRuntime 组件进入 DEB，不改变已有 DesktopIntegration 组件。
# * CPack 根前缀为 /，相对 opt/mmm 路径最终落到 /opt/mmm。
# * 主 ELF 来自同一次构建，不混入另一个工具链的更新器。
# * 系统库由 dpkg-shlibdeps 建立依赖，不复制第三方运行库。
# * 运行库下限来自 ELF 符号，不能按发行版名称猜测。
# * 主程序文件名、桌面 Exec 和命令链接始终采用稳定名称。
# * 多配置构建使用目标生成器表达式，不猜测配置输出目录。
# * 替代二进制入口只用于已发布产物，不能把开发机 ABI 强加给发行包。
# * 软件版本由根工程定义，包修订仅表达同版本的重打包顺序。
# * 同一版本和修订对应不可变文件，仓库生成器拒绝哈希冲突。
# * 交叉构建的目标架构不能无条件采用宿主 dpkg 架构。
# * 目标架构同时用于 DEB 控制字段和软件源过滤。
# * 包内不包含个人配置、日志、下载缓存或测试资源。
# * 主程序只保留一份，终端入口必须是符号链接。
# * 公钥属于公开包内容，私钥和撤销证书留在发布服务器。
# * 密钥转换只写入构建目录，不修改宿主全局信任库。
# * 安装过程离线建立 Signed-By 关系，不从网络取得新信任材料。
# * 维护脚本必须幂等，不在 dpkg 锁内递归调用 APT。
# * postrm 升级分支保留软件源，最终移除则清理密钥引用。
# * 桌面文件的 App ID 与 Wayland 窗口识别保持一致。
# * 源码版本随包记录，Gentoo 源码版可与二进制版核对输入。
# * 版权文本随包发布，许可证不因安装方式改变。
# * 缺少发行版包工具时默认关闭，仅显式开启才要求这些工具。
# * 测试用 Python 只是构建依赖，维护脚本只依赖 Essential 工具。
# * CTest 脚本与 C++ 套件独立，所有输出位于构建树。
# * CPack 变量在 include(CPack) 前完成，生成配置捕获同一组参数。
# * 关闭 DEB 打包只跳过发布规则，不改变原有主程序构建。
# * Windows 与 macOS 不消费 Debian 模板或命令。

# Linux 的 DEB 发布只收集 DebianRuntime 组件，普通 install 和其他平台不改变布局。 主程序与桌面资源以外，只安装 APT
# 信任公钥；更新器和预编译依赖不进入该组件。
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

# Debian 主机默认开放 CPack；其他 Linux 主机可显式关闭，避免要求 dpkg 工具。
find_program(MMM_DPKG_EXECUTABLE dpkg)
set(_mmm_deb_default OFF)
if(MMM_DPKG_EXECUTABLE)
  set(_mmm_deb_default ON)
endif()
option(MMM_ENABLE_DEB_PACKAGING "启用 Linux CPack DEB 打包。" ${_mmm_deb_default})
if(NOT MMM_ENABLE_DEB_PACKAGING)
  return()
endif()

# 版本修订独立于应用版本：同一版本重发修复包时必须递增，APT 才会升级。
set(MMM_DEB_REVISION
    "2"
    CACHE STRING "DEB 包修订号，只接受正整数。")
if(NOT MMM_DEB_REVISION MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "MMM_DEB_REVISION 必须为正整数。")
endif()
# 公共地址由安装脚本固定声明；离线安装也使用包内公钥，不在安装期间下载密钥。
set(MMM_DEB_MAINTAINER
    "xiangCaO233 <odokoei@outlook.com>"
    CACHE STRING "DEB 维护者及公开联系邮箱。")
# 正常配置直接消费本次构建目标；发布站现有二进制可用此入口打包，避免重编译改变 ABI。
set(MMM_DEB_EXECUTABLE
    ""
    CACHE FILEPATH "可选的已发布 Linux 主程序绝对路径。")
if(MMM_DEB_EXECUTABLE)
  if(NOT IS_ABSOLUTE "${MMM_DEB_EXECUTABLE}" OR NOT EXISTS
                                                "${MMM_DEB_EXECUTABLE}")
    message(FATAL_ERROR "MMM_DEB_EXECUTABLE 必须指向存在的绝对路径。")
  endif()
  set(_mmm_deb_program "${MMM_DEB_EXECUTABLE}")
else()
  # 生成器表达式按实际配置选择主程序，不把 build/bin 或配置名写死。
  set(_mmm_deb_program "$<TARGET_FILE:MusicMapMaker-Next>")
endif()

# 包架构随构建目标选择，交叉构建必须显式覆盖，不能无条件采用宿主 dpkg 架构。
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(_mmm_deb_arch "amd64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(_mmm_deb_arch "arm64")
else()
  message(FATAL_ERROR "当前目标架构尚未定义 DEB 映射：${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(MMM_DEB_ARCHITECTURE
    "${_mmm_deb_arch}"
    CACHE STRING "DEB 目标架构。")

# 只导出公钥；私钥保留在发布服务器的专用 GnuPG 目录，永远不进入包或源码仓库。
find_program(MMM_GPG_EXECUTABLE gpg REQUIRED)
set(_mmm_deb_dir "${CMAKE_BINARY_DIR}/generated/debian")
file(MAKE_DIRECTORY "${_mmm_deb_dir}")
execute_process(
  COMMAND
    "${MMM_GPG_EXECUTABLE}" --batch --yes --dearmor --output
    "${_mmm_deb_dir}/mmm-archive-keyring.gpg"
    "${CMAKE_SOURCE_DIR}/packaging/linux/mmm-archive-keyring.asc"
    COMMAND_ERROR_IS_FATAL ANY)
# 维护脚本由 dpkg 调用，只在 configure/remove/purge 边沿更新自己的源条目。 postrm
# 升级分支必须保留新包配置；两个模板共享相同源地址和目标架构。
foreach(_mmm_script postinst postrm)
  configure_file("${CMAKE_SOURCE_DIR}/packaging/linux/${_mmm_script}.in"
                 "${_mmm_deb_dir}/${_mmm_script}" @ONLY)
endforeach()

# 源码身份记录实际构建输入，便于核对发行包和后续开发版本。

# * 原生构建读取当前 Git 提交，不使用可移动的分支名或标签名。
# * 替代 ELF 必须显式传入真实源码提交，不能取包装仓当前 HEAD。
# * 只记录公开的源码身份与编译器版本，不记录构建用户或签名私钥。
# * 校验完整提交哈希，避免短哈希在历史增长后失去唯一性。
# * Git 压缩源码包没有元数据时允许调用方提供同一身份，不伪造空值。
# * Gentoo live 源码包可以使用较新 HEAD，不要求与已发布 DEB 相等。
set(MMM_DEB_SOURCE_COMMIT
    ""
    CACHE STRING "发行主程序对应的完整源码提交。")
if(NOT MMM_DEB_SOURCE_COMMIT)
  if(MMM_DEB_EXECUTABLE)
    message(FATAL_ERROR "包装外部 ELF 时必须设置 MMM_DEB_SOURCE_COMMIT。")
  endif()
  execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE MMM_DEB_SOURCE_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif()
string(LENGTH "${MMM_DEB_SOURCE_COMMIT}" _mmm_commit_length)
if(NOT _mmm_commit_length EQUAL 40 OR NOT MMM_DEB_SOURCE_COMMIT MATCHES
                                      "^[0-9a-f]+$")
  message(FATAL_ERROR "MMM_DEB_SOURCE_COMMIT 必须是完整的 40 位 Git 提交。")
endif()
# 文档目录中的结构化记录供发布者核对，不是程序启动时的配置输入。
configure_file("${CMAKE_SOURCE_DIR}/packaging/linux/build-info.json.in"
               "${_mmm_deb_dir}/build-info.json" @ONLY)
install(
  FILES "${_mmm_deb_dir}/build-info.json"
  DESTINATION "usr/share/doc/musicmapmaker-next"
  COMPONENT DebianRuntime)

# 相对路径配合 CPack 的根前缀形成 /opt/mmm；安装树只含一个主 ELF。
install(
  PROGRAMS "${_mmm_deb_program}"
  DESTINATION "opt/mmm"
  RENAME MusicMapMaker-Next
  COMPONENT DebianRuntime)
# 入口符号链接使终端与桌面 Exec 共用同一二进制，不复制第二份程序。
configure_file("${CMAKE_SOURCE_DIR}/packaging/linux/InstallLinks.cmake.in"
               "${_mmm_deb_dir}/InstallLinks.cmake" @ONLY)
install(SCRIPT "${_mmm_deb_dir}/InstallLinks.cmake" COMPONENT DebianRuntime)
# 桌面资源仍采用已有 Wayland App ID；不把整套运行 assets 塞进安装包。
install(
  FILES "${CMAKE_SOURCE_DIR}/Modules/Main/src/MusicMapMaker-Next.desktop"
  DESTINATION "usr/share/applications"
  COMPONENT DebianRuntime)
install(
  FILES "${CMAKE_SOURCE_DIR}/Modules/Main/src/logo.svg"
  DESTINATION "usr/share/icons/hicolor/scalable/apps"
  RENAME MusicMapMaker-Next.svg
  COMPONENT DebianRuntime)
# 公钥为包管理文件；postrm remove 清理对应源，避免删除公钥后 APT 仍引用它。
install(
  FILES "${_mmm_deb_dir}/mmm-archive-keyring.gpg"
  DESTINATION "usr/share/keyrings"
  COMPONENT DebianRuntime)
install(
  FILES "${CMAKE_SOURCE_DIR}/LICENSE"
  DESTINATION "usr/share/doc/musicmapmaker-next"
  RENAME copyright
  COMPONENT DebianRuntime)

# 唯一组件过滤掉既有普通安装规则和所有第三方库，不改变它们本来的安装方式。 生成器只声明 DEB，其他平台的 CPack 配置由对应模块处理。
set(CPACK_GENERATOR DEB)
# 组件白名单避免普通 install 的 /bin 主程序再次进入包内。
set(CPACK_COMPONENTS_ALL DebianRuntime)
# 按组件过滤安装树；这里只有一个发行组件，不产生拆分运行包。
set(CPACK_DEB_COMPONENT_INSTALL ON)
# 包安装前缀是系统根；不会沿用开发构建的 /usr/local 前缀。
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
# 产品包名采用 Debian 小写约定，与 APT 索引和 Gentoo URI 相同。
set(CPACK_PACKAGE_NAME "musicmapmaker-next")
# 供应者字段不参与安装目录和程序窗口标题。
set(CPACK_PACKAGE_VENDOR "MusicMapMaker")
# 应用版本只取根工程版本，修订号由独立缓存参数控制。
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "MusicMapMaker-Next rhythm game beatmap editor")
set(CPACK_DEBIAN_PACKAGE_NAME "musicmapmaker-next")
# 单组件 DEB 仍使用产品包名，禁止 CPack 自动附加内部组件后缀。
set(CPACK_DEBIAN_DEBIANRUNTIME_PACKAGE_NAME "musicmapmaker-next")
# 修订提高但应用版本不变时，dpkg 的版本比较仍能识别升级。
set(CPACK_DEBIAN_PACKAGE_RELEASE "${MMM_DEB_REVISION}")
# 只发布实际生成的目标架构，APT 不向其他架构安装本 ELF。
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${MMM_DEB_ARCHITECTURE}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${MMM_DEB_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://mmm.xiang233.top")
# 声音分类供软件管理器展示，不产生额外的运行依赖。
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
# 按实际 ELF 生成运行库版本下限，不宣称跨发行版或旧 glibc 的二进制兼容性。
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "ca-certificates")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "xdg-utils")
# 标准文件名携带版本、修订和架构，发布脚本无需重新命名。
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${_mmm_deb_dir}/postinst;${_mmm_deb_dir}/postrm")
# dpkg 要求维护脚本拥有执行位，不能依赖源码模板在克隆后的文件权限。
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
include(CPack)

# 脚本测试不链接 C++ 套件，仍由 CTest 在隔离目录中统一运行。
if(BUILD_TESTING)
  find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED)
  add_test(
    NAME DebianMaintenanceTest
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/packaging/linux/tests/test_maintenance.py")
  set_tests_properties(
    DebianMaintenanceTest
    PROPERTIES
      ENVIRONMENT
      "MMM_TEST_OUTPUT_DIR=${CMAKE_BINARY_DIR}/test_output/debian-maintenance")
  add_test(
    NAME AptRepositoryTest
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/apt/test_generate_repository.py")
  set_tests_properties(
    AptRepositoryTest
    PROPERTIES
      ENVIRONMENT
      "MMM_TEST_OUTPUT_DIR=${CMAKE_BINARY_DIR}/test_output/apt-repository")
  # 发布测试模拟 SSH/SCP 与失败清理，不连接生产服务器。
  add_test(NAME AptPublishTest
           COMMAND "${Python3_EXECUTABLE}"
                   "${CMAKE_SOURCE_DIR}/scripts/apt/test_publish_repository.py")

endif()
