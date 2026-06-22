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

Invoke-Native git fetch --prune origin +refs/heads/ci:refs/remotes/origin/ci
Invoke-Native git checkout --force -B ci origin/ci
Invoke-Native git reset --hard origin/ci
Invoke-Native git lfs pull

Remove-Item -Recurse -Force -LiteralPath build_msvc -ErrorAction SilentlyContinue
Invoke-Native cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMMM_PGO_INSTRUMENT=OFF -DMMM_PGO_USE=OFF -S . -B build_msvc
Invoke-Native cmake --build build_msvc
Invoke-Native ctest --test-dir build_msvc --output-on-failure
