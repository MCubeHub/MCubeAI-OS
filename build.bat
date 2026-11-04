@echo off
rem ==========================================================================
rem  MCubeAIOS release builder (Windows entry point)
rem
rem  Usage:
rem      build.bat 1.0.0
rem      build.bat /v 1.0.0
rem      build.bat                 (auto version: yy.MM.dd.HH.mm)
rem
rem  Environment:
rem      MCUBE_WSL_DISTRO   WSL distribution (default: Debian)
rem      MCUBE_WSL_DIR      Repository path inside WSL (autodetected)
rem      MCUBE_BUILD_DIR    Build directory inside the repo (default: build)
rem      MCUBE_JOBS         Parallel make jobs (default: CPU count)
rem
rem  Output:
rem      dist\MCubeAIOS-<version>\          ISO, IMG, VHD, QCOW2, ELF, EFI...
rem      history-version\MCubeAIOS-<version>.zip
rem ==========================================================================
setlocal EnableExtensions DisableDelayedExpansion

set "REPO_WIN=%~dp0"
if "%REPO_WIN:~-1%"=="\" set "REPO_WIN=%REPO_WIN:~0,-1%"

if not defined MCUBE_WSL_DISTRO set "MCUBE_WSL_DISTRO=Debian"
if not defined MCUBE_BUILD_DIR set "MCUBE_BUILD_DIR=build"
if not defined MCUBE_JOBS set "MCUBE_JOBS=%NUMBER_OF_PROCESSORS%"
if not defined MCUBE_JOBS set "MCUBE_JOBS=4"

rem ---- version ------------------------------------------------------------
set "VERSION="
if /I "%~1"=="/v" (
    set "VERSION=%~2"
) else if /I "%~1"=="/version" (
    set "VERSION=%~2"
) else if not "%~1"=="" (
    set "VERSION=%~1"
)

rem No version given on the command line -> derive one from the current time,
rem e.g. 26.10.25.10.23 (yy.MM.dd.HH.mm). Never fall back to a fixed version.
if not defined VERSION (
    for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "Get-Date -Format 'yy.MM.dd.HH.mm'"`) do set "VERSION=%%I"
)
if not defined VERSION (
    echo [ERROR] Could not determine a version number.
    exit /b 2
)

set "MCUBE_VERSION=%VERSION%"
powershell -NoProfile -Command "if ($env:MCUBE_VERSION -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]{0,31}$') { exit 1 }"
if errorlevel 1 (
    echo [ERROR] Invalid version "%VERSION%".
    echo         Use 1-32 characters: letters, digits, dot, dash or underscore.
    exit /b 2
)

set "MCUBE_BUILD_NAME=%MCUBE_BUILD_DIR%"
powershell -NoProfile -Command "if ($env:MCUBE_BUILD_NAME -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') { exit 1 }"
if errorlevel 1 (
    echo [ERROR] Invalid MCUBE_BUILD_DIR "%MCUBE_BUILD_DIR%".
    exit /b 2
)

set "MCUBE_DISTRO_NAME=%MCUBE_WSL_DISTRO%"
powershell -NoProfile -Command "if ($env:MCUBE_DISTRO_NAME -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') { exit 1 }"
if errorlevel 1 (
    echo [ERROR] Invalid MCUBE_WSL_DISTRO "%MCUBE_WSL_DISTRO%".
    exit /b 2
)

set "DIST=%REPO_WIN%\dist\MCubeAIOS-%VERSION%"
set "HISTORY=%REPO_WIN%\history-version"
set "ZIP=%HISTORY%\MCubeAIOS-%VERSION%.zip"
set "BUILD_DIR=%MCUBE_BUILD_DIR%"

echo.
echo ============================================================
echo   MCubeAIOS build
echo   Version : %VERSION%
echo   Repo    : %REPO_WIN%
echo   Output  : %DIST%
echo ============================================================
echo.

rem ---- prerequisites -------------------------------------------------------
where wsl.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] wsl.exe was not found. Install WSL2 and a Debian distro.
    exit /b 2
)

rem wsl.exe in this environment treats a quoted -d argument as part of the
rem distro name ("Debian" instead of Debian) and reports
rem WSL_E_DISTRO_NOT_FOUND. The name is validated to contain no spaces above,
rem so it is passed unquoted on purpose.
wsl.exe -d %MCUBE_WSL_DISTRO% -- true >nul 2>&1
if errorlevel 1 (
    echo [ERROR] WSL distribution "%MCUBE_WSL_DISTRO%" is not available.
    echo         Install it with: wsl --install -d %MCUBE_WSL_DISTRO%
    exit /b 2
)

rem WSL cannot translate a network-drive working directory (Z:) and emits a
rem Relay "Failed to translate" error, so run all WSL commands from a local
rem directory. Every path used below is absolute.
cd /d "%SystemDrive%\"

rem ---- locate the repository inside WSL ------------------------------------
set "REPO_WSL="
if defined MCUBE_WSL_DIR set "REPO_WSL=%MCUBE_WSL_DIR%"

if not defined REPO_WSL (
    for /f "usebackq delims=" %%I in (`wsl.exe -d %MCUBE_WSL_DISTRO% -- wslpath -a "%REPO_WIN%" 2^>nul`) do set "REPO_WSL=%%I"
)

if defined REPO_WSL (
    wsl.exe -d %MCUBE_WSL_DISTRO% -- test -f "%REPO_WSL%/Makefile" >nul 2>&1
    if errorlevel 1 set "REPO_WSL="
)

rem wslpath cannot translate mapped network drives whose /mnt entry is a UNC
rem mount (Z: here), nor SMB UNC paths. Map a drive path to /mnt/<drive>/...
rem directly instead: the drive only needs to be visible inside WSL.
if not defined REPO_WSL (
    if not "%REPO_WIN:~0,1%"=="\" call :wsl_repo_from_windows "%REPO_WIN%"
)

rem A UNC working copy has no drive letter of its own; probe for the share
rem mounted under one (Z:\...).
if not defined REPO_WSL (
    set "MCUBE_REPO_TAIL=%~p0"
    set "REPO_WIN2="
    for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
        if not defined REPO_WIN2 (
            if exist "%%D:%MCUBE_REPO_TAIL%Makefile" set "REPO_WIN2=%%D:%MCUBE_REPO_TAIL%"
        )
    )
    if defined REPO_WIN2 call :wsl_repo_from_windows "%REPO_WIN2%"
)

if defined REPO_WSL if not "%REPO_WSL:~0,1%"=="/" set "REPO_WSL="

if not defined REPO_WSL (
    echo [ERROR] Cannot translate "%REPO_WIN%" to a WSL path.
    echo         Set MCUBE_WSL_DIR to the repository path inside WSL,
    echo         for example: set MCUBE_WSL_DIR=/mnt/z/MCubeAIOS
    exit /b 2
)

echo [INFO] WSL repo: %REPO_WSL%

rem ---- toolchain -----------------------------------------------------------
wsl.exe -d %MCUBE_WSL_DISTRO% -- bash -lc "for t in make gcc nasm x86_64-w64-mingw32-gcc xorriso qemu-img mformat python3; do command -v $t >/dev/null 2>&1 || { echo missing-tool:$t; exit 1; }; done"
if errorlevel 1 (
    echo [ERROR] The WSL build environment is incomplete.
    echo         Run: sudo apt install build-essential nasm gcc-mingw-w64-x86-64 xorriso qemu-system-x86 mtools python3
    exit /b 2
)

rem ---- build ---------------------------------------------------------------
echo [INFO] Building ISO / IMG / VHD / QCOW2 (jobs=%MCUBE_JOBS%)...
wsl.exe -d %MCUBE_WSL_DISTRO% -- bash -lc "cd '%REPO_WSL%' && make BUILD='%BUILD_DIR%' VERSION='%VERSION%' -j%MCUBE_JOBS% package"
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed. If QEMU is still running from a previous boot,
    echo         close it first: the image file is locked by the virtual machine.
    exit /b 1
)

if not exist "%DIST%\MCubeAIOS-%VERSION%.iso" (
    echo [ERROR] Expected ISO was not produced: %DIST%\MCubeAIOS-%VERSION%.iso
    exit /b 1
)
if not exist "%DIST%\SHA256SUMS.txt" (
    echo [ERROR] Package manifest is missing: %DIST%\SHA256SUMS.txt
    exit /b 1
)
if not exist "%DIST%\MCubeAIOS-%VERSION%-usb.img" (
    echo [ERROR] Expected USB image was not produced: %DIST%\MCubeAIOS-%VERSION%-usb.img
    exit /b 1
)

rem Rufus rejects an ISO whose El Torito entry is BIOS-only. Fail the build
rem instead of shipping a package nobody can flash.
set "ISO_UEFI="
for /f "usebackq delims=" %%I in (`wsl.exe -d %MCUBE_WSL_DISTRO% -- bash -lc "xorriso -indev '%REPO_WSL%/dist/MCubeAIOS-%VERSION%/MCubeAIOS-%VERSION%.iso' -report_el_torito plain 2>/dev/null | grep -c UEFI"`) do set "ISO_UEFI=%%I"
if not defined ISO_UEFI set "ISO_UEFI=0"
if "%ISO_UEFI%"=="0" (
    echo [ERROR] ISO has no UEFI El Torito boot entry.
    echo         Check the xorriso flags in the Makefile ISO rule.
    exit /b 1
)

rem A UEFI El Torito entry only helps a CD/DVD drive. Once the same ISO is
rem written to a USB stick the firmware scans the MBR, so the image must also
rem carry a partition of type 0xEF pointing at the FAT32 ESP (produced by
rem xorriso -append_partition). Without it Rufus writes a stick that nothing
rem can boot.
set "ISO_ESP="
for /f "usebackq delims=" %%I in (`wsl.exe -d %MCUBE_WSL_DISTRO% -- bash -lc "xorriso -indev '%REPO_WSL%/dist/MCubeAIOS-%VERSION%/MCubeAIOS-%VERSION%.iso' -report_system_area plain 2>/dev/null | grep -c '0xef'"`) do set "ISO_ESP=%%I"
if not defined ISO_ESP set "ISO_ESP=0"
if "%ISO_ESP%"=="0" (
    echo [ERROR] ISO has no 0xEF ^(EFI System Partition^) MBR entry.
    echo         A USB stick written from it would not boot in UEFI mode.
    echo         Check the -append_partition flag in the Makefile ISO rule.
    exit /b 1
)

rem Legacy BIOS never reads the El Torito catalog. It runs the 432-byte MBR
rem written by -isohybrid-mbr, which scans the partition table for the BIOS
rem payload partition (type 0x83, produced by -append_partition). Without that
rem entry the stick would only boot on UEFI machines.
set "ISO_BIOS="
for /f "usebackq delims=" %%I in (`wsl.exe -d %MCUBE_WSL_DISTRO% -- bash -lc "xorriso -indev '%REPO_WSL%/dist/MCubeAIOS-%VERSION%/MCubeAIOS-%VERSION%.iso' -report_system_area plain 2>/dev/null | grep -c '0x83'"`) do set "ISO_BIOS=%%I"
if not defined ISO_BIOS set "ISO_BIOS=0"
if "%ISO_BIOS%"=="0" (
    echo [ERROR] ISO has no 0x83 ^(BIOS payload^) MBR entry.
    echo         A USB stick written from it would not boot in legacy BIOS mode.
    echo         Check the -append_partition flag in the Makefile ISO rule.
    exit /b 1
)

rem ---- history archive -----------------------------------------------------
if not exist "%HISTORY%" mkdir "%HISTORY%"
if exist "%ZIP%" (
    echo [WARN] Replacing existing archive %ZIP%
)

set "MCUBE_DIST=%DIST%"
set "MCUBE_ZIP=%ZIP%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $src=$env:MCUBE_DIST; $dst=$env:MCUBE_ZIP; if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Force }; Compress-Archive -Path (Join-Path $src '*') -DestinationPath $dst -CompressionLevel Optimal"
if errorlevel 1 (
    echo [ERROR] Failed to create %ZIP%
    exit /b 1
)

echo.
echo ============================================================
echo   Build complete
echo ============================================================
echo   Version : %VERSION%
echo   Package : %DIST%
echo   Archive : %ZIP%
echo.
echo   Flash to USB (both images boot on UEFI *and* legacy BIOS):
echo     %DIST%\MCubeAIOS-%VERSION%.iso
echo       Rufus: image mode, partition GPT
echo     %DIST%\MCubeAIOS-%VERSION%-usb.img
echo       Rufus: DD mode (byte-for-byte write)
echo   On a BIOS-only machine pick the non-UEFI USB entry in the boot menu.
echo   Real hardware: disable Secure Boot in UEFI mode.
echo.
powershell -NoProfile -Command "$z=Get-Item -LiteralPath $env:MCUBE_ZIP; Write-Host ('  Zip size: {0:N1} MiB' -f ($z.Length/1MB)); $h=(Get-FileHash -LiteralPath $env:MCUBE_ZIP -Algorithm SHA256).Hash; Write-Host ('  Zip SHA256: ' + $h)"
echo.

endlocal
exit /b 0

rem ---- helpers -------------------------------------------------------------
rem Translate <drive>:\<tail> to /mnt/<drive>/<tail> without wslpath, which
rem refuses mapped network drives. Only accepts the path if the Makefile is
rem actually visible from inside the distribution.
:wsl_repo_from_windows
set "WP_DRIVE=%~1"
set "WP_DRIVE=%WP_DRIVE:~0,1%"
set "WP_TAIL=%~1"
set "WP_TAIL=%WP_TAIL:~2%"
if "%WP_TAIL:~0,1%"=="\" set "WP_TAIL=%WP_TAIL:~1%"
set "WP_TAIL=%WP_TAIL:\=/%"
set "WP_LC="
for %%D in (c d e f g h i j k l m n o p q r s t u v w x y z) do (
    if /I "%WP_DRIVE%"=="%%D" set "WP_LC=%%D"
)
if not defined WP_LC exit /b 0
set "WP_CAND=/mnt/%WP_LC%/%WP_TAIL%"
wsl.exe -d %MCUBE_WSL_DISTRO% -- test -f "%WP_CAND%/Makefile" >nul 2>&1
if errorlevel 1 exit /b 0
set "REPO_WSL=%WP_CAND%"
exit /b 0

