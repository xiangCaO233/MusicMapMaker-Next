#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/stage-msvc-clang-prebuilts.sh [options]

Copy clang-cl/MSVC source-build static libraries into the prebuilt layout.

Options:
  --build-dir <path>      Source build directory. Default: build_cross_msvc_sources
  --build-type <type>     Prebuilt config directory. Default: RelWithDebInfo
  --compiler-tag <tag>    Prebuilt compiler tag. Default: 2026
  --scope <all|main|ice>  Staging scope. Default: all
  --packages <list>       Comma-separated packages to stage. Default: all
  --strict-symbols        Fail when a Debug/RelWithDebInfo PDB cannot be found.
  --embedded-symbols      Remove stale PDBs because CodeView is embedded in archives.
  -h, --help              Show this help

Environment overrides:
  MSVC_PREBUILT_COMPILER_TAG  Default prebuilt compiler tag
EOF
}

projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        printf "%s\n" "${inputPath}"
    else
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

findSourceFile() {
    local outVar="$1"
    shift

    local resolvedPath=""
    local candidatePath
    for candidatePath in "$@"; do
        if [[ -f "${buildDir}/${candidatePath}" ]]; then
            resolvedPath="${buildDir}/${candidatePath}"
            break
        fi
    done

    printf -v "${outVar}" "%s" "${resolvedPath}"
}

copyLib() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local outputName="$3"
    shift 3

    if ! shouldStagePackage "${packageName}"; then
        return 0
    fi

    local sourcePath=""
    findSourceFile sourcePath "$@"

    if [[ -z "${sourcePath}" ]]; then
        printf "error: source library not found for %s/%s\n" "${packageName}" "${outputName}" >&2
        printf "searched:\n" >&2
        local searchedPath
        for searchedPath in "$@"; do
            printf "  %s/%s\n" "${buildDir}" "${searchedPath}" >&2
        done
        exit 1
    fi

    local outputPath="${prebuiltRoot}/binaries/windows/${packageName}/libs/x86_64/msvc/${compilerTag}/${buildType}/${outputName}"
    install -D -m 0644 "${sourcePath}" "${outputPath}"
    printf "staged %s\n" "${outputPath#${projectRoot}/}"
}

findPdbByName() {
    local outVar="$1"
    shift

    local foundPath=""
    local pdbName
    for pdbName in "$@"; do
        while IFS= read -r -d '' foundPath; do
            printf -v "${outVar}" "%s" "${foundPath}"
            return
        done < <(find "${buildDir}" -type f -name "${pdbName}" -print0 -quit)
    done

    printf -v "${outVar}" "%s" ""
}

copyPdb() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local outputName="$3"
    shift 3

    if ! shouldStagePackage "${packageName}"; then
        return 0
    fi

    case "${buildType}" in
        Debug | RelWithDebInfo)
            ;;
        *)
            return 0
            ;;
    esac

    local outputPath="${prebuiltRoot}/binaries/windows/${packageName}/symbols/x86_64/msvc/${compilerTag}/${buildType}/${outputName}"
    if (( embeddedSymbols )); then
        if [[ -f "${outputPath}" ]]; then
            rm -f -- "${outputPath}"
            printf "removed stale %s\n" "${outputPath#${projectRoot}/}"
        fi
        return 0
    fi

    local sourcePath=""
    findPdbByName sourcePath "$@"

    if [[ -z "${sourcePath}" ]]; then
        local message="warning: PDB not found for ${packageName}/${outputName}"
        if (( strictSymbols )); then
            printf "error: %s\n" "${message#warning: }" >&2
            exit 1
        fi
        printf "%s\n" "${message}" >&2
        return 0
    fi

    install -D -m 0644 "${sourcePath}" "${outputPath}"
    printf "staged %s\n" "${outputPath#${projectRoot}/}"
}

copyMainLib() {
    if [[ "${stageScope}" = "ice" ]]; then
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/prebuilts" "$@"
}

copyIceLib() {
    if [[ "${stageScope}" = "main" ]]; then
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts" "$@"
}

copyMainPdb() {
    if [[ "${stageScope}" = "ice" ]]; then
        return 0
    fi

    copyPdb "${projectRoot}/3rdpty/prebuilts" "$@"
}

copyIcePdb() {
    if [[ "${stageScope}" = "main" ]]; then
        return 0
    fi

    copyPdb "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts" "$@"
}

stageMainLibWithPdb() {
    local packageName="$1"
    local outputName="$2"
    local pdbName="$3"
    shift 3

    copyMainLib "${packageName}" "${outputName}" "$@"
    copyMainPdb "${packageName}" "${pdbName}" "${pdbName}"
}

stageIceLibWithPdb() {
    local packageName="$1"
    local outputName="$2"
    local pdbName="$3"
    shift 3

    copyIceLib "${packageName}" "${outputName}" "$@"
    copyIcePdb "${packageName}" "${pdbName}" "${pdbName}"
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

shouldStagePackage() {
    local packageName="$1"

    [[ -z "${packageFilter}" || ",${packageFilter}," == *",${packageName},"* ]]
}

buildDir="build_cross_msvc_sources"
buildType="RelWithDebInfo"
compilerTag="${MSVC_PREBUILT_COMPILER_TAG:-2026}"
stageScope="all"
packageFilter=""
strictSymbols=0
embeddedSymbols=0

while (( $# > 0 )); do
    case "$1" in
        --build-dir)
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --build-type)
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --compiler-tag)
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --scope)
            if (( $# < 2 )); then
                printf "error: --scope requires a value\n" >&2
                exit 1
            fi
            stageScope="$2"
            shift 2
            ;;
        --packages)
            if (( $# < 2 )); then
                printf "error: --packages requires a value\n" >&2
                exit 1
            fi
            packageFilter="$2"
            shift 2
            ;;
        --strict-symbols)
            strictSymbols=1
            shift
            ;;
        --embedded-symbols)
            embeddedSymbols=1
            shift
            ;;
        -h | --help)
            showUsage
            exit 0
            ;;
        *)
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

if [[ -z "${compilerTag}" ]]; then
    printf "error: --compiler-tag must not be empty\n" >&2
    exit 1
fi

if [[ -n "${packageFilter}" && ",${packageFilter}," == *",,"* ]]; then
    printf "error: --packages contains an empty package name: %s\n" "${packageFilter}" >&2
    exit 1
fi

case "${stageScope}" in
    all | main | ice)
        ;;
    *)
        printf "error: --scope must be one of all, main, ice: %s\n" "${stageScope}" >&2
        exit 1
        ;;
esac

if (( strictSymbols && embeddedSymbols )); then
    printf "error: --strict-symbols and --embedded-symbols cannot be used together\n" >&2
    exit 1
fi

buildDir="$(projectPath "${buildDir}")"

if [[ ! -d "${buildDir}" ]]; then
    printf "error: build directory not found: %s\n" "${buildDir}" >&2
    exit 1
fi

fmtOutputName="fmt.lib"
fmtPdbName="fmt.pdb"
freetypeOutputName="freetype.lib"
freetypePdbName="freetype.pdb"
spdlogOutputName="spdlog.lib"
spdlogPdbName="spdlog.pdb"
zlibOutputName="libzs.lib"

case "${buildType}" in
    Debug | debug)
        fmtOutputName="fmtd.lib"
        fmtPdbName="fmtd.pdb"
        freetypeOutputName="freetyped.lib"
        freetypePdbName="freetype.pdb"
        spdlogOutputName="spdlogd.lib"
        spdlogPdbName="spdlog.pdb"
        zlibOutputName="libzsd.lib"
        ;;
esac

stageMainLibWithPdb "ImGuiFileDialog" "ImGuiFileDialog.lib" "ImGuiFileDialog.pdb" "lib/ImGuiFileDialog.lib"
stageMainLibWithPdb "IonCachyEngine" "IonCachyEngine-static.lib" "IonCachyEngine-static.pdb" "lib/IonCachyEngine-static.lib"
stageMainLibWithPdb "curl" "libcurl.lib" "libcurl.pdb" "lib/libcurl.lib" "lib/libcurl_static.lib"
stageMainLibWithPdb "ffmpeg" "avcodec.lib" "avcodec.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avcodec.lib"
stageMainLibWithPdb "ffmpeg" "avformat.lib" "avformat.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avformat.lib"
stageMainLibWithPdb "ffmpeg" "avutil.lib" "avutil.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avutil.lib"
stageMainLibWithPdb "ffmpeg" "swresample.lib" "swresample.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/swresample.lib"
stageMainLibWithPdb "ffmpeg" "swscale.lib" "swscale.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/swscale.lib"
stageMainLibWithPdb "fftw" "fftw3.lib" "fftw3.pdb" "3rdpty/fftw_inst/lib/fftw3.lib"
stageMainLibWithPdb "fmt" "${fmtOutputName}" "${fmtPdbName}" "lib/${fmtOutputName}" "lib/fmt.lib" "lib/fmtd.lib"
stageMainLibWithPdb "freetype" "${freetypeOutputName}" "${freetypePdbName}" "lib/${freetypeOutputName}" "lib/freetype.lib" "lib/freetyped.lib"
stageMainLibWithPdb "glfw" "glfw3.lib" "glfw.pdb" "lib/glfw3.lib" "lib/glfw.lib"
stageMainLibWithPdb "imgui" "imgui-static.lib" "imgui-static.pdb" "lib/imgui-static.lib"
stageMainLibWithPdb "implot" "3rd_implot.lib" "3rd_implot.pdb" "lib/3rd_implot.lib"
stageMainLibWithPdb "lame" "libmp3lame-static.lib" "mp3lame.pdb" "3rdpty/lame_inst/lib/mp3lame.lib"
stageMainLibWithPdb "libsamplerate" "samplerate.lib" "samplerate.pdb" "3rdpty/libsamplerate/samplerate.lib" "lib/samplerate.lib"
stageMainLibWithPdb "luajit" "lua51.lib" "lua51.pdb" "luajit/src/libluajit.a"
stageMainLibWithPdb "lunasvg" "lunasvg.lib" "lunasvg.pdb" "lib/lunasvg.lib"
stageMainLibWithPdb "lunasvg" "plutovg.lib" "plutovg.pdb" "lib/plutovg.lib"
stageMainLibWithPdb "miniz" "3rd_miniz.lib" "3rd_miniz.pdb" "lib/3rd_miniz.lib"
stageMainLibWithPdb "nativefiledialog-extended" "nfd.lib" "nfd.pdb" "lib/nfd.lib"
stageMainLibWithPdb "openal" "OpenAL32.lib" "OpenAL.pdb" "lib/OpenAL32.lib" "lib/OpenAL.lib"
stageMainLibWithPdb "rubberband" "rubberband-static.lib" "rubberband-static.pdb" "rb_inst/lib/rubberband-static.lib" "rb_inst/lib/rubberband.lib" "rb_inst/lib/librubberband.a" "lib/rubberband-static.lib"
stageMainLibWithPdb "sdl" "SDL3-static.lib" "SDL3-static.pdb" "lib/SDL3-static.lib" "lib/SDL3.lib"
stageMainLibWithPdb "spdlog" "${spdlogOutputName}" "${spdlogPdbName}" "lib/${spdlogOutputName}" "lib/spdlog.lib" "lib/spdlogd.lib"
stageMainLibWithPdb "zlib" "${zlibOutputName}" "zlib.pdb" "3rdpty/zlib_inst/lib/libz.lib"

stageIceLibWithPdb "ffmpeg" "avcodec.lib" "avcodec.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avcodec.lib"
stageIceLibWithPdb "ffmpeg" "avformat.lib" "avformat.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avformat.lib"
stageIceLibWithPdb "ffmpeg" "avutil.lib" "avutil.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avutil.lib"
stageIceLibWithPdb "ffmpeg" "swresample.lib" "swresample.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/swresample.lib"
stageIceLibWithPdb "ffmpeg" "swscale.lib" "swscale.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/swscale.lib"
stageIceLibWithPdb "fftw" "fftw3.lib" "fftw3.pdb" "3rdpty/fftw_inst/lib/fftw3.lib"
stageIceLibWithPdb "fmt" "${fmtOutputName}" "${fmtPdbName}" "lib/${fmtOutputName}" "lib/fmt.lib" "lib/fmtd.lib"
stageIceLibWithPdb "lame" "libmp3lame-static.lib" "mp3lame.pdb" "3rdpty/lame_inst/lib/mp3lame.lib"
stageIceLibWithPdb "libsamplerate" "samplerate.lib" "samplerate.pdb" "3rdpty/libsamplerate/samplerate.lib" "lib/samplerate.lib"
stageIceLibWithPdb "openal" "OpenAL32.lib" "OpenAL.pdb" "lib/OpenAL32.lib" "lib/OpenAL.lib"
stageIceLibWithPdb "rubberband" "rubberband-static.lib" "rubberband-static.pdb" "rb_inst/lib/rubberband-static.lib" "rb_inst/lib/rubberband.lib" "rb_inst/lib/librubberband.a" "lib/rubberband-static.lib"
stageIceLibWithPdb "sdl" "SDL3-static.lib" "SDL3-static.pdb" "lib/SDL3-static.lib" "lib/SDL3.lib"
stageIceLibWithPdb "spdlog" "${spdlogOutputName}" "${spdlogPdbName}" "lib/${spdlogOutputName}" "lib/spdlog.lib" "lib/spdlogd.lib"
stageIceLibWithPdb "zlib" "${zlibOutputName}" "zlib.pdb" "3rdpty/zlib_inst/lib/libz.lib"
