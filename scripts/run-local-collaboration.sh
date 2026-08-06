#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 ]]; then
    echo "用法: $0 <程序路径> <项目路径> [客户端总数=8] [配置根目录=build/collaboration_profiles]" >&2
    exit 2
fi

app_path=$1
project_path=$2
client_count=${3:-8}
profile_root=${4:-build/collaboration_profiles}

if [[ ! -x "$app_path" ]]; then
    echo "程序不存在或不可执行: $app_path" >&2
    exit 2
fi
if [[ ! -e "$project_path" ]]; then
    echo "项目路径不存在: $project_path" >&2
    exit 2
fi
if [[ ! "$client_count" =~ ^[2-8]$ ]]; then
    echo "客户端总数必须为 2～8" >&2
    exit 2
fi

mkdir -p "$profile_root"
app_path=$(realpath "$app_path")
project_path=$(realpath "$project_path")
profile_root=$(realpath "$profile_root")

pids=()
for ((index = 1; index <= client_count; ++index)); do
    client_profile="$profile_root/client-$index"
    mkdir -p "$client_profile"
    MMM_CONFIG_ROOT="$client_profile" \
        MMM_CREATOR="Local Client $index" \
        "$app_path" "$project_path" &
    pids+=("$!")
done

echo "已启动 $client_count 个隔离客户端。Client 1 可开启房间，其余客户端使用 127.0.0.1、显示端口和房间码连接。"

exit_code=0
for pid in "${pids[@]}"; do
    if ! wait "$pid"; then
        exit_code=1
    fi
done
exit "$exit_code"
