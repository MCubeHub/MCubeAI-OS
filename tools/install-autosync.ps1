# MCubeAI-OS：安装「登录自启、每 5 分钟自动提交并推送」。
#
# 用法
#   powershell -ExecutionPolicy Bypass -File tools\install-autosync.ps1
#   powershell -ExecutionPolicy Bypass -File tools\install-autosync.ps1 -IntervalMinutes 5
#   powershell -ExecutionPolicy Bypass -File tools\install-autosync.ps1 -Remove
#
# 优先注册 Windows 计划任务（不需要管理员权限；被杀了下一轮会自动恢复）。
# 注册失败时退回「启动文件夹 + 隐藏常驻进程」，同样不需要任何权限。
#
# 仓库在 NAS 上，登录时 Z: 可能还没映射，所以两种方式都先跑一个放在本机的
# bootstrap.ps1：等盘符和 .git 出现后再调用仓库里的 autosync.ps1。
#
# 注意：本文件必须保存为 UTF-8 带 BOM。

param(
    [int]$IntervalMinutes = 5,
    [switch]$Remove
)

$ErrorActionPreference = "Continue"

$taskName      = "MCubeAIOS-AutoSync"
$startupDir    = [Environment]::GetFolderPath("Startup")
$vbsPath       = Join-Path $startupDir "MCubeAIOS-AutoSync.vbs"
$stateDir      = Join-Path $env:LOCALAPPDATA "MCubeAIOS-AutoSync"
$bootstrapPath = Join-Path $stateDir "bootstrap.ps1"
$syncScript    = Join-Path $PSScriptRoot "autosync.ps1"
$repoRoot      = Split-Path -Parent $PSScriptRoot

function Stop-SyncProcess {
    $procs = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
               Where-Object {
                   $_.ProcessId -ne $PID -and
                   (
                       $_.CommandLine -like "*MCubeAIOS\tools\autosync.ps1*" -or
                       $_.CommandLine -like "*MCubeAIOS-AutoSync\bootstrap.ps1*"
                   ) -and
                   $_.CommandLine -notlike "*install-autosync*"
               })

    foreach ($p in $procs) {
        try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop } catch { }
    }

    if ($procs.Count -gt 0) { Write-Host "已结束 $($procs.Count) 个正在跑的同步进程。" }
}

function Remove-Install {
    Stop-SyncProcess

    if (Test-Path $vbsPath) {
        Remove-Item $vbsPath -Force
        Write-Host "已删除启动项：$vbsPath"
    }

    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        try {
            Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction Stop
            Write-Host "已删除计划任务：$taskName"
        } catch {
            Write-Host "删除计划任务失败（需要管理员权限）：$($_.Exception.Message)" -ForegroundColor Yellow
        }
    }

    if (Test-Path $stateDir) {
        Remove-Item $stateDir -Recurse -Force
        Write-Host "已删除本机引导脚本：$stateDir"
    }

    Write-Host "自动同步已卸载。" -ForegroundColor Green
}

# 放在本机的引导脚本：等 NAS 盘符就绪，再调用仓库里的 autosync.ps1。
# 内容保持纯 ASCII，避免 PowerShell 5.1 的编码坑。
function Write-Bootstrap {
    New-Item -ItemType Directory -Path $stateDir -Force | Out-Null

    $bootstrap = @'
param(
    [switch]$Once,
    [int]$IntervalSeconds = 300
)

$repo = "Z:\MCubeAIOS"
$fallback = "\\LGY-NAS\Worlds\MCubeAIOS"

for ($i = 0; $i -lt 20; $i++) {
    if (Test-Path (Join-Path $repo ".git")) { break }
    if (Test-Path (Join-Path $fallback ".git")) { $repo = $fallback; break }
    Start-Sleep -Seconds 15
}

if (-not (Test-Path (Join-Path $repo ".git"))) { exit 1 }

$sync = Join-Path $repo "tools\autosync.ps1"
if (-not (Test-Path $sync)) { exit 1 }

if ($Once) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File $sync -Once
} else {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File $sync -IntervalSeconds $IntervalSeconds
}
'@

    Set-Content -LiteralPath $bootstrapPath -Value $bootstrap -Encoding ASCII
}

function Install-ScheduledTask {
    $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $startBoundary = (Get-Date).AddMinutes(-1).ToString("yyyy-MM-ddTHH:mm:ss")

    $xml = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>MCubeAI-OS: 登录自启，每 $IntervalMinutes 分钟自动提交并推送</Description>
  </RegistrationInfo>
  <Triggers>
    <TimeTrigger>
      <StartBoundary>$startBoundary</StartBoundary>
      <Enabled>true</Enabled>
      <Repetition>
        <Interval>PT${IntervalMinutes}M</Interval>
        <StopAtDurationEnd>false</StopAtDurationEnd>
      </Repetition>
    </TimeTrigger>
    <LogonTrigger>
      <Enabled>true</Enabled>
      <Delay>PT30S</Delay>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>$sid</UserId>
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>LeastPrivilege</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT10M</ExecutionTimeLimit>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>powershell.exe</Command>
      <Arguments>-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "$bootstrapPath" -Once</Arguments>
      <WorkingDirectory>$stateDir</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
"@

    Register-ScheduledTask -TaskName $taskName -Xml $xml -Force -ErrorAction Stop | Out-Null
}

function Install-StartupLauncher {
    $inner = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' +
             $bootstrapPath + '" -IntervalSeconds ' + ($IntervalMinutes * 60)
    $inner = $inner -replace '"', '""'

    $vbs = @(
        "' MCubeAI-OS auto-sync launcher: hidden at logon, syncs every $IntervalMinutes minutes.",
        "' Delete this file or run install-autosync.ps1 -Remove to stop.",
        'Set sh = CreateObject("WScript.Shell")',
        "sh.Run ""$inner"", 0, False"
    ) -join "`r`n"

    Set-Content -LiteralPath $vbsPath -Value $vbs -Encoding ASCII
    Start-Process -FilePath "wscript.exe" -ArgumentList ('"' + $vbsPath + '"')
}

if ($Remove) {
    Remove-Install
    return
}

if (-not (Test-Path $syncScript)) {
    Write-Host "找不到同步脚本：$syncScript" -ForegroundColor Red
    exit 1
}

if ($IntervalMinutes -lt 1) {
    Write-Host "-IntervalMinutes 至少要 1。" -ForegroundColor Red
    exit 1
}

Stop-SyncProcess
Write-Bootstrap

$usedTask = $false
try {
    Install-ScheduledTask
    $usedTask = $true
} catch {
    Write-Host "注册计划任务没成功（通常是缺管理员权限）：$($_.Exception.Message)" -ForegroundColor Yellow
}

if ($usedTask) {
    if (Test-Path $vbsPath) { Remove-Item $vbsPath -Force }

    Write-Host "已注册计划任务「$taskName」：登录后每 $IntervalMinutes 分钟同步一次。" -ForegroundColor Green
    try {
        Start-ScheduledTask -TaskName $taskName -ErrorAction Stop
        Write-Host "已立刻触发一轮。"
    } catch {
        Write-Host "触发失败，等下一次调度即可。" -ForegroundColor Yellow
    }
} else {
    Install-StartupLauncher
    Write-Host "已安装启动项「$vbsPath」：登录后静默运行，每 $IntervalMinutes 分钟同步一次。" -ForegroundColor Green
}

Write-Host ""
Write-Host "查看运行记录：Get-Content '$repoRoot\.auto-sync.log' -Tail 20" -ForegroundColor Cyan
Write-Host "查看计划任务：schtasks /Query /TN $taskName /V /FO LIST" -ForegroundColor Cyan
Write-Host "卸载：powershell -ExecutionPolicy Bypass -File tools\install-autosync.ps1 -Remove" -ForegroundColor Cyan
