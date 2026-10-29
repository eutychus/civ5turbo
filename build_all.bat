@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

REM Locate a 32-bit MSVC environment if cl.exe is not already configured for x86
where cl.exe >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
            if exist "%%i\VC\Auxiliary\Build\vcvars32.bat" (
                call "%%i\VC\Auxiliary\Build\vcvars32.bat"
            )
        )
    )
)

where cl.exe >nul 2>nul
if errorlevel 1 (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
    ) else (
        echo [ERROR] Could not find vcvars32.bat. Run from an x86 Native Tools Command Prompt.
        exit /b 1
    )
)
if errorlevel 1 exit /b 1

if not exist "%ROOT_DIR%\dist" mkdir "%ROOT_DIR%\dist"

pushd "%ROOT_DIR%\luajit\src"
call msvcbuild.bat
if errorlevel 1 (
    popd
    exit /b 1
)

copy /Y lua51.dll "%ROOT_DIR%\dist\lua51_Win32.dll" >nul
if errorlevel 1 (
    popd
    exit /b 1
)

REM Clean intermediate build artifacts in luajit\src
del /Q *.obj *.lib *.exp *.pdb *.manifest lua51.dll luajit.exe minilua.exe buildvm.exe >nul 2>nul
popd

pushd "%ROOT_DIR%\benchmark"
cl /nologo /O2 /W3 civ5_bench_harness.c /Fe:civ5_bench_harness.exe
if errorlevel 1 (
    popd
    exit /b 1
)
del /Q civ5_bench_harness.obj >nul 2>nul
copy /Y civ5_bench_harness.exe "%ROOT_DIR%\dist\civ5_bench_harness.exe" >nul
popd

echo [+] BUILD SUCCESSFUL: "%ROOT_DIR%\dist\lua51_Win32.dll"
