# MCubeAI-OS 自动同步器：盯着工作区，一有改动就提交并推到 GitHub。
#
# 用法
#   powershell -ExecutionPolicy Bypass -File tools\autosync.ps1
#   powershell -ExecutionPolicy Bypass -File tools\autosync.ps1 -IntervalSeconds 60
#   powershell -ExecutionPolicy Bypass -File tools\autosync.ps1 -Once          # 只同步一轮
#   powershell -ExecutionPolicy Bypass -File tools\autosync.ps1 -NoPush        # 只提交不推送
#
# 日常不用手动跑，用 tools\install-autosync.ps1 装成登录自启即可（默认 5 分钟一轮）。
#
# 注意：本文件必须保存为 UTF-8 带 BOM。Windows PowerShell 5.1 会把无 BOM 的 .ps1
# 按本地代码页（GBK）读取，中文注释和提交信息会变成乱码。

param(
    # 每轮间隔。默认 300 秒 = 5 分钟。
    [int]$IntervalSeconds = 300,

    # 推到哪个分支。
    [string]$Branch = "master",

    # 固定提交信息。不填就生成「自动同步：时间（N 处改动）」。
    [string]$Message = "",

    # 只同步一次就退出（计划任务用这个）。
    [switch]$Once,

    # 只提交到本地，不推送。
    [switch]$NoPush
)

# git 的正常提示也会写到 stderr；不要用 Stop，统一看 $LASTEXITCODE。
$ErrorActionPreference = "Continue"

# 仓库在 Z:\MCubeAIOS（NAS）。登录时盘符可能还没挂好，先等最多两分钟。
$repoDir = Split-Path -Parent $PSScriptRoot
for ($i = 0; $i -lt 8; $i++) {
    if (Test-Path (Join-Path $repoDir ".git")) { break }
    Start-Sleep -Seconds 15
}

$repo = git rev-parse --show-toplevel 2>$null
if (-not $repo) {
    Set-Location $repoDir
    $repo = git rev-parse --show-toplevel 2>$null
}
if (-not $repo) {
    Write-Host "当前目录不是 git 仓库：$repoDir" -ForegroundColor Red
    exit 1
}
Set-Location $repo

# 无人值守时看不到窗口，顺手记一份日志（*.log 已被 .gitignore 忽略）
$logFile = Join-Path $repo ".auto-sync.log"
function Write-SyncLog([string]$text) {
    try {
        Add-Content -Path $logFile -Value ("[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $text) -Encoding UTF8
    } catch {
        # 日志写不进去不影响同步
    }
}

# 计划任务和启动项可能同时拉起两份，用独占文件锁保证只有一份在跑
$lockPath = Join-Path $env:TEMP "mcubeaios-autosync.lock"
$lock = $null
try {
    $lock = [System.IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None')
} catch {
    Write-SyncLog "另一轮同步还在跑，本次跳过"
    exit 0
}

# 无人值守时绝不能让 git 停下来等账号密码
$env:GIT_TERMINAL_PROMPT = "0"

# 本地领先远端多少个提交。用远端跟踪分支判断，上次没推成功的提交这轮会重试。
function Get-AheadCount {
    $n = git rev-list --count "origin/$Branch..HEAD" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $n) { return 1 }
    return [int]$n
}

function Push-Pending {
    if ((Get-AheadCount) -le 0) { return $true }

    git push --quiet origin "HEAD:$Branch" 2>$null
    if ($LASTEXITCODE -eq 0) { return $true }

    # 远端可能被别的机器/网页改过：rebase 后重试一次
    git pull --rebase --autostash --quiet origin $Branch 2>$null
    if ($LASTEXITCODE -ne 0) {
        git rebase --abort 2>$null
        return $false
    }

    git push --quiet origin "HEAD:$Branch" 2>$null
    return ($LASTEXITCODE -eq 0)
}

function Sync-Once {
    $changed = @(git status --porcelain)
    $stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"

    if ($changed.Count -gt 0) {
        $msg = if ([string]::IsNullOrWhiteSpace($Message)) {
            "自动同步：$stamp（$($changed.Count) 处改动）"
        } else {
            $Message
        }

        git add -A
        git commit --quiet -m $msg 2>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[$stamp] 提交失败，跳过本轮。" -ForegroundColor Yellow
            Write-SyncLog "提交失败（$($changed.Count) 处改动）"
        } else {
            Write-Host "[$stamp] 已提交 $($changed.Count) 处改动。" -ForegroundColor Cyan
            Write-SyncLog "已提交 $($changed.Count) 处改动"
        }
    }

    if ($NoPush) { return }

    # 即使本轮没有新改动，也检查一下上次是否留下没推上去的提交
    if (Push-Pending) {
        if ($changed.Count -gt 0) {
            Write-Host "[$stamp] 已推送到 origin/$Branch。" -ForegroundColor Green
            Write-SyncLog "已推送"
        }
    } else {
        Write-Host "[$stamp] 推送失败（凭据 / 网络 / 冲突），提交都在本地，下一轮重试。" -ForegroundColor Yellow
        Write-SyncLog "推送失败（凭据 / 网络 / 冲突），下一轮重试"
    }
}

if ($Once) {
    Write-Host "自动同步：只跑一轮。"
} else {
    Write-Host "自动同步：每 $IntervalSeconds 秒检查一次，推到 origin/$Branch。Ctrl+C 停止。"
}

try {
    while ($true) {
        Sync-Once
        if ($Once) { break }
        Start-Sleep -Seconds $IntervalSeconds
    }
}
finally {
    if ($lock) { $lock.Dispose() }
}
