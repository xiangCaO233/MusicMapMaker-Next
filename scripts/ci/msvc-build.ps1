$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Get-CiBuildJobs {
    $maxThreads = [Environment]::ProcessorCount
    if ($maxThreads -lt 1) {
        $maxThreads = 1
    }

    $buildJobs = [Math]::Floor($maxThreads * 3 / 4)
    if ($buildJobs -lt 1) {
        $buildJobs = 1
    }

    return [int]$buildJobs
}

$ciBuildJobs = Get-CiBuildJobs

$mainLfsIncludes = '3rdpty/prebuilts/headers/**,3rdpty/prebuilts/binaries/windows/*/libs/x86_64/msvc/2026/RelWithDebInfo/**,assets/**,tests/data/**,Modules/Main/src/logo.svg'
Invoke-Native git lfs pull "--include=$mainLfsIncludes" '--exclude='

Remove-Item -Recurse -Force -LiteralPath build_msvc -ErrorAction SilentlyContinue
# CI 构建不得写入 Runner 的用户配置目录。
Invoke-Native cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSOURCES_BUILD=OFF -DBUILD_TESTING=ON -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF -DMMM_PGO_INSTRUMENT=OFF -DMMM_PGO_USE=OFF -S . -B build_msvc
Invoke-Native cmake --build build_msvc --parallel $ciBuildJobs
Invoke-Native ctest --test-dir build_msvc --output-on-failure
