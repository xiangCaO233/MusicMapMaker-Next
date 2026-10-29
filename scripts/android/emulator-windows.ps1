<#
.SYNOPSIS
创建或启动单个精简 AOSP 模拟器，不安装 Android Studio 或 Google Play。
.PARAMETER SdkRoot
SDK 根目录，默认与 Windows 原生构建脚本共用用户 Android 目录。
.PARAMETER WorkDir
集中工作目录，默认仓库 build-android；AVD 数据与日志保存在该目录中。
.PARAMETER CreateOnly
仅创建配置，不启动模拟器进程。
.PARAMETER Headless
自动验证时显式无窗口启动；默认显示交互窗口，供用户直接操作。
#>
[CmdletBinding()]
param(
    [string]$SdkRoot = $env:ANDROID_HOME,
    [string]$WorkDir = $env:MMM_ANDROID_WORK_DIR,
    [switch]$CreateOnly,
    [switch]$Headless
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# 工具与输出路径统一为绝对路径，脚本命令仍在仓库根执行。
# 本入口不修改系统 PATH、Java 默认版本或用户其他 AVD。
$androidRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-Location -LiteralPath $androidRepo
if (!$SdkRoot) { $SdkRoot = Join-Path $env:USERPROFILE 'Android/Sdk' }
$SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
if (!$WorkDir) { $WorkDir = Join-Path $androidRepo 'build-android' }
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
# 模拟器磁盘不是源码，仓库内部只允许写入已 ignore 的 build-android。
# 比较使用完整目录前缀，避免把同名前缀的兄弟目录误判为源码子目录。
# 已规范化的路径也用于创建 AVD，使启动记录与实际磁盘目录保持一致。
# 仓库外的显式目录仍可使用，便于用户将大文件移到其他磁盘。
$androidAllowedBuild = Join-Path $androidRepo 'build-android'
if ($WorkDir -eq $androidRepo -or
    ($WorkDir.StartsWith($androidRepo.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -and
     $WorkDir -ne $androidAllowedBuild -and
     !$WorkDir.StartsWith($androidAllowedBuild + '\', [StringComparison]::OrdinalIgnoreCase))) {
    Write-Error 'Use build-android or a directory outside the source repository.'
    exit 1
}
$env:ANDROID_HOME = $SdkRoot
$env:ANDROID_AVD_HOME = Join-Path $WorkDir 'avd'
$androidEvidence = Join-Path $WorkDir 'evidence'
New-Item -ItemType Directory -Force -Path $env:ANDROID_AVD_HOME, $androidEvidence | Out-Null

# 沿用固定命令行工具，避免最新工具自动替换成另一套实验 CLI。
# AOSP API 36 与编译 SDK 36.1 分开安装；运行系统满足 minSdk 即可。
$androidManager = Join-Path $SdkRoot 'cmdline-tools/19.0/bin/avdmanager.bat'
$androidEmulator = Join-Path $SdkRoot 'emulator/emulator.exe'
$androidName = 'mmm-api36-minimal'
$androidAvd = Join-Path $env:ANDROID_AVD_HOME "$androidName.avd"
if (!(Test-Path -LiteralPath $androidManager) -or !(Test-Path -LiteralPath $androidEmulator)) {
    Write-Error 'Install command-line tools 19.0 and the Android emulator first.'
    exit 1
}
if (!(Test-Path -LiteralPath (Join-Path $androidAvd 'config.ini'))) {
    # 完整 JDK 只供配置工具使用，模拟器本身不依赖 Java。
    # JDK 发现规则与构建入口一致，不拾取编辑器内置的残缺 JRE。
    if (!$env:JAVA_HOME) {
        $androidJdks = @(Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE 'Android/jdk') -Directory |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin/javac.exe') })
        if ($androidJdks.Count -eq 1) { $env:JAVA_HOME = $androidJdks[0].FullName }
    }
    # 已有 AVD 不强制覆盖，保留应用私有工程和用户测试数据。
    # 首次创建才写资源上限；API 36 模拟器会将过低内存提升至 2560 MiB。
    'no' | & $androidManager create avd --name $androidName --package 'system-images;android-36;default;x86_64' --device pixel_2 --path $androidAvd
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $androidConfig = Join-Path $androidAvd 'config.ini'
    $androidValues = [ordered]@{
        'hw.cpu.ncore' = '2'; 'hw.ramSize' = '2560'; 'vm.heapSize' = '256'
        'hw.lcd.width' = '720'; 'hw.lcd.height' = '1280'; 'hw.lcd.density' = '160'
        'disk.dataPartition.size' = '2048M'; 'hw.sdCard' = 'no'; 'hw.keyboard' = 'yes'
        'hw.gpu.enabled' = 'yes'; 'hw.gpu.mode' = 'host'; 'showDeviceFrame' = 'no'
        'fastboot.forceColdBoot' = 'yes'; 'fastboot.forceFastBoot' = 'no'
    }
    # 移除同名旧项再追加，避免依赖 INI 重复键的覆盖顺序。
    # 禁用快照以节省磁盘；普通启动保留 userdata，不执行 wipe-data。
    $androidLines = @(Get-Content -LiteralPath $androidConfig | Where-Object {
        $androidKey = ($_ -split '=', 2)[0].Trim()
        !$androidValues.Contains($androidKey)
    })
    $androidLines += $androidValues.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }
    $androidLines | Set-Content -LiteralPath $androidConfig -Encoding utf8
}
if ($CreateOnly) { Write-Output "AVD: $androidAvd"; exit 0 }
# 加速不可用时明确失败，不能静默退成极慢的软件 CPU 模拟。
# 图形由宿主 Vulkan/驱动提供，后续以真实 APK 启动结果验证兼容性。
& $androidEmulator -accel-check
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$androidArguments = @('-avd', $androidName, '-port', '5554', '-gpu', 'host', '-no-snapshot', '-no-boot-anim', '-no-audio')
# 固定本测试实例的串口编号，后续安装命令不会误发到用户连接的真实手机。
# 先启动宿主 adb 服务，再让模拟器注册，避免首轮启动留下离线传输记录。
# 不重启已有服务，真实设备与其他客户端继续使用同一 adb 进程。
& (Join-Path $SdkRoot 'platform-tools/adb.exe') start-server
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Headless) { $androidArguments += '-no-window' }
$androidWindowStyle = if ($Headless) { 'Hidden' } else { 'Normal' }
Start-Process -FilePath $androidEmulator -ArgumentList $androidArguments -WindowStyle $androidWindowStyle -PassThru `
    -RedirectStandardOutput (Join-Path $androidEvidence 'emulator-stdout.log') `
    -RedirectStandardError (Join-Path $androidEvidence 'emulator-stderr.log') |
    Select-Object Id, ProcessName
