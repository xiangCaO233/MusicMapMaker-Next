<#
.SYNOPSIS
在 Windows 原生环境中构建安卓 APK，使用 NDK 和 MSYS2，不依赖 WSL。
.PARAMETER Abi
仅构建所列 ABI，默认 x86_64 供模拟器使用，避免同时生成两套原生依赖。
.PARAMETER SdkRoot
Android SDK 根目录；优先使用 ANDROID_HOME，否则采用用户 Android 目录。
.PARAMETER JavaHome
完整 JDK 21 所在目录；不接受仅含运行时的编辑器内置 JRE。
.PARAMETER WorkDir
集中构建目录；默认位于仓库 build-android，各 ABI、缓存、资源和 APK 均放在其中。
.PARAMETER MsysRoot
既有 MSYS2 安装目录，只借用宿主工具，最终安卓库必须全部由 NDK 编译。
.PARAMETER Jobs
外层原生构建的并发数，不改变用户的桌面构建目录或系统环境变量。
#>
[CmdletBinding()]
param(
    [ValidateSet('x86_64', 'arm64-v8a')][string[]]$Abi = @('x86_64'),
    [string]$SdkRoot = $env:ANDROID_HOME,
    [string]$JavaHome = $env:MMM_ANDROID_JAVA_HOME,
    [string]$WorkDir = $env:MMM_ANDROID_WORK_DIR,
    [string]$MsysRoot = 'C:/msys64',
    [ValidateRange(1, 128)][int]$Jobs = 12
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

<#
.SYNOPSIS
执行构建工具并立即传播失败，不继续用旧共享库或旧 APK 打包。
.PARAMETER Program
已经解析的宿主可执行文件或批处理入口，路径和参数分开传递。
.PARAMETER Arguments
原样传入的参数列表；禁止拼接为另一个 shell 的可执行代码。
#>
function Invoke-AndroidTool {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# 所有命令在源码根执行，所有派生文件使用绝对路径并收纳到专用构建树。
# 默认复用标准 SDK；构建目录单独集中，便于统计空间和安全清理。
$androidRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-Location -LiteralPath $androidRepo
if (!$SdkRoot) { $SdkRoot = Join-Path $env:USERPROFILE 'Android/Sdk' }
$SdkRoot = [IO.Path]::GetFullPath($SdkRoot).Replace('\', '/')
if (!$WorkDir) { $WorkDir = Join-Path $androidRepo 'build-android' }
$WorkDir = [IO.Path]::GetFullPath($WorkDir).Replace('\', '/')
$androidSourcePrefix = $androidRepo.TrimEnd('/', '\') + [IO.Path]::DirectorySeparatorChar
$androidAllowedBuild = [IO.Path]::GetFullPath((Join-Path $androidRepo 'build-android'))
$androidWorkNative = $WorkDir.Replace('/', '\')
# 仓库内部只允许已 ignore 的专用目录，不能把根目录或业务源码目录作为输出。
# 路径先规范化再比较，防止相对路径中的 .. 绕过输出边界。
if ($androidWorkNative -eq $androidRepo -or
    ($androidWorkNative.StartsWith($androidSourcePrefix, [StringComparison]::OrdinalIgnoreCase) -and
     $androidWorkNative -ne $androidAllowedBuild -and
     !$androidWorkNative.StartsWith($androidAllowedBuild + '\', [StringComparison]::OrdinalIgnoreCase))) {
    Write-Error 'Use build-android or a directory outside the source repository.'
    exit 1
}

# 仅选择包含 javac 的完整 JDK；用户显式指定时不能悄悄换成其他安装。
# 默认安装目录只用于发现本任务的便携 JDK，不改写系统 Java 配置。
if (!$JavaHome) {
    $androidJdkRoot = Join-Path $env:USERPROFILE 'Android/jdk'
    $androidJdks = @(Get-ChildItem -LiteralPath $androidJdkRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin/javac.exe') })
    if ($androidJdks.Count -eq 1) { $JavaHome = $androidJdks[0].FullName }
}
if (!$JavaHome -or !(Test-Path -LiteralPath (Join-Path $JavaHome 'bin/javac.exe'))) {
    Write-Error 'Install a complete JDK 21 and pass -JavaHome or MMM_ANDROID_JAVA_HOME.'
    exit 1
}
$JavaHome = [IO.Path]::GetFullPath($JavaHome).Replace('\', '/')
# 版本约束在原生编译前检查，不能等到 Gradle 才发现 daemon 使用了错误 JDK。
$androidJavacVersion = & "$JavaHome/bin/javac.exe" -version
if ($LASTEXITCODE -ne 0 -or $androidJavacVersion -notmatch '^javac 21(\.|$)') {
    Write-Error 'Android builds require JDK 21.'
    exit 1
}
$androidNdk = "$SdkRoot/ndk/30.0.16248370"
$androidNdkBin = "$androidNdk/toolchains/llvm/prebuilt/windows-x86_64/bin"
# NDK、SDK 平台与 Build Tools 沿用 Linux 脚本的固定版本。
# 缺包时明确失败，不回退到桌面预编译库或另一版 NDK。
foreach ($androidRequired in @("$androidNdk/build/cmake/android.toolchain.cmake", "$SdkRoot/build-tools/36.1.0/apksigner.bat", "$MsysRoot/usr/bin/sh.exe", "$MsysRoot/usr/bin/make.exe")) {
    if (!(Test-Path -LiteralPath $androidRequired)) {
        Write-Error "Missing Android build prerequisite: $androidRequired"
        exit 1
    }
}

# 宿主工具仅通过当前进程 PATH 提供，子进程继承同一环境。
# MSYS2 提供 configure/make；CMake 与 Ninja 仍为 Windows 原生程序。
$env:PATH = "$MsysRoot/clang64/bin;$MsysRoot/usr/bin;$env:PATH"
$env:ANDROID_HOME = $SdkRoot
$env:ANDROID_SDK_ROOT = $SdkRoot
$env:JAVA_HOME = $JavaHome
$env:GRADLE_USER_HOME = "$WorkDir/cache/gradle-user"
$env:TEMP = "$WorkDir/cache/tmp"
$env:TMP = $env:TEMP
# Meson 的目标安装前缀必须保留为 /，不能被 MSYS2 改成宿主根目录。
# 其余 /c/... 文件路径仍须转换，供 Windows 原生 NDK 编译器和归档器读取。
$env:MSYS2_ARG_CONV_EXCL = '--prefix=/'
foreach ($androidFolder in @('cache/tmp', 'dependencies', 'build/assets', 'build/jni', 'apk', 'evidence')) {
    New-Item -ItemType Directory -Force -Path "$WorkDir/$androidFolder" | Out-Null
}
$androidCmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$androidPython = "$MsysRoot/clang64/bin/python.exe"
Invoke-AndroidTool "$JavaHome/bin/java.exe" @('-version')

# Vulkan-Hpp 使用与 Linux 相同的固定官方标签，NDK 自带 C 头不能替代它。
# 下载只进入集中依赖目录，不在第三方源码中产生下游补丁。
$androidVulkanTag = 'vulkan-sdk-1.4.363.0'
$androidHeaders = "$WorkDir/dependencies/Vulkan-Headers"
if (!(Test-Path -LiteralPath "$androidHeaders/include/vulkan/vulkan.hpp")) {
    $androidArchive = "$WorkDir/cache/Vulkan-Headers-$androidVulkanTag.tar.gz"
    Invoke-AndroidTool 'curl.exe' @('--fail', '--location', '--retry', '3', '--output', $androidArchive, "https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/$androidVulkanTag")
    New-Item -ItemType Directory -Force -Path $androidHeaders | Out-Null
    # 明确使用 Windows 自带 tar，MSYS tar 会把 C: 误解释为远程主机。
    Invoke-AndroidTool "$env:SystemRoot/System32/tar.exe" @('-xzf', $androidArchive, '-C', $androidHeaders, '--strip-components=1')
}

# ABI 构建树彼此隔离；默认仅生成模拟器包，手机包须显式选择 arm64-v8a。
# 所有原生依赖按本次 NDK 源码重建，不用 Windows 或 Linux 桌面归档冒充。
foreach ($androidAbi in $Abi) {
    $androidNative = "$WorkDir/build/$androidAbi"
    $androidTriple = if ($androidAbi -eq 'x86_64') { 'x86_64-linux-android' } else { 'aarch64-linux-android' }
    Invoke-AndroidTool $androidCmake @('-S', $androidRepo, '-B', $androidNative, '-G', 'Ninja',
        "-DCMAKE_TOOLCHAIN_FILE=$androidNdk/build/cmake/android.toolchain.cmake",
        "-DANDROID_ABI=$androidAbi", '-DANDROID_PLATFORM=android-28', '-DANDROID_STL=c++_shared',
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DSOURCES_BUILD=ON', '-DBUILD_TESTING=OFF',
        '-DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF', '-DMMM_DISABLE_CLANG_LTO=ON',
        '-DMMM_PGO_INSTRUMENT=OFF', '-DMMM_PGO_USE=OFF', "-DVulkan_INCLUDE_DIR=$androidHeaders/include",
        # 外部 Make 工程不读取 Ninja 的 --parallel，须显式传入同一并发上限。
        "-DPROCESSOR_COUNT=$Jobs")
    Invoke-AndroidTool $androidCmake @('--build', $androidNative, '--target', 'MusicMapMaker-Next', '--parallel', "$Jobs")
    $androidJni = "$WorkDir/build/jni/$androidAbi"
    New-Item -ItemType Directory -Force -Path $androidJni | Out-Null
    # 运行库严格来自 CMake 清单，缺任意文件都停止，不打包历史构建残留。
    # 标准库按当前 ABI 的 triple 查找，不能复用另一个架构的 libc++。
    foreach ($androidLibrary in Get-Content -LiteralPath "$androidNative/android-runtime-libraries.txt") {
        if (!(Test-Path -LiteralPath $androidLibrary)) { Write-Error "Missing runtime: $androidLibrary"; exit 1 }
        Copy-Item -LiteralPath $androidLibrary -Destination $androidJni
    }
    Copy-Item -LiteralPath "$androidNdk/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/$androidTriple/libc++_shared.so" -Destination $androidJni
}

# 资源与调试签名共用工作目录；用户工程与正式发布签名不属于缓存。
# 资源脚本检查 LFS 内容，不能将指针文件当图片或字体放入 APK。
Invoke-AndroidTool $androidPython @("$androidRepo/scripts/android/package_assets.py", '--repo', $androidRepo, '--output', "$WorkDir/build/assets")
if (!(Test-Path -LiteralPath "$WorkDir/cache/debug.keystore")) {
    Invoke-AndroidTool "$JavaHome/bin/keytool.exe" @('-genkeypair', '-keystore', "$WorkDir/cache/debug.keystore",
        '-storepass', 'android', '-keypass', 'android', '-alias', 'androiddebugkey', '-keyalg', 'RSA',
        '-keysize', '2048', '-validity', '10000', '-dname', 'CN=MusicMapMaker Android Dev,O=Local,C=CN')
}
# Java 外壳与本次 SDL 子模块成对，不从 Maven 混入另一版本的 SDL 运行库。
# Gradle 缓存显式集中到工作目录，避免源码根或打包源码目录生成 .gradle/.cxx。
$androidSdlJava = "$androidRepo/3rdpty/sources/IonCachyEngine/3rdpty/sources/sdl/android-project/app/src/main/java"
Invoke-AndroidTool "$androidRepo/packaging/android/gradlew.bat" @('-p', "$androidRepo/packaging/android",
    '--project-cache-dir', "$WorkDir/cache/gradle-project", '--no-daemon',
    "-PmmmAndroidWorkDir=$WorkDir", "-PmmmSdlJavaDir=$androidSdlJava", "-PmmmAndroidAbis=$($Abi -join ',')", ':app:assembleDebug')
$androidApk = "$WorkDir/apk/MusicMapMaker-Next-android-debug.apk"
Copy-Item -LiteralPath "$WorkDir/build/gradle/app/outputs/apk/debug/app-debug.apk" -Destination $androidApk
Invoke-AndroidTool "$SdkRoot/build-tools/36.1.0/apksigner.bat" @('verify', $androidApk)
Write-Output "APK: $androidApk"
