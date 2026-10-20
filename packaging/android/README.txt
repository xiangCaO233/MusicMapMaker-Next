Android 主项目实验构建

范围
  Android 使用 SDL3 窗口、事件与 ImGui 平台后端；桌面仍使用 GLFW。
  Vulkan、现有业务 UI、Lua 皮肤与字体共用原项目实现。
  当前脚本只构建 arm64-v8a 调试 APK，目标是连接的手机。
  当前 UI 保持横屏和桌面布局，仅使用 SDL 标准输入，不做触控手势优化。
  桌面 NFD 文件选择器尚未接 Android SAF，相关入口明确返回不支持。
  主项目 APK 不等于先前的最小探针；本轮未构建主项目 x86_64 模拟器包。
  当前主渲染器仍要求 Vulkan 1.4，不能据此承诺所有 Vulkan 手机均可运行。
  Android 暂无预编译依赖包，必须显式 SOURCES_BUILD=ON；默认 OFF 不变且不回退。

在项目根目录执行
  ./scripts/android/build.sh

已验证工具组合
  Linux x86_64 宿主；CMake >= 3.31、Ninja、make、Meson、Python3、curl、tar、Git LFS。
  ANDROID_HOME 默认 ~/Android/Sdk。
  NDK 30.0.16248370，SDK 36.1，Build Tools 36.1.0。
  Java 21；脚本默认 /usr/lib/jvm/openjdk-21，不修改系统默认 Java。
  Gradle 9.3.0 wrapper、AGP 9.0.1；wrapper JAR 是引导文件，随源码保存。
  Vulkan-Headers 固定 vulkan-sdk-1.4.363.0，自动下载到外部依赖目录。
  SDL Java 和 native 使用同一子模块版本，不复制上游 Java 实现到仓库。
  需先检出项目原有依赖子模块与完整 LFS 资源；缺失资源会终止打包。

集中目录
  MMM_ANDROID_WORK_DIR 默认 ~/Android/mmm-smoke/main，必须在源码仓库外。
  build/arm64-v8a       原生 CMake/Ninja、依赖生成物与未剥离符号
  build/gradle         Java 与 APK 打包输出
  build/jni            本次 APK 的三个共享库
  build/assets         受 Git 管理的默认资源 ZIP 与内容版本标记
  dependencies         Vulkan 公共头文件
  cache                临时文件、项目 Gradle 缓存与独立调试签名
  apk                  可安装的交付 APK
  evidence             本机验证日志和截图，构建脚本不强制生成验证记录
  Gradle 下载缓存默认共用工作目录同级 cache/gradle。
  可通过 MMM_ANDROID_GRADLE_CACHE、MMM_ANDROID_JAVA_HOME 和 MMM_ANDROID_JOBS 覆盖。
  本机 SDK、NDK、JDK、下载缓存和构建输出不进入 Git；无需追加全局忽略规则。
  清理 build/ 不影响源码；保留 cache/debug.keystore 可保持重复安装签名一致。

安装与启动
  adb -s <设备序列号> install -r ~/Android/mmm-smoke/main/apk/MusicMapMaker-Next-android-arm64-debug.apk
  adb -s <设备序列号> shell am start -W -n top.xiang233.musicmapmaker.next.dev/top.xiang233.musicmapmaker.MainActivity

手机数据
  实验包 applicationId 为 top.xiang233.musicmapmaker.next.dev，不覆盖最小探针。
  默认资源随包提供，首次启动展开到本包 files/config/mmm/assets。
  配置、日志和解压缓存位于应用私有目录，不申请共享存储权限。
  后续资源包变化按 ZIP 内容标记更新，升级保留已有配置。
  后台恢复只更新 Surface/交换链；业务视图和工程状态继续保留。
  初期使用 identity surface transform 由系统合成器补偿横屏方向。
  设备旋转的 GPU 预旋转优化和 SAF 文件导入/导出需后续单独接入。

桌面回归
  cmake --build build --parallel <线程数>
  ctest --test-dir build --output-on-failure
  Android 分支不更改 SOURCES_BUILD 默认值或桌面 GLFW 的选择。

本机验收（2026-10-20）
  NX809J / Android 16 / arm64-v8a：现有中文主界面、皮肤字体、侧栏和设置页绘制与点击通过。
  修复后台过渡时交换链创建失败后继续使用空句柄的问题；连续三次 Home/恢复保持相同 PID 和设置页。
  桌面 LLVM 23 Debug、预编译依赖模式构建通过；完整 CTest 169/169 通过。
  本批自维护源码、构建配置逐文件注释率记录见集中 evidence/comment-ratios.tsv。
  这些验证不代表已完成触控编辑、SAF 导入导出或手机音频播放回归。
