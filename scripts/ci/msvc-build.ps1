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

Invoke-Native git fetch --prune origin +refs/heads/ci:refs/remotes/origin/ci
Invoke-Native git checkout --force -B ci origin/ci
Invoke-Native git reset --hard origin/ci
Invoke-Native git submodule update --init --recursive
Invoke-Native git lfs pull

Remove-Item -Recurse -Force -LiteralPath build_msvc -ErrorAction SilentlyContinue
Invoke-Native cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMMM_PGO_INSTRUMENT=OFF -DMMM_PGO_USE=OFF -S . -B build_msvc
Invoke-Native cmake --build build_msvc --parallel $ciBuildJobs
Invoke-Native ctest --test-dir build_msvc --output-on-failure
