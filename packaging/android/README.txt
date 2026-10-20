Android 主项目实验构建

范围
  Android 使用 SDL3 窗口、事件与 ImGui 平台后端；桌面仍使用 GLFW。
  Vulkan、现有业务 UI、Lua 皮肤与字体共用原项目实现。
  默认同时构建 arm64-v8a 手机和 x86_64 模拟器调试 APK。
  当前 UI 保持横屏和桌面布局，仅使用 SDL 标准输入，不做触控手势优化。
  工程目录、音频导入和谱面另存为已接异步 SAF；部分次要原生选择入口仍待迁移。
  Android 请求 Vulkan 1.3，桌面保持原要求；设备仍需满足实际使用的功能和格式。
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
  build/<ABI>          各 ABI 独立的原生 CMake/Ninja、依赖生成物与未剥离符号
  build/gradle         Java 与 APK 打包输出
  build/jni/<ABI>      各 ABI 的 main、SDL3 和 libc++ 共享库
  build/assets         受 Git 管理的默认资源 ZIP 与内容版本标记
  dependencies         Vulkan 公共头文件
  cache                临时文件、项目 Gradle 缓存与独立调试签名
  apk                  可安装的交付 APK
  evidence             本机验证日志和截图，构建脚本不强制生成验证记录
  Gradle 下载缓存默认共用工作目录同级 cache/gradle。
  可通过 MMM_ANDROID_GRADLE_CACHE、MMM_ANDROID_JAVA_HOME 和 MMM_ANDROID_JOBS 覆盖。
  MMM_ANDROID_ABIS="arm64-v8a" 可仅构建手机，默认 "arm64-v8a x86_64"。
  本机 SDK、NDK、JDK、下载缓存和构建输出不进入 Git；无需追加全局忽略规则。
  清理 build/ 不影响源码；保留 cache/debug.keystore 可保持重复安装签名一致。

安装与启动
  adb -s <设备序列号> install -r ~/Android/mmm-smoke/main/apk/MusicMapMaker-Next-android-debug.apk
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
    Gradle JDK 选 /usr/lib/jvm/openjdk-21，避免 VS Code 扩展自带的无 jlink JRE。
    Gradle user home 选 ~/Android/mmm-smoke/cache/gradle，共用下载缓存。
  Java 或 Manifest 修改后可由 Studio 打包；修改 C++ 后须先运行统一 build.sh。
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
