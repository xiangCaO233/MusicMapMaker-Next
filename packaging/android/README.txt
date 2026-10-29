Android 主项目实验构建

范围
  Android 使用 SDL3 窗口、事件与 ImGui 平台后端；桌面仍使用 GLFW。
  Vulkan、现有业务 UI、Lua 皮肤与字体共用原项目实现。
  Linux 入口默认同时构建手机 arm64-v8a 和模拟器 x86_64；Windows 默认只构建模拟器。
  当前 UI 保持横屏和桌面布局，仅使用 SDL 标准输入，不做触控手势优化。
  启动图标复用 logo.svg 的双片 M，自适应圆形与圆角遮罩，并提供 Android 13 主题单色轮廓。
  工程目录、音频导入和谱面另存为已接异步 SAF；部分次要原生选择入口仍待迁移。
  Android 请求 Vulkan 1.3，桌面保持原要求；设备仍需满足实际使用的功能和格式。
  Android 暂无预编译依赖包，必须显式 SOURCES_BUILD=ON；默认 OFF 不变且不回退。

在项目根目录执行
  ./scripts/android/build.sh

Windows 原生构建（无 WSL）
  使用 PowerShell 7、现有 MSYS2 clang64、CMake >= 3.31、Ninja、make、Meson、Python 与完整 JDK 21。
  默认 SDK 为 %USERPROFILE%/Android/Sdk，JDK 放在同级 jdk 目录下；可用参数显式指定。
  MSYS2 只提供 configure/make 等宿主工具，安卓库由 Windows NDK 编译，不能混用桌面库。
  命令行工具固定 19.0；安装 SDK 36.1、Build Tools 36.1.0 和 NDK 30.0.16248370。
  SDK 环境重建命令（先设置 JAVA_HOME 指向完整 JDK 21）：
    sdkmanager.bat --sdk_root=<SDK绝对路径> "platform-tools" "platforms;android-36.1" "build-tools;36.1.0" "ndk;30.0.16248370" "emulator" "system-images;android-36;default;x86_64"
  默认只构建 x86_64，以减少磁盘占用与首次编译时间：
    pwsh -File ./scripts/android/build-windows.ps1
  手机 ABI 或双 ABI 构建可从 PowerShell 直接指定：
    ./scripts/android/build-windows.ps1 -Abi arm64-v8a
    ./scripts/android/build-windows.ps1 -Abi x86_64,arm64-v8a
  -SdkRoot、-JavaHome、-WorkDir、-MsysRoot 和 -Jobs 可覆盖默认路径与并发。
  默认生成物集中在仓库 build-android，不进入业务源码目录或桌面 build。
  APK 为 apk/MusicMapMaker-Next-android-debug.apk，原生符号保存在 build/<ABI>。
  模拟器使用单个 API 36 AOSP x86_64 镜像，不安装 Google Play 或 Android Studio：
    pwsh -File ./scripts/android/emulator-windows.ps1 -CreateOnly
    pwsh -File ./scripts/android/emulator-windows.ps1
  默认显示交互窗口；仅自动测试时显式传 -Headless，启动前检查 WHPX 加速。
  配置使用两核、2560 MiB RAM、720×1280 屏幕，禁用 SD 卡与快照。
  userdata 请求 2 GiB；API 36 镜像会提高逻辑容量，但 qcow2 按实际写入占用磁盘。
  已有 AVD 不覆盖、不清除 userdata；反复测试保留应用私有工程与设置。
  adb 命令使用本次 SDK 的 platform-tools/adb.exe，并显式指定 emulator-5554：
    adb -s emulator-5554 install -r <APK绝对路径>
    adb -s emulator-5554 shell am start -n top.xiang233.musicmapmaker.next.dev/top.xiang233.musicmapmaker.MainActivity
    adb -s emulator-5554 emu kill
  系统镜像 API 36 与编译 SDK 36.1 职责不同；无需再下载第二份模拟器镜像。

已验证工具组合
  Linux x86_64 宿主；CMake >= 3.31、Ninja、make、Meson、Python3、curl、tar、Git LFS。
  ANDROID_HOME 默认 ~/Android/Sdk。
  NDK 30.0.16248370，SDK 36.1，Build Tools 36.1.0。
  Java 21；脚本默认 /usr/lib/jvm/openjdk-21，不修改系统默认 Java。
  Gradle 9.3.0 wrapper、AGP 9.0.1；wrapper JAR 是引导文件，随源码保存。
  Vulkan-Headers 固定 vulkan-sdk-1.4.363.0，自动下载到专用构建树的 dependencies 目录。
  SDL Java 和 native 使用同一子模块版本，不复制上游 Java 实现到仓库。
  需先检出项目原有依赖子模块与完整 LFS 资源；缺失资源会终止打包。

集中目录
  MMM_ANDROID_WORK_DIR 默认 <仓库>/build-android，已由 .gitignore 明确排除。
  也可显式指定仓库外目录；仓库内只允许 build-android 及其子目录。
  build/<ABI>          各 ABI 独立的原生 CMake/Ninja、依赖生成物与未剥离符号
  build/gradle         Java 与 APK 打包输出
  build/jni/<ABI>      各 ABI 的 main、SDL3 和 libc++ 共享库
  build/assets         受 Git 管理的默认资源 ZIP 与内容版本标记
  dependencies         Vulkan 公共头文件
  cache                临时文件、项目 Gradle 缓存与独立调试签名
  avd                  本项目模拟器配置、userdata 与运行时磁盘
  apk                  可安装的交付 APK
  evidence             本机验证日志和截图，构建脚本不强制生成验证记录
  Gradle 下载缓存默认位于工作目录内的 cache/gradle-user。
  Linux 可通过 MMM_ANDROID_GRADLE_CACHE、MMM_ANDROID_JAVA_HOME 和 MMM_ANDROID_JOBS 覆盖。
  Linux 的 MMM_ANDROID_ABIS="arm64-v8a" 可仅构建手机，默认 "arm64-v8a x86_64"。
  本机 SDK、NDK、JDK、下载缓存和构建输出不进入 Git；无需追加全局忽略规则。
  清理 build/ 不影响源码；保留 cache/debug.keystore 可保持重复安装签名一致。

安装与启动
  adb -s <设备序列号> install -r <仓库>/build-android/apk/MusicMapMaker-Next-android-debug.apk
  adb -s <设备序列号> shell am start -W -n top.xiang233.musicmapmaker.next.dev/top.xiang233.musicmapmaker.MainActivity

手机数据
  实验包 applicationId 为 top.xiang233.musicmapmaker.next.dev，不覆盖最小探针。
  默认资源随包提供，首次启动展开到本包 files/config/mmm/assets。
  配置和资源位于应用私有 files，日志在 cache/mmm/logs。
  SAF 工作副本与 URI 映射属于 files/documents 用户数据，不是可随意清理的缓存。
  后续资源包变化按 ZIP 内容标记更新，升级保留已有配置。
  后台恢复只更新 Surface/交换链；业务视图和工程状态继续保留。
  初期使用 identity surface transform 由系统合成器补偿横屏方向。
  设备旋转的 GPU 预旋转优化仍待单独接入。

Android Studio
  File > Open 选择仓库下 packaging/android，不选择整个 CMake 源码根目录。
  Gradle daemon 由 gradle/gradle-daemon-jvm.properties 固定使用完整 JDK 21。
  Settings > Build, Execution, Deployment > Build Tools > Gradle：
    Gradle JDK 选完整 JDK 21（Windows 可选 ~/Android/jdk 下的安装），避免无 jlink 的 JRE。
    Gradle user home 选 <仓库>/build-android/cache/gradle-user，复用脚本下载缓存。
  Java 或 Manifest 修改后可由 Studio 打包；修改 C++ 后须先运行对应平台的构建脚本。
  原生生成物、资源与签名仍取自集中工作树，Studio 不隐式重复构建另一套 CMake。
  .idea、Android 的 .gradle 和 local.properties 为忽略的本机文件，wrapper 继续入 Git。

手机文件访问
  默认系统文件选择（SAF）：工程目录授予整树权限，单文件导入只授予该文件。
  成功授权后复制为持久工作副本，保存前核对外部摘要，写入后读回校验。
  取消系统选择不改变当前工程；重选同一 URI 不覆盖可能存在的本地编辑。
  本机目录兼容：设置 > 软件 > 偏好与同步 > 文件访问方式，选择本机目录兼容。
  点击“打开所有文件访问设置”，由用户手动授权；返回后重新打开共享存储中的工程目录。
  兼容模式直接打开和保存原目录，不使用 SAF 工作副本；原有 SAF 副本仍独立保留。
  未授权时回到 SAF，切回“系统文件选择”可继续使用系统文档提供器。
  当前兼容授权入口需要 Android 11 / API 30 及以上；更早平台继续使用 SAF。
  NX809J / Android 16 自带提供器实测目录列表为空，猫头鹰提供器可导入但新建嵌套文件被其拒绝。
  这两条实测限制不等于 SAF 通用不可用，也不能将私有副本保存成功当作外部写回成功。

桌面回归
  cmake --build build --parallel <线程数>
  ctest --test-dir build --output-on-failure
  Android 分支不更改 SOURCES_BUILD 默认值或桌面 GLFW 的选择。

本机验收（2026-10-20～21）
  NX809J / Android 16 / arm64-v8a：现有中文主界面、皮肤字体、侧栏和设置页绘制与点击通过。
  修复后台过渡时交换链创建失败后继续使用空句柄的问题；连续三次 Home/恢复保持相同 PID 和设置页。
  桌面 LLVM 23 Debug、预编译依赖模式构建通过；完整 CTest 169/169 通过。
  x86_64 / API 36.1 模拟器：原生 x86_64 库启动现有中文界面，Goldfish Vulkan 绘制通过。
  Cherry Pop 测试副本：SAF 目录导入、谱面与背景加载、音频时间轴和系统输出活跃状态已确认。
  NX809J 的 SAF 嵌套文件写回受提供器问题阻塞，不能视为该手机 SAF 保存通过。
  AOSP 模拟器系统 SAF：目录导入、嵌套 .mmm 文件新建、谱面外部写回及摘要读回校验通过。
  用户手动授予所有文件访问后，兼容模式可浏览共享存储，打开同一测试工程。
  原目录 .mmm 分片保存、谱面写入、另存为新文件和重启后重开通过。
  真机与模拟器保存文件读回均为 1090 个物件；用户确认手机能正常听到音乐。
  修复 Android 合成旋转持续返回 Suboptimal 时逐帧重建交换链的问题。
  同一工程播放、同一 8× 配置：修复前约 70～90 FPS，修复后代表性区间约 955～960 FPS。
  这些 FPS 是应用提交次数；系统实际显示模式观察为 120Hz，应用已请求设备支持的 144Hz。
  未读取到受保护的 GPU 忙碌计数器，不能将 CPU 提交统计当作物理显示或 GPU 完成次数。
  验收后已关闭真机临时渲染性能日志；本批逐文件注释率见集中 evidence/comment-audit.tsv。
  触控优化尚未开展。

Windows 原生验收（2026-10-29）
  Windows NDK r30、MSYS2 宿主工具、JDK 21、Gradle 9.3.0 和 AGP 9.0.1 完成 x86_64 构建。
  默认 build-android 工作树完成原生链接、资源打包、APK 生成和 apksigner 校验。
  单个 API 36 AOSP x86_64 模拟器通过 WHPX 启动；中文主界面、设置页点击与 Home/恢复通过。
  横屏由模拟器系统旋转设置验证，恢复前后 PID 一致，crash buffer 无崩溃记录。
  启动时通过默认 X509TrustManager 导出当前应用接受的 CA，以 PEM 快照接入所有 libcurl 请求。
  更新检查成功读取官方清单；资源下载、远程图片和 PGO 上传共用相同的句柄工厂。
  证书加载失败时拒绝联网，离线编辑器仍可启动；证书链和主机名校验保持启用。
  设备端 CurlTrustTest 对本机受控 HTTPS 服务验证正常证书成功，自签名、过期、域名不符均返回 60。
  测试 CA 只传给独立探针进程，未安装到 Android 系统或应用的信任库。
  桌面完整构建和 CTest 180/180 通过；本轮未重新构建手机 arm64-v8a。
