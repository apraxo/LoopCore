@echo off
setlocal EnableDelayedExpansion
rem ===========================================================================
rem  loopcore build
rem
rem  Needs, one time only:
rem    * Visual Studio 2019/2022 with "Desktop development with C++"
rem    * Windows 10 SDK 10.0.19041 or newer  (ships with the above)
rem    * CUDA Toolkit 11.8 or 12.x
rem    * TensorRT 8.6 or 10.x, unzipped anywhere
rem    * git, for fetching Dear ImGui the first time
rem
rem  Point TENSORRT_DIR at the folder containing include\ and lib\, either as
rem  an environment variable or by editing the line below.
rem
rem  Usage:  build.bat            release
rem          build.bat debug      debug symbols, no optimisation
rem          build.bat clean      wipe build\ and start over
rem ===========================================================================

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "SRC=%ROOT%\src"
set "BUILD=%ROOT%\build"
set "THIRD=%ROOT%\third_party"
set "IMGUI=%THIRD%\imgui"

rem ---- edit here if you do not want an environment variable ----------------
if "%TENSORRT_DIR%"=="" set "TENSORRT_DIR="

if /I "%~1"=="clean" (
    echo Removing %BUILD%
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    echo Done.
    exit /b 0
)

set "MODE=release"
if /I "%~1"=="debug" set "MODE=debug"

echo.
echo  loopcore build [%MODE%]
echo  ---------------------------------------------------------------

rem =========================================================== Visual Studio
if defined VCINSTALLDIR goto vs_ready

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    call :missing "Visual Studio C++ build tools"
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    call :missing "the Visual Studio C++ toolset"
    exit /b 1
)
echo  [+] Visual Studio  %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo  [x] vcvars64.bat failed. The C++ toolset may be incomplete.
    exit /b 1
)
:vs_ready

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo  [x] cl.exe is not on PATH after running vcvars64.
    exit /b 1
)

rem ==================================================================== CUDA
if "%CUDA_PATH%"=="" (
    for /d %%d in ("%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v*") do set "CUDA_PATH=%%d"
)
if "%CUDA_PATH%"=="" (
    call :missing "the CUDA Toolkit"
    exit /b 1
)
if not exist "%CUDA_PATH%\bin\nvcc.exe" (
    echo  [x] CUDA_PATH is set to "%CUDA_PATH%" but bin\nvcc.exe is not there.
    exit /b 1
)
echo  [+] CUDA            %CUDA_PATH%

rem ================================================================ TensorRT
if "%TENSORRT_DIR%"=="" (
    for /d %%d in ("%ProgramFiles%\NVIDIA\TensorRT*") do set "TENSORRT_DIR=%%d"
)
if "%TENSORRT_DIR%"=="" (
    for /d %%d in ("C:\TensorRT*") do set "TENSORRT_DIR=%%d"
)
if "%TENSORRT_DIR%"=="" (
    call :missing "TensorRT"
    exit /b 1
)
if not exist "%TENSORRT_DIR%\include\NvInfer.h" (
    echo  [x] TENSORRT_DIR is "%TENSORRT_DIR%" but include\NvInfer.h is missing.
    echo      Point it at the folder that directly contains include\ and lib\.
    exit /b 1
)
echo  [+] TensorRT        %TENSORRT_DIR%

rem =================================================================== ImGui
if not exist "%IMGUI%\imgui.cpp" (
    echo  [.] Fetching Dear ImGui...
    where git >nul 2>&1
    if errorlevel 1 (
        call :missing "git"
        exit /b 1
    )
    if not exist "%THIRD%" mkdir "%THIRD%"
    git clone --depth 1 --branch docking https://github.com/ocornut/imgui "%IMGUI%" >nul 2>&1
    if errorlevel 1 (
        echo  [x] git clone failed. Check the network connection.
        exit /b 1
    )
)
echo  [+] Dear ImGui      %IMGUI%

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%BUILD%\obj" mkdir "%BUILD%\obj"

rem ================================================================== flags
set "INC=/I"%SRC%" /I"%IMGUI%" /I"%IMGUI%\backends" /I"%CUDA_PATH%\include" /I"%TENSORRT_DIR%\include""

if /I "%MODE%"=="debug" (
    set "CFLAGS=/nologo /std:c++17 /EHsc /MDd /Od /Zi /W3 /D_DEBUG"
    set "NVFLAGS=-std=c++17 -G -g -Xcompiler "/MDd /EHsc""
    set "LFLAGS=/DEBUG"
) else (
    set "CFLAGS=/nologo /std:c++17 /EHsc /MD /O2 /Oi /GL /W3 /DNDEBUG"
    set "NVFLAGS=-std=c++17 -O3 -lineinfo -Xcompiler "/MD /EHsc /O2""
    set "LFLAGS=/LTCG /OPT:REF /OPT:ICF"
)

rem Turing = sm_75 (2080 Ti). PTX for anything newer so it still runs there.
set "ARCH=-gencode arch=compute_75,code=sm_75 -gencode arch=compute_75,code=compute_75"

rem =============================================================== compile cu
echo.
echo  [.] preprocess.cu
"%CUDA_PATH%\bin\nvcc.exe" %NVFLAGS% %ARCH% -I"%SRC%" -I"%CUDA_PATH%\include" ^
    -c "%SRC%\preprocess.cu" -o "%BUILD%\obj\preprocess.obj"
if errorlevel 1 goto fail

rem ============================================================== compile cpp
set "SOURCES=main.cpp capture.cpp engine.cpp overlay.cpp"
for %%f in (%SOURCES%) do (
    echo  [.] %%f
    cl %CFLAGS% %INC% /c "%SRC%\%%f" /Fo"%BUILD%\obj\%%~nf.obj"
    if errorlevel 1 goto fail
)

set "IMSRC=imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp"
for %%f in (%IMSRC%) do (
    if not exist "%BUILD%\obj\%%~nf.obj" (
        echo  [.] imgui\%%f
        cl %CFLAGS% %INC% /c "%IMGUI%\%%f" /Fo"%BUILD%\obj\%%~nf.obj"
        if errorlevel 1 goto fail
    )
)
for %%f in (imgui_impl_win32.cpp imgui_impl_dx11.cpp) do (
    if not exist "%BUILD%\obj\%%~nf.obj" (
        echo  [.] imgui\backends\%%f
        cl %CFLAGS% %INC% /c "%IMGUI%\backends\%%f" /Fo"%BUILD%\obj\%%~nf.obj"
        if errorlevel 1 goto fail
    )
)

rem ==================================================================== link
echo.
echo  [.] linking
set "LIBS=d3d11.lib dxgi.lib dxguid.lib windowsapp.lib user32.lib gdi32.lib shcore.lib winmm.lib ole32.lib"
set "LIBS=%LIBS% cudart.lib nvinfer.lib nvinfer_plugin.lib"

link /nologo %LFLAGS% /SUBSYSTEM:WINDOWS /OUT:"%BUILD%\loopcore.exe" ^
    "%BUILD%\obj\*.obj" ^
    /LIBPATH:"%CUDA_PATH%\lib\x64" /LIBPATH:"%TENSORRT_DIR%\lib" ^
    %LIBS%
if errorlevel 1 goto fail

rem ============================================== copy runtime DLLs next to exe
for %%d in ("%TENSORRT_DIR%\lib\*.dll") do (
    if not exist "%BUILD%\%%~nxd" copy /y "%%d" "%BUILD%\" >nul
)
for %%d in ("%CUDA_PATH%\bin\cudart64*.dll") do (
    if not exist "%BUILD%\%%~nxd" copy /y "%%d" "%BUILD%\" >nul
)

echo.
echo  ---------------------------------------------------------------
echo   Built %BUILD%\loopcore.exe
echo.
echo   Put your engine next to it, or type the full path in the Loop tab:
echo       yolo export model=best.pt format=engine half=True nms=True device=0
echo.
echo   Engines are tied to one GPU model and one TensorRT version, so
echo   export on the machine that will run this.
echo  ---------------------------------------------------------------
endlocal
exit /b 0

:missing
echo.
echo  ---------------------------------------------------------------
echo   Missing: %~1
echo.
if exist "%ROOT%\setup.bat" (
    echo   setup.bat installs the prerequisites for you.
    echo.
    choice /C YN /M "  Run setup.bat now"
    if errorlevel 2 goto :eof
    start "" "%ROOT%\setup.bat"
    echo.
    echo   Setup is running in another window. When it finishes ^(and after
    echo   the reboot it may ask for^), run build.bat again.
) else (
    echo   setup.bat is not next to build.bat. Re-extract the zip so the
    echo   folder layout is intact.
)
echo  ---------------------------------------------------------------
goto :eof

:fail
echo.
echo  ---------------------------------------------------------------
echo   Build failed. The first error above is the one to fix; the rest
echo   are usually knock-on effects.
echo.
echo   If it is a missing header or .lib, a prerequisite installed only
echo   partly -- run setup.bat again.
echo  ---------------------------------------------------------------
endlocal
exit /b 1
