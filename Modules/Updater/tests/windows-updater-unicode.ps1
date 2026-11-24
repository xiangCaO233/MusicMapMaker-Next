<#
.SYNOPSIS
通过原生 Windows 更新器验证包含 Unicode 的下载、安装和重启路径。
.DESCRIPTION
夹具包含两个互不相同的 Unicode 目录，且两个文件名都含空格及一个代理对。
命令行走进程创建接口，因此同一次执行可验证 argv 解码、备份后缀和实际启动。
目标原来存在，用于覆盖旧程序备份与回滚路径所依赖的同一个文件名规则。
待安装载荷由系统 PowerShell 临时编译，不依赖已安装的 MusicMapMaker。
成功标记由更新器生成，重启证据由新程序生成，两者必须分别与原始输入一致。
.PARAMETER Updater
当前构建产物中独立更新器的绝对路径。
.PARAMETER OutputDirectory
仅供本用例写入的测试输出根目录，允许多个用例共享根目录并行运行。
#>

param(
    [Parameter(Mandatory = $true)][string]$Updater,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

# 使用独立目录，避免上次残留的标记让重启验证误判。参数用原生命令行传递。
# 脚本只含 ASCII 语法，由运行时生成旧 Windows ANSI 代码页无法表示的字符。
$work = Join-Path $OutputDirectory ([Guid]::NewGuid().ToString('N'))
$process = $null
try {
    # 补充平面字符由两段 UTF-16 代码单元组成，能查出把命令行降级为 ANSI 的路径。
    # 通过码点构造字符，测试启动方式不会受 PowerShell 脚本源编码影响。
    $rocket = [char]::ConvertFromUtf32(0x1F680)
    $downloadDir = Join-Path $work ('download ' + $rocket)
    $installDir = Join-Path $work ('install ' + $rocket)
    $download = Join-Path $downloadDir ('payload ' + $rocket + '.exe')
    $target = Join-Path $installDir ('editor ' + $rocket + '.exe')
    $witness = Join-Path $installDir 'launch-proof.txt'
    $marker = Join-Path $installDir '.mm_update_success'

    [void][IO.Directory]::CreateDirectory($downloadDir)
    [void][IO.Directory]::CreateDirectory($installDir)
    # 旧文件内容和可执行载荷不同。替换后哈希一致才能证明覆盖实际发生。
    [IO.File]::WriteAllText($target, 'old installation fixture')

    # 夹具只记录实际的本机映像路径及当前目录，验证更新器确实启动已替换的文件。
    # 即使 ShellExecute 成功返回，仍要等这份证据完成后才报告回归通过。
    $fixtureCode = @'
using System;
using System.Diagnostics;
using System.IO;

public static class UpdaterUnicodeProbe {
    public static void Main() {
        string image = Process.GetCurrentProcess().MainModule.FileName;
        string directory = Directory.GetCurrentDirectory();
        string proof = Path.Combine(Path.GetDirectoryName(image), "launch-proof.txt");
        File.WriteAllLines(proof + ".tmp", new[] { image, directory });
        File.Move(proof + ".tmp", proof);
    }
}
'@
    # Windows PowerShell 自带 C# 编译器，可无需额外测试可执行目标生成载荷。
    # 本次夹具仅发出本地文件证据，并且在执行结束后离开。
    Add-Type -TypeDefinition $fixtureCode -OutputType ConsoleApplication -OutputAssembly $download
    $payloadHash = (Get-FileHash -LiteralPath $download).Hash

    # 先启动并等待一个短命父进程，传给更新器真实已退出的 PID。
    $parent = Start-Process -FilePath $env:ComSpec -ArgumentList '/d /c exit 0' -NoNewWindow -PassThru -Wait
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Updater
    # 两个文件路径含空格和非 BMP 字符，使用引号保持 argv 的参数边界。
    $start.Arguments = '"{0}" "{1}" {2}' -f $download, $target, $parent.Id
    # 刻意从另一个目录启动更新器，重启进程若错误继承该目录便不能通过检查。
    $start.WorkingDirectory = $work
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardError = $true
    # 使用进程 API 调用实际二进制，跳过交互式终端或 cmd 的代码页转换。
    $process = [System.Diagnostics.Process]::Start($start)
    # 夹具没有长驻父进程。测试超时用于防止生产 wait 退化时让测试永久阻塞。
    if (!$process.WaitForExit(15000)) {
        throw 'Updater did not complete within 15 seconds'
    }
    if ($process.ExitCode -ne 0) {
        throw ('Updater exited {0}: {1}' -f $process.ExitCode, $process.StandardError.ReadToEnd())
    }

    # 同时证明下载文件已消费、目标确实替换成功，以及 Unicode 备份名已清理。
    if ([IO.File]::Exists($download) -or [IO.File]::Exists($target + '.old') -or
        (Get-FileHash -LiteralPath $target).Hash -ne $payloadHash) {
        throw 'Unicode install and download files do not have the expected state'
    }
    # 标记按 UTF-8 解析，不能借助启动时的默认 ANSI 解码隐藏错误。
    if ([IO.File]::ReadAllText($marker, [Text.Encoding]::UTF8) -cne $target) {
        throw 'Success marker did not retain the installed UTF-8 path'
    }

    # ShellExecute 异步启动新程序，证据通过原子重命名发布，无需读取半写内容。
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while (![IO.File]::Exists($witness) -and $clock.ElapsedMilliseconds -lt 10000) {
        Start-Sleep -Milliseconds 50
    }
    # 载荷取得的是运行时映像名及工作目录，与启动方传入的目标字符串独立。
    $actual = [IO.File]::ReadAllLines($witness)
    if ($actual.Length -ne 2 -or $actual[0] -cne $target -or $actual[1] -cne $installDir) {
        throw 'Relaunched executable or its working directory lost Unicode path data'
    }
    # 随结果记录系统代码页，便于区分传统 ACP 和系统配置为 UTF-8 的机器。
    Write-Output ('Unicode updater round trip passed. ACP={0}' -f [Text.Encoding]::Default.CodePage)
} finally {
    # 只结束此次创建且超时未退出的更新器，不影响系统中的其他程序。
    if ($null -ne $process) {
        if (!$process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
    # 清理只限本用例创建的目录。重启夹具最后可能仍在释放映像句柄。
    for ($attempt = 0; $attempt -lt 5 -and [IO.Directory]::Exists($work); $attempt++) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
        if ([IO.Directory]::Exists($work)) { Start-Sleep -Milliseconds 50 }
    }
}
