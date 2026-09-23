# 在原生 MSVC Runner 上拉取 msvc/2026 预编译依赖并运行完整测试。
# 所有外部命令通过统一 helper 传播退出码，避免 PowerShell 静默继续。
# build_msvc 是可重建的专用构建树，不与 MinGW 或本机构建共享缓存。
param(
    [switch]$VulkanValidationLayers
)

$ErrorActionPreference = 'Stop'

# 执行原生命令并在非零退出时立即以相同状态结束脚本。
function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    # 数组展开保留每个命令行参数的边界。
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        # 外层 CI 需要看到具体工具退出码，而不是通用 PowerShell 异常码。
        exit $LASTEXITCODE
    }
}

function Get-CiBuildJobs {
    # 保留四分之一逻辑核心给链接器峰值和 Runner 后台服务。
    $maxThreads = [Environment]::ProcessorCount
    if ($maxThreads -lt 1) {
        # 异常主机信息回退到单线程。
        $maxThreads = 1
    }

    $buildJobs = [Math]::Floor($maxThreads * 3 / 4)
    if ($buildJobs -lt 1) {
        # 单核机器经向下取整后仍至少使用一个构建 job。
        $buildJobs = 1
    }

    # 显式整数返回，避免 --parallel 接收浮点文本。
    return [int]$buildJobs
}

# 并发度在开始下载依赖前确定。
$ciBuildJobs = Get-CiBuildJobs

# LFS include 精确覆盖 MSVC 2026、资源、测试夹具和 Windows 图标。
$mainLfsIncludes = '3rdpty/prebuilts/headers/**,3rdpty/prebuilts/binaries/windows/*/libs/x86_64/msvc/2026/RelWithDebInfo/**,assets/**,tests/data/**,Modules/Main/src/logo.svg'
Invoke-Native git lfs pull "--include=$mainLfsIncludes" '--exclude='

# 构建树属于 CI 临时产物，删除时使用 LiteralPath 防止通配符展开。
Remove-Item -Recurse -Force -LiteralPath build_msvc -ErrorAction SilentlyContinue
# CI 构建不得写入 Runner 的用户配置目录。
$vulkanValidationValue = if ($VulkanValidationLayers) { 'ON' } else { 'OFF' }
Invoke-Native cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSOURCES_BUILD=OFF -DBUILD_TESTING=ON "-DMMM_ENABLE_VULKAN_VALIDATION_LAYERS=$vulkanValidationValue" -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF -DMMM_PGO_INSTRUMENT=OFF -DMMM_PGO_USE=OFF -S . -B build_msvc
# 构建与 CTest 都通过 helper 严格传播原生工具失败。
Invoke-Native cmake --build build_msvc --parallel $ciBuildJobs
Invoke-Native ctest --test-dir build_msvc --output-on-failure
