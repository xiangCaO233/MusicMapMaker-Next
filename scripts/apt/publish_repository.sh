#!/usr/bin/env bash
# @brief 将本项目 DEB 上传到独立 MMM APT 仓库并使用服务器既有密钥签名。
# 私钥始终留在服务器；本脚本不安装依赖、不重启服务、不触碰网站部署目录。
# 此脚本属于一次性人工或 CI 发布入口，不驻留运行，也不创建自动更新任务。
# 所有远端参数均先经过字符白名单检查，再进入 SSH 的远端命令字符串。
# 用户本地 DEB 路径可以包含空格，但永远不作为远端命令片段使用。
# 包内钥环限定为唯一主钥，防止源配置额外信任其它发布身份。
# 发布成功只表示远端生成器完成，不主动刷新服务器外部客户端缓存。
set -Eeuo pipefail

# @brief 打印公共调用契约；必须先显示帮助，帮助调用不依赖发布工具。
usage() {
    # 帮助正文是固定文档，不插入身份目录内容或调用者环境变量。
    cat <<'HELP'
用法：publish_repository.sh [选项] /绝对路径/包.deb [...]
将 musicmapmaker-next DEB 发布到 https://mmm.xiang233.top/apt/。
选项：
  --remote USER@HOST   默认 podman@xiang233.top
  --port PORT          默认 22
  --signing-key FPR    默认 57C834EEC79F747A4644A4ACB3812C03C3460400
  --repo PATH          默认 /home/podman/data/caddy/web/mmm-apt
  --gnupg-home PATH    默认 /home/podman/.local/share/mmm-apt/gnupg
  --help               显示帮助
输入包必须携带与 signing-key 匹配的 /usr/share/keyrings/mmm-archive-keyring.gpg。
依赖：bash、dpkg-deb、gpg、tar、ssh、scp；远端还需仓库生成器依赖。
远端依赖：python3、dpkg-deb、apt-ftparchive（apt-utils）、gpg、flock，无需 dpkg-dev。
HELP
}

# @brief 在任何网络操作之前拒绝不符合约定的输入。
fail() {
    # stderr 与机器可读成功 metadata 分离，调用者可直接依据退出码判断。
    printf 'MMM APT publish: %s\n' "$*" >&2
    exit 1
}

# @brief 验证绝对远端路径；禁止根目录、路径逃逸、控制字符及 shell 元字符。
# @param 1 用于诊断的选项名；2 仅允许路径所需字符的参数值。
validate_path() {
    local label=$1 path=$2
    # 允许绝对目录中的常规名称，不接受引号、反引号或变量替换字符。
    [[ "$path" =~ ^/[A-Za-z0-9_./-]+$ && "$path" != / ]] || fail "invalid $label"
    # 单独的点组件与重复斜线均拒绝，保留一眼可审核的规范路径。
    [[ "$path" != */../* && "$path" != */.. && "$path" != */./* && "$path" != */. && "$path" != *//* ]] || fail "invalid $label"
}

# @brief 默认身份与目录专属于 MMM，网站部署的 --delete 不管理此归档。
remote=podman@xiang233.top
# @brief 端口只允许十进制数值，不提供任意 SSH 选项透传接口。
port=22
# @brief 专用生产身份完整指纹是公开信任标识，不是私钥材料。
signing_key=57C834EEC79F747A4644A4ACB3812C03C3460400
# @brief 与静态网站内容分离的持久仓库根，生成器保留历史包与哈希索引。
repo=/home/podman/data/caddy/web/mmm-apt
# @brief 既有服务器私钥目录，上传文件不能以此目录为暂存目的地。
gnupg_home=/home/podman/.local/share/mmm-apt/gnupg
# @brief 暂存根固定，不从 caller 的仓库路径猜测清理边界。
incoming_root=/home/podman/.local/share/mmm-apt
debs=()
while (($#)); do
    # 选项值仅保存数据；完整解析结束后统一验证，尚未启动外部网络工具。
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --remote|--port|--signing-key|--repo|--gnupg-home)
            (($# >= 2)) || fail "missing value for $1"
            case "$1" in
                --remote) remote=$2 ;;
                --port) port=$2 ;;
                --signing-key) signing_key=$2 ;;
                --repo) repo=$2 ;;
                --gnupg-home) gnupg_home=$2 ;;
            esac
            shift 2 ;;
        # -- 后全部视为输入文件，文件是否为绝对 .deb 路径稍后检查。
        --) shift; debs+=("$@"); break ;;
        -*) fail "unknown option: $1" ;;
        *) debs+=("$1"); shift ;;
    esac
done
# 不接受 SSH 选项、空格或远端 shell 表达式作为 user/host。
[[ "$remote" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*@[A-Za-z0-9][A-Za-z0-9.-]*$ ]] || fail "invalid remote"
[[ "$port" =~ ^[0-9]{1,5}$ ]] || fail "invalid port"
# 明确十进制基数，避免带前导零的合法端口被 Bash 当作八进制解释。
((10#$port >= 1 && 10#$port <= 65535)) || fail "invalid port"
[[ "$signing_key" =~ ^([A-Fa-f0-9]{40}|[A-Fa-f0-9]{64})$ ]] || fail "full signing fingerprint required"
# 使用 POSIX tr 兼容非 Linux 主机的旧 Bash，不依赖 Bash 4 大写扩展。
signing_key=$(printf '%s' "$signing_key" | tr '[:lower:]' '[:upper:]')
validate_path repo "$repo"
validate_path gnupg-home "$gnupg_home"
# 发布路径不能等于私钥目录或暂存根，否则文件布局发生职责混淆。
[[ "$repo" != "$gnupg_home" && "$repo" != "$incoming_root" ]] || fail "repository overlaps signing data"
((${#debs[@]})) || fail "at least one DEB is required"
for tool in dpkg-deb gpg tar ssh scp mktemp; do
    # 缺少工具直接失败；脚本不能为了发布修改客户端软件安装状态。
    command -v "$tool" >/dev/null || fail "required tool unavailable: $tool"
done
# 使用脚本所在的真实本地目录，调用者工作目录不会改变上传的生成器版本。
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
generator=$script_dir/generate_repository.py
# 上传工具只在服务器既有 R2 虚拟环境运行，本机不读取云凭据。
cdn_uploader=$script_dir/upload_cdn.py
[[ -f "$generator" ]] || fail "repository generator is missing"
# @brief 命令数组明确分离 SSH 选项、端口、远端与命令，不使用 eval。
# BatchMode 禁止无人值守发布退回交互密码输入；主机信任沿用用户 SSH 配置。
ssh_command=(ssh -T -p "$port" -o BatchMode=yes -- "$remote")
scp_command=(scp -P "$port" -o BatchMode=yes --)
local_temp=
# @brief 清理状态先置空，尚未创建远端 job 时失败不会执行远端删除。
remote_job=

# @brief 退出时只清理本次临时目录，失败不能删除或回滚既有仓库。
# @warning 清理失败只报告；保留原始发布退出码供 CI 判断。
cleanup() {
    local result=$?
    # 退出 trap 先解除，避免清理过程的 exit 再次触发自己。
    trap - EXIT
    if [[ -n "$remote_job" ]]; then
        # remote_job 在使用前已验证为固定 incoming 根下的 mktemp 子目录。
        "${ssh_command[@]}" "rm -rf -- $remote_job" || printf 'MMM APT publish: temporary remote cleanup failed\n' >&2
    fi
    if [[ -n "$local_temp" ]]; then
        # local_temp 只能来自本次 mktemp 返回值，从不指向仓库或源目录。
        rm -rf -- "$local_temp"
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
local_temp=$(mktemp -d -t mmm-apt-publish.XXXXXX)
# 隔离身份目录只供 show-keys 读取，不加载用户默认 gpg.conf 或钥环。
mkdir -m 700 -- "$local_temp/gnupg"
# @brief 输出 metadata 必须等最终发布成功后才打印。
metadata=()
index=0
for deb in "${debs[@]}"; do
    [[ "$deb" == /* && -f "$deb" && "$deb" == *.deb ]] || fail "DEB must be an existing absolute .deb path: $deb"
    # 产品名必须完全一致，禁止此前用于探针的 debianruntime 包混入生产源。
    package=$(dpkg-deb --field "$deb" Package)
    [[ "$package" == musicmapmaker-next ]] || fail "unexpected Package: $package"
    version=$(dpkg-deb --field "$deb" Version)
    architecture=$(dpkg-deb --field "$deb" Architecture)
    # 元数据最后用于 CI 输出，禁止换行或控制字符破坏导入记录。
    [[ "$version" =~ ^[0-9][A-Za-z0-9.+:~-]*$ ]] || fail "invalid Version"
    [[ "$architecture" =~ ^[a-z0-9][a-z0-9-]*$ ]] || fail "invalid Architecture"
    keyring=$local_temp/key-$index.gpg
    # 只读取 tar 的特定条目，不展开 DEB 的维护脚本或其它文件。
    dpkg-deb --fsys-tarfile "$deb" | tar -xOf - ./usr/share/keyrings/mmm-archive-keyring.gpg > "$keyring"
    key_info=$(gpg --batch --no-options --homedir "$local_temp/gnupg" --with-colons --show-keys "$keyring")
    # 机器可读冒号字段避免本地化文本或用户 ID 内容影响指纹解析。
    primary_count=0
    primary_fingerprint=
    expect_fingerprint=no
    while IFS=: read -r -a fields; do
        case "${fields[0]}" in
            # 只统计 pub 主钥；sub 子钥是同一身份组成部分，不扩大信任根。
            pub) primary_count=$((primary_count + 1)); expect_fingerprint=yes ;;
            fpr)
                if [[ "$expect_fingerprint" == yes ]]; then
                    # 第一个主钥后的 fpr 是完整指纹，后续子钥 fpr 不可覆盖它。
                    primary_fingerprint=${fields[9]}
                    expect_fingerprint=no
                fi ;;
        esac
    done <<< "$key_info"
    # 不只查找预期指纹：额外主钥也必须拒绝，避免扩大客户端信任范围。
    # GnuPG 的 --with-colons fpr 输出使用大写十六进制，与规范化期望一致。
    [[ "$primary_count" == 1 && "$primary_fingerprint" == "$signing_key" ]] || fail "packaged archive key does not match signing fingerprint"
    metadata+=("$package|$version|$architecture")
    index=$((index + 1))
done
# 只有所有输入的包身份和信任根都验证成功，才开始任何网络操作。
candidate_job=$("${ssh_command[@]}" "umask 077; mkdir -p $incoming_root; mktemp -d $incoming_root/incoming.XXXXXX")
# 拒绝登陆横幅、空输出和异常目录；未通过验证的值不能参与清理命令。
[[ "$candidate_job" =~ ^/home/podman/\.local/share/mmm-apt/incoming\.[A-Za-z0-9]{6,}$ ]] || fail "unexpected remote staging path"
# SSH 失败可能同时产生 stdout；只有成功且验证过的路径才交给退出清理。
remote_job=$candidate_job
# 上传的生成器是当前工作副本，发布行为不依赖远端旧脚本是否存在。
"${scp_command[@]}" "$generator" "$remote:$remote_job/generate_repository.py"
# 发布前验证 CDN 实际归档，失败时旧签名索引保持有效。
"${scp_command[@]}" "$cdn_uploader" "$remote:$remote_job/upload_cdn.py"
# 远端文件名来自计数器，用户本地文件名不会进入远端命令字符串。
remote_debs=()
index=0
for deb in "${debs[@]}"; do
    # 每个输入单独上传，失败立即退出；全部成功后才调用签名生成器。
    remote_deb=$remote_job/package-$index.deb
    "${scp_command[@]}" "$deb" "$remote:$remote_deb"
    remote_debs+=("$remote_deb")
    index=$((index + 1))
done
# 白名单保证下列参数只含安全单词；数组先保存本地参数边界。
# 不传递 passphrase、环境或私钥，服务器既有 GnuPG agent 负责钥解锁。
remote_arguments=(python3 "$remote_job/generate_repository.py" --output "$repo" --signing-key "$signing_key" --gnupg-home "$gnupg_home"
    --cdn-base-url https://downloads.xiang233.top
    --cdn-uploader "$remote_job/upload_cdn.py"
    --cdn-python /home/podman/.local/share/mmm-cdn/venv/bin/python
    "${remote_debs[@]}")
# SSH 的远端接口必经 shell，白名单参数保证拼接不会产生新语法。
# 不引入远端 glob；输入文件集合由上方数组完整确定。
"${ssh_command[@]}" "${remote_arguments[*]}"
# 成功状态独立输出，服务器工具自身的诊断不会混淆 CI 的导入记录。
printf 'HTTPSURL=https://mmm.xiang233.top/apt/\nSIGNING_FINGERPRINT=%s\nREPOSITORY=%s\n' "$signing_key" "$repo"
printf 'IMPORTED=%s\n' "${metadata[@]}"
