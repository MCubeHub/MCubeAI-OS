@echo off
setlocal EnableExtensions
rem Rebuild MCubeAIOS and boot it in the "MCubeAIOS" VirtualBox VM.
rem Double-click this file after making changes.

set "VM=MCubeAIOS"
set "VBOX=C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
set "WORK=C:\Users\X\MCubeAIOS-test"
set "IMG=\\LGY-NAS\Worlds\MCubeAIOS\build3\esp.img"
set "VDI=%WORK%\esp-latest.vdi"

if not exist "%VBOX%" (
  echo [x] VBoxManage not found: %VBOX%
  pause
  exit /b 1
)
if not exist "%WORK%" mkdir "%WORK%"

echo [1/5] Building build3 image ...
rem WSL cannot translate a working directory on a network drive, so every
rem wsl.exe call below runs from a local directory (paths stay absolute).
cd /d "%SystemDrive%\"
rem Always repack the ESP: if the kernel did not change, make would skip the
rem image and the VM would keep booting whatever the disk already holds.
if exist "%IMG%" del /q "%IMG%"
wsl.exe -d Debian -- bash -lc "cd /mnt/z/MCubeAIOS && make BUILD=build3 -j4 image"
if errorlevel 1 (
  echo [x] Build failed.
  pause
  exit /b 1
)

rem Guard against a silently stale image: if esp.img was not rewritten by this
rem run, the VM would boot an old kernel and look frozen on the splash screen.
powershell -NoProfile -Command "$f=Get-Item -LiteralPath $env:IMG; if (-not $f -or $f.LastWriteTime -lt (Get-Date).AddMinutes(-5)) { Write-Host ('[x] esp.img missing or stale: ' + $f.LastWriteTime); exit 1 }"
if errorlevel 1 (
  echo [x] The ESP image was not refreshed; aborting so an old kernel is not booted.
  pause
  exit /b 1
)

echo [2/5] Stopping VM if running ...
"%VBOX%" showvminfo "%VM%" --machinereadable | findstr /b /c:"VMState=" | findstr /c:"running" >nul
if not errorlevel 1 (
  "%VBOX%" controlvm "%VM%" poweroff
  rem "timeout" needs a console for its input redirection; this script may be
  rem launched without one, so sleep through PowerShell instead.
  powershell -NoProfile -Command "Start-Sleep -Seconds 3"
)

echo [3/5] Detaching stale media and fixing the boot order ...
rem An ISO left in the DVD drive wins the boot contest and starts an old
rem kernel, which is exactly what makes the VM sit on the splash screen.
"%VBOX%" storageattach "%VM%" --storagectl IDE --port 1 --device 0 --type dvddrive --medium none >nul 2>&1
"%VBOX%" storageattach "%VM%" --storagectl IDE --port 0 --device 0 --type hdd --medium none >nul 2>&1
"%VBOX%" modifyvm "%VM%" --boot1 disk --boot2 none --boot3 none --boot4 none >nul

echo [4/5] Refreshing boot disk ...
"%VBOX%" closemedium disk "%VDI%" --delete >nul 2>&1
"%VBOX%" convertfromraw "%IMG%" "%VDI%" --format VDI
if errorlevel 1 (
  echo [x] Disk conversion failed.
  pause
  exit /b 1
)
"%VBOX%" storageattach "%VM%" --storagectl IDE --port 0 --device 0 --type hdd --medium "%VDI%"
if errorlevel 1 (
  echo [x] Could not attach the new disk.
  pause
  exit /b 1
)

echo [5/5] Starting VM (GUI) ...
"%VBOX%" startvm "%VM%" --type gui
exit /b 0
