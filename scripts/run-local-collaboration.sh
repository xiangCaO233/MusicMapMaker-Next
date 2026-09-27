#!/usr/bin/env bash
# 启动 2～8 个配置隔离的本地客户端，用于协作房间联调。
# Client 1 打开指定项目并充当建房端，其余客户端仅进入欢迎界面。
# 每个客户端使用独立 MMM_CONFIG_ROOT 和创作者名，避免身份相互覆盖。
# 脚本等待全部子进程退出，并在任一客户端失败时返回非零。
set -euo pipefail

# 参数数量先验证，防止位置参数在 nounset 模式下提前终止。
if [[ $# -lt 2 || $# -gt 4 ]]; then
    echo "用法: $0 <程序路径> <项目路径> [客户端总数=8] [配置根目录=build/collaboration_profiles]" >&2
    exit 2
fi

app_path=$1
# 项目路径只传给第一个客户端，其他客户端通过联机加入。
project_path=$2
# 默认八客户端覆盖房间容量上限，也允许较小规模快速联调。
client_count=${3:-8}
# 隔离配置默认保存在构建树，便于测试后整体移除。
profile_root=${4:-build/collaboration_profiles}

# 主程序必须可执行，普通存在性不足以保证后续启动成功。
if [[ ! -x "$app_path" ]]; then
    echo "程序不存在或不可执行: $app_path" >&2
    exit 2
fi
if [[ ! -e "$project_path" ]]; then
    # 项目既可以是目录，也可以是应用支持的包文件。
    echo "项目路径不存在: $project_path" >&2
    exit 2
fi
if [[ ! "$client_count" =~ ^[2-8]$ ]]; then
    # 正则同时限制整数格式和协作测试支持的容量范围。
    echo "客户端总数必须为 2～8" >&2
    exit 2
fi

# 先创建配置根，再将所有用户输入规范化为绝对路径。
mkdir -p "$profile_root"
# 绝对程序路径保证后台客户端不受后续工作目录变化影响。
app_path=$(realpath "$app_path")
# 项目路径同样固定，供 Client 1 可靠打开。
project_path=$(realpath "$project_path")
# 配置根固定后再派生每个客户端目录。
profile_root=$(realpath "$profile_root")

# 保存所有后台 PID，确保脚本不会提前退出并遗留无人管理进程。
pids=()
for ((index = 1; index <= client_count; ++index)); do
    # 每个编号映射到稳定目录，重复运行可复用其联调状态。
    client_profile="$profile_root/client-$index"
    mkdir -p "$client_profile"
    if ((index == 1)); then
        # 首客户端同时接收项目参数，负责进入可创建房间的编辑状态。
        MMM_CONFIG_ROOT="$client_profile" \
            MMM_CREATOR="Local Client $index" \
            "$app_path" "$project_path" &
    else
        # 其余客户端不打开项目，避免各自产生独立项目会话。
        MMM_CONFIG_ROOT="$client_profile" \
            MMM_CREATOR="Local Client $index" \
            "$app_path" &
    fi
    # $! 必须紧跟后台启动捕获，避免记录其他异步命令 PID。
    pids+=("$!")
done

# 提示明确描述后续人工联调步骤，不自动传播房间码。
echo "已启动 $client_count 个隔离客户端。Client 1 已打开项目并可开启房间，其余客户端无需打开项目，使用 127.0.0.1、显示端口和房间码连接。"

# 汇总所有客户端状态，同时仍等待后续 PID 完成清理。
exit_code=0
for pid in "${pids[@]}"; do
    # wait 失败只更新汇总状态，不因 errexit 跳过其他客户端。
    if ! wait "$pid"; then
        exit_code=1
    fi
done
# 全部客户端关闭后向调用者传播是否存在异常退出。
exit "$exit_code"
