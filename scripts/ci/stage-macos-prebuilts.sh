#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/stage-macos-prebuilts.sh [options]

Copy native macOS source-build static libraries into the prebuilt layout.

Options:
  --build-dir <path>      Source build directory. Default: build_macos_sources_<arch>
  --build-type <type>     Prebuilt config directory. Default: RelWithDebInfo
  --arch <arm64|x86_64>   Prebuilt architecture directory. Default: native architecture
  --toolchain <name>      Prebuilt toolchain directory. Default: clang
  --compiler-tag <tag>    Prebuilt compiler tag. Default: detected clang major
  --scope <all|main|ice>  Staging scope. Default: all
  -h, --help              Show this help

Environment overrides:
  MACOS_PREBUILT_ARCH          Default prebuilt architecture directory
  MACOS_PREBUILT_TOOLCHAIN     Default prebuilt toolchain directory
  MACOS_PREBUILT_COMPILER_TAG  Default prebuilt compiler tag
EOF
}

normalizeArchitecture() {
    case "$1" in
        arm64 | aarch64)
            printf "arm64\n"
            ;;
        x86_64 | amd64)
            printf "x86_64\n"
            ;;
        *)
            printf "error: unsupported macOS architecture: %s\n" "$1" >&2
            exit 1
            ;;
    esac
}

projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        printf "%s\n" "${inputPath}"
    else
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

detectClangCompilerTag() {
    local cxxCompiler
    cxxCompiler="$(xcrun --find clang++)"

    local macroOutput
    if ! macroOutput="$(printf "\n" | "${cxxCompiler}" -dM -E -x c++ - 2>/dev/null)"; then
        printf "error: failed to query Clang predefined macros: %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    local clangMajor
    clangMajor="$(awk '$2 == "__clang_major__" { print $3; exit }' <<<"${macroOutput}")"
    if [[ ! "${clangMajor}" =~ ^[0-9]+$ ]]; then
        printf "error: failed to detect Clang major version from: %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    printf "clang%s\n" "${clangMajor}"
}

verifyArchitecture() {
    local sourcePath="$1"
    local architectures

    if ! architectures="$(xcrun lipo -archs "${sourcePath}" 2>/dev/null)"; then
        printf "error: failed to inspect library architecture: %s\n" "${sourcePath}" >&2
        exit 1
    fi

    if [[ " ${architectures} " != *" ${targetArch} "* ]]; then
        printf "error: library does not contain %s architecture: %s (%s)\n" \
            "${targetArch}" "${sourcePath}" "${architectures}" >&2
        exit 1
    fi
}

copyLib() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local outputName="$3"
    shift 3

    local sourcePath=""
    local candidatePath
    for candidatePath in "$@"; do
        candidatePath="${buildDir}/${candidatePath}"
        if [[ -f "${candidatePath}" ]]; then
            sourcePath="${candidatePath}"
            break
        fi
    done

    if [[ -z "${sourcePath}" ]]; then
        printf "error: source library not found for %s/%s\n" "${packageName}" "${outputName}" >&2
        printf "searched:\n" >&2
        local searchedPath
        for searchedPath in "$@"; do
            printf "  %s/%s\n" "${buildDir}" "${searchedPath}" >&2
        done
        exit 1
    fi

    verifyArchitecture "${sourcePath}"

    local outputPath="${prebuiltRoot}/binaries/macos/${packageName}/libs/${targetArch}/${prebuiltToolchain}/${compilerTag}/${buildType}/${outputName}"
    mkdir -p "$(dirname "${outputPath}")"
    install -m 0644 "${sourcePath}" "${outputPath}"
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

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf "error: scripts/ci/stage-macos-prebuilts.sh must run on macOS\n" >&2
    exit 1
fi

if ! command -v xcrun >/dev/null 2>&1; then
    printf "error: required command not found: xcrun\n" >&2
    exit 1
fi

buildDir=""
buildType="RelWithDebInfo"
targetArch="${MACOS_PREBUILT_ARCH:-$(uname -m)}"
prebuiltToolchain="${MACOS_PREBUILT_TOOLCHAIN:-clang}"
compilerTag="${MACOS_PREBUILT_COMPILER_TAG:-}"
stageScope="all"

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
        --arch)
            if (( $# < 2 )); then
                printf "error: --arch requires a value\n" >&2
                exit 1
            fi
            targetArch="$2"
            shift 2
            ;;
        --toolchain)
            if (( $# < 2 )); then
                printf "error: --toolchain requires a value\n" >&2
                exit 1
            fi
            prebuiltToolchain="$2"
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

targetArch="$(normalizeArchitecture "${targetArch}")"

if [[ -z "${buildDir}" ]]; then
    buildDir="build_macos_sources_${targetArch}"
fi
if [[ -z "${compilerTag}" ]]; then
    compilerTag="$(detectClangCompilerTag)"
fi

if [[ -z "${prebuiltToolchain}" || -z "${compilerTag}" ]]; then
    printf "error: prebuilt toolchain and compiler tag must not be empty\n" >&2
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

buildDir="$(projectPath "${buildDir}")"

if [[ ! -d "${buildDir}" ]]; then
    printf "error: build directory not found: %s\n" "${buildDir}" >&2
    exit 1
fi

fmtOutputName="libfmt.a"
freetypeOutputName="libfreetype.a"
spdlogOutputName="libspdlog.a"

case "${buildType}" in
    Debug | debug)
        fmtOutputName="libfmtd.a"
        freetypeOutputName="libfreetyped.a"
        spdlogOutputName="libspdlogd.a"
        ;;
esac

copyMainLib "ImGuiFileDialog" "libImGuiFileDialog.a" "lib/libImGuiFileDialog.a"
copyMainLib "IonCachyEngine" "libIonCachyEngine-static.a" "lib/libIonCachyEngine-static.a"
copyMainLib "curl" "libcurl.a" "lib/libcurl.a" "lib/libcurl_static.a"
copyMainLib "ffmpeg" "libavcodec.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavcodec.a"
copyMainLib "ffmpeg" "libavformat.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a"
copyMainLib "ffmpeg" "libavutil.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a"
copyMainLib "ffmpeg" "libswresample.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a"
copyMainLib "ffmpeg" "libswscale.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a"
copyMainLib "fftw" "libfftw3.a" "3rdpty/fftw_inst/lib/libfftw3.a"
copyMainLib "fmt" "${fmtOutputName}" "lib/${fmtOutputName}" "lib/libfmt.a" "lib/libfmtd.a"
copyMainLib "freetype" "${freetypeOutputName}" "lib/${freetypeOutputName}" "lib/libfreetype.a" "lib/libfreetyped.a"
copyMainLib "glfw" "libglfw3.a" "lib/libglfw3.a" "lib/libglfw.a"
copyMainLib "imgui" "libimgui-static.a" "lib/libimgui-static.a"
copyMainLib "implot" "lib3rd_implot.a" "lib/lib3rd_implot.a"
copyMainLib "lame" "libmp3lame.a" "3rdpty/lame_inst/lib/libmp3lame.a"
copyMainLib "libsamplerate" "libsamplerate.a" "3rdpty/libsamplerate/libsamplerate.a"
copyMainLib "luajit" "libluajit.a" "luajit/src/libluajit.a"
copyMainLib "lunasvg" "liblunasvg.a" "lib/liblunasvg.a"
copyMainLib "lunasvg" "libplutovg.a" "lib/libplutovg.a"
copyMainLib "miniz" "lib3rd_miniz.a" "lib/lib3rd_miniz.a"
copyMainLib "nativefiledialog-extended" "libnfd.a" "lib/libnfd.a"
copyMainLib "openal" "libopenal.a" "lib/libopenal.a" "lib/libOpenAL.a"
copyMainLib "rubberband" "librubberband.a" "rb_inst/lib/librubberband.a"
copyMainLib "sdl" "libSDL3.a" "lib/libSDL3.a"
copyMainLib "spdlog" "${spdlogOutputName}" "lib/${spdlogOutputName}" "lib/libspdlog.a" "lib/libspdlogd.a"
copyMainLib "zlib" "libz.a" "3rdpty/zlib_inst/lib/libz.a"

copyIceLib "ffmpeg" "libavcodec.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavcodec.a"
copyIceLib "ffmpeg" "libavformat.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a"
copyIceLib "ffmpeg" "libavutil.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a"
copyIceLib "ffmpeg" "libswresample.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a"
copyIceLib "ffmpeg" "libswscale.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a"
copyIceLib "fftw" "libfftw3.a" "3rdpty/fftw_inst/lib/libfftw3.a"
copyIceLib "fmt" "${fmtOutputName}" "lib/${fmtOutputName}" "lib/libfmt.a" "lib/libfmtd.a"
copyIceLib "lame" "libmp3lame.a" "3rdpty/lame_inst/lib/libmp3lame.a"
copyIceLib "libsamplerate" "libsamplerate.a" "3rdpty/libsamplerate/libsamplerate.a"
copyIceLib "openal" "libopenal.a" "lib/libopenal.a" "lib/libOpenAL.a"
copyIceLib "rubberband" "librubberband.a" "rb_inst/lib/librubberband.a"
copyIceLib "sdl" "libSDL3.a" "lib/libSDL3.a"
copyIceLib "spdlog" "${spdlogOutputName}" "lib/${spdlogOutputName}" "lib/libspdlog.a" "lib/libspdlogd.a"
copyIceLib "zlib" "libz.a" "3rdpty/zlib_inst/lib/libz.a"
