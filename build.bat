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
if not exist "%TENSORRT_DIR%\lib\nvinfer*.lib" (
    echo  [x] No nvinfer import library under "%TENSORRT_DIR%\lib".
    echo      This looks like a partial extract. Unzip the package again.
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

rem Compiler output goes to a file so it survives the window closing.
set "LOG=%BUILD%\build.log"
if exist "%LOG%" del "%LOG%"
echo loopcore build log  %DATE% %TIME% > "%LOG%"
echo VS       = %VSPATH% >> "%LOG%"
echo CUDA     = %CUDA_PATH% >> "%LOG%"
echo TENSORRT = %TENSORRT_DIR% >> "%LOG%"
echo. >> "%LOG%"

rem ================================================================== flags
rem NOMINMAX must be defined before ANY header is read. Putting it in a .cpp
rem is not enough -- windows.h arrives through d3d11.h in the first include,
rem and then std::max(a, b) expands to std::(a > b ? a : b).
set "WINDEFS=/DNOMINMAX /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00"

set "INC=/I"%SRC%" /I"%IMGUI%" /I"%IMGUI%\backends" /I"%CUDA_PATH%\include" /I"%TENSORRT_DIR%\include""

if /I "%MODE%"=="debug" (
    set "CFLAGS=/nologo /std:c++17 /EHsc /MDd /Od /Zi /W3 /D_DEBUG %WINDEFS%"
    set "NVFLAGS=-std=c++17 -G -g -Xcompiler "/MDd /EHsc""
    set "LFLAGS=/DEBUG"
) else (
    set "CFLAGS=/nologo /std:c++17 /EHsc /MD /O2 /Oi /GL /W3 /DNDEBUG %WINDEFS%"
    set "NVFLAGS=-std=c++17 -O3 -lineinfo -Xcompiler "/MD /EHsc /O2""
    set "LFLAGS=/LTCG /OPT:REF /OPT:ICF"
)

rem Turing = sm_75 (2080 Ti). PTX for anything newer so it still runs there.
set "ARCH=-gencode arch=compute_75,code=sm_75 -gencode arch=compute_75,code=compute_75"

rem =============================================================== compile cu
echo.
echo  [.] preprocess.cu
echo === nvcc preprocess.cu === >> "%LOG%"
"%CUDA_PATH%\bin\nvcc.exe" %NVFLAGS% %ARCH% -I"%SRC%" -I"%CUDA_PATH%\include" ^
    -c "%SRC%\preprocess.cu" -o "%BUILD%\obj\preprocess.obj" >> "%LOG%" 2>&1
if errorlevel 1 goto fail

rem ============================================================== compile cpp
set "SOURCES=main.cpp capture.cpp engine.cpp overlay.cpp chrome.cpp theme.cpp builder.cpp store.cpp hwinfo.cpp crashlog.cpp control.cpp perfmon.cpp serialports.cpp flasher.cpp rawinput.cpp capture_writer.cpp tuner.cpp gpumon.cpp sessiontrack.cpp sketch.cpp paths.cpp"
for %%f in (%SOURCES%) do (
    echo  [.] %%f
    echo === cl %%f === >> "%LOG%"
    cl %CFLAGS% %INC% /c "%SRC%\%%f" /Fo"%BUILD%\obj\%%~nf.obj" >> "%LOG%" 2>&1
    if errorlevel 1 (
        echo  [x] %%f failed to compile. See the log.
        goto fail
    )
    rem Belt and braces: a compiler that returns success but writes nothing
    rem leaves the link to fail later with a page of unresolved symbols and
    rem no indication of which file was responsible.
    if not exist "%BUILD%\obj\%%~nf.obj" (
        echo  [x] %%f reported success but produced no object file.
        goto fail
    )
)

set "IMSRC=imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp"
for %%f in (%IMSRC%) do (
    if not exist "%BUILD%\obj\%%~nf.obj" (
        echo  [.] imgui\%%f
        echo === cl imgui/%%f === >> "%LOG%"
        cl %CFLAGS% %INC% /c "%IMGUI%\%%f" /Fo"%BUILD%\obj\%%~nf.obj" >> "%LOG%" 2>&1
        if errorlevel 1 goto fail
    )
)
for %%f in (imgui_impl_win32.cpp imgui_impl_dx11.cpp) do (
    if not exist "%BUILD%\obj\%%~nf.obj" (
        echo  [.] imgui\backends\%%f
        echo === cl imgui/backends/%%f === >> "%LOG%"
        cl %CFLAGS% %INC% /c "%IMGUI%\backends\%%f" /Fo"%BUILD%\obj\%%~nf.obj" >> "%LOG%" 2>&1
        if errorlevel 1 goto fail
    )
)

rem =============================================================== resources
echo.
echo  [.] loopcore.rc
if not exist "%ROOT%\res\loopcore.ico" (
    echo  [!] res\loopcore.ico is missing. Building without an icon.
    set "RESOBJ="
) else (
    echo === rc loopcore.rc === >> "%LOG%"
    rc /nologo /I "%SRC%" /fo "%BUILD%\obj\loopcore.res" "%ROOT%\res\loopcore.rc" >> "%LOG%" 2>&1
    if errorlevel 1 (
        echo  [!] rc.exe failed. Building without an icon; see the log.
        set "RESOBJ="
    ) else (
        set "RESOBJ="%BUILD%\obj\loopcore.res""
    )
)

rem ==================================================================== link
echo.
echo  [.] linking
rem TensorRT 10 versioned its Windows import libraries: nvinfer_10.lib rather
rem than nvinfer.lib. Probe instead of guessing, so a future rename does not
rem break this again. Only the core and plugin libs are wanted -- nvinfer_lean,
rem nvinfer_dispatch and friends are separate runtimes and must not be linked.
set "TRT_CORE="
set "TRT_PLUGIN="
for %%n in (nvinfer_10.lib nvinfer_11.lib nvinfer_12.lib nvinfer.lib) do (
    if not defined TRT_CORE if exist "%TENSORRT_DIR%\lib\%%n" set "TRT_CORE=%%n"
)
for %%n in (nvinfer_plugin_10.lib nvinfer_plugin_11.lib nvinfer_plugin_12.lib nvinfer_plugin.lib) do (
    if not defined TRT_PLUGIN if exist "%TENSORRT_DIR%\lib\%%n" set "TRT_PLUGIN=%%n"
)
set "TRT_ONNX="
for %%n in (nvonnxparser_10.lib nvonnxparser_11.lib nvonnxparser_12.lib nvonnxparser.lib) do (
    if not defined TRT_ONNX if exist "%TENSORRT_DIR%\lib\%%n" set "TRT_ONNX=%%n"
)
if not defined TRT_ONNX (
    echo  [x] No nvonnxparser import library under "%TENSORRT_DIR%\lib".
    echo      It is needed to convert .pt and .onnx models in the app.
    echo      Here is what is actually there:
    dir /b "%TENSORRT_DIR%\lib\*.lib" 2>nul
    goto fail
)

if not defined TRT_CORE (
    echo.
    echo  [x] No TensorRT core import library under
    echo      "%TENSORRT_DIR%\lib"
    echo      Looked for nvinfer_10.lib, nvinfer_11.lib, nvinfer_12.lib, nvinfer.lib.
    echo      Here is what is actually there:
    dir /b "%TENSORRT_DIR%\lib\*.lib" 2>nul
    goto fail
)
if not defined TRT_PLUGIN (
    echo  [!] No nvinfer_plugin library found. Engines exported with nms=True
    echo      use EfficientNMS_TRT and will fail to load. Linking without it.
    set "TRT_PLUGIN="
)
echo      TensorRT libs: !TRT_CORE! !TRT_PLUGIN! !TRT_ONNX!

set "LIBS=d3d11.lib dxgi.lib dxguid.lib windowsapp.lib user32.lib gdi32.lib shcore.lib winmm.lib ole32.lib dwmapi.lib comdlg32.lib shell32.lib dbghelp.lib setupapi.lib psapi.lib windowscodecs.lib shlwapi.lib"
set "LIBS=%LIBS% cudart.lib !TRT_CORE! !TRT_PLUGIN! !TRT_ONNX!"

rem Full paths, via %%~fo.
rem
rem A for loop over a wildcard yields the bare file name, so the linker was
rem being handed "engine.obj" and left to resolve it against the working
rem directory. Whether that happens to work depends on where the script was
rem launched from, which is not something a build should depend on.
set "OBJS="
for %%o in ("%BUILD%\obj\*.obj") do set "OBJS=!OBJS! "%%~fo""
if "!OBJS!"=="" (
    echo  [x] No object files in %BUILD%\obj. Nothing compiled.
    goto fail
)

rem Every source must have contributed an object. Catching a missing one
rem here names the file; catching it at link time produces a page of
rem unresolved symbols that name everything except the cause.
set "MISSING="
for %%f in (%SOURCES%) do (
    if not exist "%BUILD%\obj\%%~nf.obj" set "MISSING=!MISSING! %%f"
)
if not "!MISSING!"=="" (
    echo  [x] These compiled but produced no object file:!MISSING!
    goto fail
)

set /a OBJCOUNT=0
for %%o in ("%BUILD%\obj\*.obj") do set /a OBJCOUNT+=1
echo objects linked: !OBJCOUNT! >> "%LOG%"

echo === link === >> "%LOG%"
link /nologo %LFLAGS% /SUBSYSTEM:WINDOWS /OUT:"%BUILD%\loopcore.exe" ^
    !OBJS! !RESOBJ! ^
    /LIBPATH:"%CUDA_PATH%\lib\x64" /LIBPATH:"%TENSORRT_DIR%\lib" ^
    %LIBS% >> "%LOG%" 2>&1
if errorlevel 1 goto fail

if not exist "%BUILD%\loopcore.exe" (
    echo  [x] link reported success but loopcore.exe is not there.
    goto fail
)

rem ================================= copy the firmware sketch next to the exe
rem The in-app flasher looks for it there, not in the source tree.
if exist "%ROOT%\firmware" (
    if not exist "%BUILD%\firmware" mkdir "%BUILD%\firmware"
    xcopy /e /i /y /q "%ROOT%\firmware" "%BUILD%\firmware" >nul
)

rem ============================================== copy runtime DLLs next to exe
rem TensorRT 10.16 moved the Windows .dll files from lib\ to bin\. Older
rem releases keep them in lib\. Check both so either layout works.
set "TRTDLLS=0"
if exist "%TENSORRT_DIR%\bin\*.dll" (
    for %%d in ("%TENSORRT_DIR%\bin\*.dll") do (
        if not exist "%BUILD%\%%~nxd" copy /y "%%d" "%BUILD%\" >nul
        set "TRTDLLS=1"
    )
)
if exist "%TENSORRT_DIR%\lib\*.dll" (
    for %%d in ("%TENSORRT_DIR%\lib\*.dll") do (
        if not exist "%BUILD%\%%~nxd" copy /y "%%d" "%BUILD%\" >nul
        set "TRTDLLS=1"
    )
)
if "!TRTDLLS!"=="0" (
    echo  [!] No TensorRT DLLs found in bin\ or lib\. loopcore.exe will not
    echo      start. Check that TENSORRT_DIR points at the extracted package.
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
echo.
echo   Models and settings live in build\bin. To regenerate the icon after
echo   editing tools\make_icon.py:  python tools\make_icon.py
echo  ---------------------------------------------------------------
echo.
pause
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
echo   Build failed. Last 40 lines of the compiler output:
echo  ---------------------------------------------------------------
if exist "%LOG%" (
    powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG%' -Tail 40"
) else (
    echo   No log was written -- the failure happened before compiling.
)
echo  ---------------------------------------------------------------
echo   Full log: %LOG%
echo.
echo   The FIRST error in that file is the one to fix; the rest are
echo   usually knock-on effects. If it is a missing header or .lib,
echo   a prerequisite installed only partly -- run setup.bat again.
echo  ---------------------------------------------------------------
echo.
pause
endlocal
exit /b 1
