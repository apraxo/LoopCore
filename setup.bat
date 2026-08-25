@echo off
setlocal EnableDelayedExpansion
rem ===========================================================================
rem  loopcore setup -- installs everything needed to build and run.
rem
rem  Handles automatically, via winget:
rem      git, Visual Studio 2022 Build Tools (+ C++ toolset and Windows SDK),
rem      CUDA Toolkit, and optionally Python + ultralytics for exporting
rem      engines.
rem
rem  Cannot handle automatically:
rem      TensorRT. NVIDIA requires a developer account and an accepted EULA,
rem      so there is no unattended download. This script opens the page, takes
rem      the path to the zip you downloaded, extracts it, and sets
rem      TENSORRT_DIR for you.
rem
rem  Run this once. Then run build.bat.
rem ===========================================================================

title loopcore setup

rem ---------------------------------------------------------------- elevate
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

cd /d "%~dp0"
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

echo.
echo  ===============================================================
echo    loopcore setup
echo  ===============================================================
echo.

set "NEEDS_REBOOT=0"

rem ----------------------------------------------------------------- winget
where winget >nul 2>&1
if errorlevel 1 (
    echo  [x] winget is not available on this machine.
    echo.
    echo      winget ships with App Installer. Install it from the Microsoft
    echo      Store ^("App Installer"^), or update Windows, then run this again.
    echo.
    echo      On Windows Server or an offline machine, install these by hand:
    echo        - Visual Studio 2022 Build Tools, workload "Desktop development with C++"
    echo        - CUDA Toolkit 12.x
    echo        - Git
    echo.
    pause
    exit /b 1
)
echo  [+] winget found.

rem ------------------------------------------------------------ nvidia driver
where nvidia-smi >nul 2>&1
if errorlevel 1 (
    echo.
    echo  [!] nvidia-smi was not found, so no NVIDIA driver is installed.
    echo      Install the driver for your GPU from nvidia.com/download first;
    echo      CUDA and TensorRT will not work without it.
    echo.
    choice /C YN /M "Open the driver download page now"
    if !errorlevel! EQU 1 start "" "https://www.nvidia.com/download/index.aspx"
    echo.
    echo      Install the driver, reboot, then run setup.bat again.
    pause
    exit /b 1
)
for /f "tokens=*" %%g in ('nvidia-smi --query-gpu^=name --format^=csv^,noheader 2^>nul') do set "GPUNAME=%%g"
echo  [+] GPU: !GPUNAME!

rem -------------------------------------------------------------------- git
where git >nul 2>&1
if errorlevel 1 (
    echo.
    echo  [.] Installing Git...
    winget install --id Git.Git -e --source winget ^
        --accept-package-agreements --accept-source-agreements
    set "NEEDS_REBOOT=1"
) else (
    echo  [+] Git already installed.
)

rem --------------------------------------------------- visual studio toolset
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "HAVE_VC=0"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -property installationPath 2^>nul`) do set "HAVE_VC=1"
)
if "!HAVE_VC!"=="0" (
    echo.
    echo  [.] Installing Visual Studio 2022 Build Tools with the C++ toolset.
    echo      This is about 7 GB and takes 10-25 minutes. Leave it running.
    echo.
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget ^
        --accept-package-agreements --accept-source-agreements ^
        --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended"
    if errorlevel 1 (
        echo.
        echo  [x] The Build Tools install did not complete.
        echo      Install it by hand from visualstudio.microsoft.com/downloads
        echo      ^(scroll to "Tools for Visual Studio" -^> "Build Tools"^) and
        echo      tick "Desktop development with C++".
        pause
        exit /b 1
    )
    set "NEEDS_REBOOT=1"
) else (
    echo  [+] Visual Studio C++ toolset already installed.
)

rem -------------------------------------------------------------------- cuda
set "HAVE_CUDA=0"
if not "%CUDA_PATH%"=="" if exist "%CUDA_PATH%\bin\nvcc.exe" set "HAVE_CUDA=1"
if "!HAVE_CUDA!"=="0" (
    for /d %%d in ("%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v*") do (
        if exist "%%d\bin\nvcc.exe" set "HAVE_CUDA=1"
    )
)
if "!HAVE_CUDA!"=="0" (
    echo.
    echo  [.] Installing the CUDA Toolkit. About 3 GB.
    echo.
    echo      Whichever major version installs, note it. The TensorRT zip
    echo      you download later must match: cuda-12.x or cuda-13.x.
    echo.
    winget install --id Nvidia.CUDA -e --source winget ^
        --accept-package-agreements --accept-source-agreements
    if errorlevel 1 (
        echo.
        echo  [x] The CUDA install did not complete.
        echo      Install it by hand from developer.nvidia.com/cuda-downloads
        echo      and choose the exe ^(local^) installer.
        pause
        exit /b 1
    )
    set "NEEDS_REBOOT=1"
) else (
    echo  [+] CUDA Toolkit already installed.
)

rem ---------------------------------------------------------------- tensorrt
call :detect_cuda_major
call :setup_tensorrt
if errorlevel 1 exit /b 1

rem ------------------------------------------------------- python (optional)
echo.
choice /C YN /M "Install Python and ultralytics, for exporting .engine files"
if !errorlevel! EQU 1 (
    where python >nul 2>&1
    if errorlevel 1 (
        echo  [.] Installing Python 3.11...
        winget install --id Python.Python.3.11 -e --source winget ^
            --accept-package-agreements --accept-source-agreements
        set "NEEDS_REBOOT=1"
        echo.
        echo      Python needs a new terminal before pip works. After the
        echo      reboot below, run:
        echo          pip install ultralytics
    ) else (
        echo  [.] Installing ultralytics...
        python -m pip install --upgrade pip
        python -m pip install ultralytics
    )
)

rem --------------------------------------------- arduino-cli (optional)
echo.
choice /C YN /M "Install arduino-cli, so loopcore can flash an Arduino for you"
if !errorlevel! EQU 1 (
    where arduino-cli >nul 2>&1
    if errorlevel 1 (
        echo  [.] Installing arduino-cli...
        winget install --id ArduinoSA.CLI -e --source winget ^
            --accept-package-agreements --accept-source-agreements
        if errorlevel 1 (
            echo.
            echo  [!] winget could not install it. Download the Windows zip from
            echo      https://arduino.github.io/arduino-cli/latest/installation/
            echo      and put arduino-cli.exe in loopcore\build\bin\tools
            echo      Flashing from inside loopcore needs it; nothing else does.
        ) else (
            set "NEEDS_REBOOT=1"
        )
    ) else (
        echo  [+] arduino-cli already installed.
    )
)

rem ------------------------------------------------- version sanity checks
call :check_versions

rem ----------------------------------------------------------------- wrap up
echo.
echo  ===============================================================
echo    Setup complete.
echo  ===============================================================
echo.
if "!NEEDS_REBOOT!"=="1" (
    echo    Something installed here changed PATH or the environment.
    echo    Reboot, then run build.bat.
    echo.
    choice /C YN /M "Reboot now"
    if !errorlevel! EQU 1 shutdown /r /t 5 /c "loopcore setup"
) else (
    echo    Run build.bat next.
)
echo.
pause
exit /b 0


rem ===========================================================================
:detect_cuda_major
rem ===========================================================================
set "CUDAMAJ="
for /f "tokens=*" %%v in ('nvcc --version 2^>nul ^| findstr /C:"release"') do set "NVCCLINE=%%v"
if defined NVCCLINE (
    for /f "tokens=2 delims=V." %%a in ("!NVCCLINE!") do set "CUDAMAJ=%%a"
)
if not defined CUDAMAJ (
    for /d %%d in ("%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v*") do (
        set "CV=%%~nxd"
        set "CV=!CV:v=!"
        for /f "tokens=1 delims=." %%a in ("!CV!") do set "CUDAMAJ=%%a"
    )
)
if not defined CUDAMAJ set "CUDAMAJ=12"
exit /b 0


rem ===========================================================================
:check_versions
rem  Two mismatches cost hours if they are found at link time instead of now.
rem ===========================================================================
echo.
echo  [.] Checking version compatibility...

set "CUDAMAJ="
for /f "tokens=*" %%v in ('nvcc --version 2^>nul ^| findstr /C:"release"') do set "NVCCLINE=%%v"
if defined NVCCLINE (
    for /f "tokens=2 delims=V." %%a in ("!NVCCLINE!") do set "CUDAMAJ=%%a"
)
if not defined CUDAMAJ (
    for /d %%d in ("%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v*") do (
        set "CV=%%~nxd"
        set "CV=!CV:v=!"
        for /f "tokens=1 delims=." %%a in ("!CV!") do set "CUDAMAJ=%%a"
    )
)

set "TRTMAJ="
if not "%TENSORRT_DIR%"=="" (
    for %%d in ("%TENSORRT_DIR%") do set "TRTNAME=%%~nxd"
    for /f "tokens=2 delims=-." %%a in ("!TRTNAME!") do set "TRTMAJ=%%a"
)

if defined CUDAMAJ echo      CUDA major version    !CUDAMAJ!
if defined TRTMAJ  echo      TensorRT major version !TRTMAJ!

if "!TRTMAJ!"=="11" (
    echo.
    echo  [!] TensorRT 11.x detected.
    echo.
    echo      loopcore targets the 10.x API. TensorRT 11 removed
    echo      BuilderFlag::kFP16, BuilderFlag::kINT8, IInt8Calibrator, and
    echo      the IPluginV2 family. That breaks both this build and the
    echo      ultralytics export flags half=True and int8=True.
    echo.
    echo      Download TensorRT 10.16.x instead and point TENSORRT_DIR at it.
    echo.
    pause
)

if "!CUDAMAJ!"=="13" (
    echo.
    echo  [i] CUDA 13 detected. That is fine -- TensorRT 10.16 has a CUDA 13
    echo      build. Make sure you downloaded the zip whose name ends in
    echo      cuda-13.x, not cuda-12.9. Mixing the two fails at link time.
    echo.
)
if "!CUDAMAJ!"=="12" (
    echo.
    echo  [i] CUDA 12 detected. Download the TensorRT zip whose name ends in
    echo      cuda-12.x, not cuda-13.x.
    echo.
)
exit /b 0


rem ===========================================================================
:setup_tensorrt
rem ===========================================================================
if not "%TENSORRT_DIR%"=="" if exist "%TENSORRT_DIR%\include\NvInfer.h" (
    echo  [+] TensorRT already set up at %TENSORRT_DIR%
    exit /b 0
)
for /d %%d in ("C:\TensorRT*") do (
    if exist "%%d\include\NvInfer.h" (
        echo  [+] Found TensorRT at %%d
        setx TENSORRT_DIR "%%d" >nul
        set "TENSORRT_DIR=%%d"
        set "NEEDS_REBOOT=1"
        exit /b 0
    )
)

echo.
echo  ---------------------------------------------------------------
echo    TensorRT needs one manual step.
echo.
echo    NVIDIA requires a free developer account and an accepted EULA
echo    to download it, so no script can fetch it unattended.
echo.
echo    On the page that opens:
echo      1. Sign in or create a free NVIDIA developer account
echo      2. Accept the terms
echo      3. Under "TensorRT 10", pick the newest 10.x release
echo         ^(10.16 or later^). Do NOT take 11.x: it removed the
echo         APIs this build and the ultralytics exporter both use.
echo      4. Expand "Windows" and take the ZIP whose name ends in
echo         cuda-!CUDAMAJ!.x -- it must match the CUDA toolkit here.
echo.
echo         You want a file named like:
echo             TensorRT-10.16.1.11.Windows.amd64.cuda-!CUDAMAJ!.2.zip
echo             ^<version^>       ^<platform^>    ^<must be cuda-!CUDAMAJ!.x^>
echo.
echo         NOT the .tar.gz ^(that is Linux^) and NOT "pip install
echo         tensorrt" ^(Python bindings only, no C++ headers^).
echo         The ZIP is the only download with include\ and lib\.
echo  ---------------------------------------------------------------
echo.
choice /C YN /M "Open the TensorRT download page now"
if !errorlevel! EQU 1 start "" "https://developer.nvidia.com/tensorrt/download"

echo.
echo    When the download has finished, paste the full path to either
echo    the ZIP file or an already-extracted folder, and press Enter.
echo    Leave it blank to skip and set TENSORRT_DIR yourself later.
echo.
set "TRTIN="
set /p "TRTIN=Path: "

if "!TRTIN!"=="" (
    echo.
    echo  [!] Skipped. Before running build.bat, do this in a terminal:
    echo          setx TENSORRT_DIR "C:\path\to\TensorRT-10.x.x.x"
    echo      then open a new terminal.
    exit /b 0
)

rem strip surrounding quotes if the user dragged the file in
set "TRTIN=!TRTIN:"=!"

if exist "!TRTIN!\include\NvInfer.h" (
    setx TENSORRT_DIR "!TRTIN!" >nul
    set "TENSORRT_DIR=!TRTIN!"
    set "NEEDS_REBOOT=1"
    echo  [+] TENSORRT_DIR set to !TRTIN!
    exit /b 0
)

if /I not "!TRTIN:~-4!"==".zip" (
    echo.
    echo  [x] That path is neither a .zip nor a folder containing
    echo      include\NvInfer.h.
    echo      Check the path and run setup.bat again.
    pause
    exit /b 1
)
if not exist "!TRTIN!" (
    echo.
    echo  [x] No file at !TRTIN!
    pause
    exit /b 1
)

echo.
echo  [.] Extracting to C:\TensorRT ...
if not exist "C:\TensorRT" mkdir "C:\TensorRT"
powershell -NoProfile -Command ^
    "Expand-Archive -LiteralPath '!TRTIN!' -DestinationPath 'C:\TensorRT' -Force"
if errorlevel 1 (
    echo  [x] Extraction failed. Unzip it by hand, then run setup.bat again
    echo      and give it the extracted folder.
    pause
    exit /b 1
)

set "TRTFOUND="
for /d %%d in ("C:\TensorRT\*") do (
    if exist "%%d\include\NvInfer.h" set "TRTFOUND=%%d"
)
if exist "C:\TensorRT\include\NvInfer.h" set "TRTFOUND=C:\TensorRT"

if "!TRTFOUND!"=="" (
    echo  [x] Extracted, but include\NvInfer.h is not where expected.
    echo      Look in C:\TensorRT, find the folder that contains include\
    echo      and lib\, then run:
    echo          setx TENSORRT_DIR "that\folder"
    pause
    exit /b 1
)

setx TENSORRT_DIR "!TRTFOUND!" >nul
set "TENSORRT_DIR=!TRTFOUND!"
set "NEEDS_REBOOT=1"
echo  [+] TENSORRT_DIR set to !TRTFOUND!
exit /b 0
