#!/usr/bin/env bash
# @brief Debian Runner 的 ARM64 发布型 APK 交付入口，外层工作流已持有 Android 工作树锁。
set -euo pipefail
# 根目录固定在 Runner 已挂载的 cross 中，不增加容器挂载或污染桌面 checkout。
android_root=/home/xiang/cross/android
android_repo="$(git -C "$(dirname "$(readlink -f "$0")")" rev-parse --show-toplevel)"
cd "$android_repo"
# 必须消费触发事件的 SHA；拒绝把未提交修改或子模块偏移混进已标记的制品。
# 只接受完整十六进制提交身份，不接受分支名、短 SHA 或参数形式的输入。
[[ "${MMM_SOURCE_REF:-}" =~ ^[0-9a-f]{40}$ ]]
[[ "$(git rev-parse HEAD)" == "$MMM_SOURCE_REF" ]]
[[ -z "$(git status --porcelain --untracked-files=no)" ]]
# SDK 与 JDK 仅作用于本进程树，既有 Linux/Windows 编译环境保持独立。
export ANDROID_HOME="$android_root/sdk"
export MMM_ANDROID_JAVA_HOME="$android_root/jdk-21"
export JAVA_HOME="$MMM_ANDROID_JAVA_HOME"
export PATH="$JAVA_HOME/bin:$PATH"
export MMM_ANDROID_WORK_DIR="$android_root/work"
# 不继承开发者用于模拟器的 ABI 列表，缓存里已有 x86 库也不能进入交付包。
export MMM_ANDROID_ABIS=arm64-v8a
# 首次源码构建需要控制峰值内存；本机有其它 Runner 作业，不使用满 CPU 并发。
export MMM_ANDROID_JOBS="${MMM_ANDROID_JOBS:-6}"
export MMM_ANDROID_VARIANT=release
# 该证书仅用于既有 QA 身份，私有文件不进入源码、缓存归档或 GitHub 制品。
export MMM_ANDROID_QA_KEYSTORE="$android_root/secrets/qa/mmm-qa.p12"
export MMM_ANDROID_QA_PASSWORD_FILE="$android_root/secrets/qa/store-password"
# 在既有 QA 版本码之上累加同一入口的运行编号，允许覆盖安装并保留数据。
# 重跑同一运行保持版本码不变；旁置元数据记录完整源码提交，便于构建对照。
[[ "${GITHUB_RUN_NUMBER:-}" =~ ^[0-9]+$ ]]
export MMM_ANDROID_QA_VERSION_CODE="$((2026111404 + GITHUB_RUN_NUMBER))"
# 上限在 Gradle 注入阶段再次验证，越界不能静默取模或生成无法更新的版本。
# QA 包名保持独立，版本码的递增不会改变正式发行包的升级策略。
# 从主项目唯一版本源读取，不能把旧 Android 外壳的默认版本写进 QA 包。
android_version="$(sed -n 's/^set(APPVER "\([^"]*\)").*/\1/p' "$android_repo/CMakeLists.txt")"
# 唯一版本源缺失时立即停止，不生成缺少主项目版本的安装包。
[[ -n "$android_version" ]]
# 对外版本名与桌面 APPVER 一致，精确源码身份单独写入 build-info.json。
# QA 前缀仅保留在历史签名接口中，不再写进交付文件名或版本名。
export MMM_ANDROID_QA_VERSION_NAME="$android_version"
# 交付契约测试只生成微型夹具，输出仍归入 Android 的集中工作目录。
# 它们不能代替下面对实际 APK 的验签、资源完整性和架构检查。
MMM_TEST_OUTPUT_DIR="$MMM_ANDROID_WORK_DIR/evidence/release-unit" \
    python3 "$android_repo/scripts/ci/test_android_release.py"
# 先完整生成并验签，再整理独立交付目录；失败不会上传历史 APK 冒充本次结果。
# 原生符号和依赖保留在 work/build，短期制品只包含安装包、摘要与来源信息。
bash "$android_repo/scripts/android/build.sh"
android_delivery="$MMM_ANDROID_WORK_DIR/apk/ci"
mkdir -p "$android_delivery"
# 只清理本入口的交付副本，不删除签名、源码、原生符号或整个构建缓存。
rm -f "$android_delivery"/*.apk "$android_delivery"/*.sha256 "$android_delivery/build-info.json"
# 与桌面交付命名一致：产品名、版本、平台、架构；ABI 仍由 APK 实测验证。
# 源码 SHA 不混入版本名，下载者可在旁置元数据中核对本次构建来源。
android_name="MusicMapMaker-Next-$android_version-android-arm64.apk"
cp "$MMM_ANDROID_WORK_DIR/apk/MusicMapMaker-Next-android-release.apk" "$android_delivery/$android_name"
# 二进制检查从实际 APK 读取，不能仅相信 CMake 缓存或文件命名中的 ABI 标签。
# 流式计算摘要；ZIP 逐成员校验 CRC，资源与原生库均不能是损坏的截断输出。
python3 - "$android_delivery/$android_name" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

apk = Path(sys.argv[1])
sdk = Path(os.environ['ANDROID_HOME']) / 'build-tools/36.1.0'
# aapt 检查最终清单，发布型包不能携带调试标志或错误的应用身份。
# 身份与版本都读最终合并清单，防止 Gradle 默认配置覆盖注入值。
badging = subprocess.check_output([sdk / 'aapt2', 'dump', 'badging', apk], text=True)
assert "name='top.xiang233.musicmapmaker.next.qa'" in badging
assert f"versionCode='{os.environ['MMM_ANDROID_QA_VERSION_CODE']}'" in badging
assert f"versionName='{os.environ['MMM_ANDROID_QA_VERSION_NAME']}'" in badging
assert 'application-debuggable' not in badging
# JNI 所有目录必须属于唯一目标 ABI；命名检查同时排除宿主 Linux 库混入。
with zipfile.ZipFile(apk) as archive:
    libraries = [name for name in archive.namelist() if name.startswith('lib/')]
    assert {name.split('/')[1] for name in libraries} == {'arm64-v8a'}
    # ELF e_machine 在固定头部偏移，183 表示 AArch64；无需把整份符号库读入内存。
    with archive.open('lib/arm64-v8a/libmain.so') as main:
        header = main.read(20)
    assert header[:4] == b'\x7fELF' and int.from_bytes(header[18:20], 'little') == 183
    # 完整 CRC 检查包含大型 assets.zip，不以 ZIP 中央目录可读冒充资源完整。
    assert archive.testzip() is None
# 签名验证输出仅包含公开证书信息，命令行没有私钥口令。
certificate = subprocess.check_output([sdk / 'apksigner', 'verify', '--print-certs', apk], text=True)
with apk.open('rb') as stream:
    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
# 元数据不包含环境变量全集和签名文件路径，避免泄露 Runner 的私有配置。
info = dict(source=os.environ['MMM_SOURCE_REF'], abi='arm64-v8a', variant='release',
            versionName=os.environ['MMM_ANDROID_QA_VERSION_NAME'],
            versionCode=int(os.environ['MMM_ANDROID_QA_VERSION_CODE']),
            bytes=apk.stat().st_size, sha256=digest, certificate=certificate,
            run=os.environ.get('GITHUB_SERVER_URL', '') + '/' + os.environ.get('GITHUB_REPOSITORY', '')
                + '/actions/runs/' + os.environ.get('GITHUB_RUN_ID', ''))
# 摘要文件使用相对 APK 名称，QA 下载整份制品后可以直接执行 sha256sum -c。
(apk.parent / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')
(apk.parent / (apk.name + '.sha256')).write_text(f'{digest}  {apk.name}\n')
# 日志只回显交付清单，原生调试符号继续留在 Runner 的专用目录。
print(json.dumps(info, indent=2))
PY
# 完整 CI 使用运行专属目录，不能在聚合时读取可能已被下一任务覆盖的 apk/ci。
# 单独安卓构建只上传 Artifact，不制造无人消费的发布缓存。
if [[ "${MMM_ANDROID_STAGE_RELEASE:-0}" == 1 ]]; then
    [[ "${GITHUB_RUN_ID:-}" =~ ^[0-9]+$ ]]
    [[ -d /home/xiang/MusicMapMaker-Next/.git ]]
    python3 "$android_repo/scripts/ci/android-release.py" stage \
        --directory "$android_delivery" --source "$MMM_SOURCE_REF" --version "$android_version" \
        --destination "/home/xiang/MusicMapMaker-Next/.git/mmm-ci-transfer/$GITHUB_RUN_ID/android-arm64"
fi
# 只有验签和二进制检查成功后才发布产物名称，供工作流的上传步骤使用。
# 独立运行脚本时不要求 GitHub 环境，也不额外创建源码侧输出文件。
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    echo "artifact_name=${android_name%.apk}" >> "$GITHUB_OUTPUT"
fi
