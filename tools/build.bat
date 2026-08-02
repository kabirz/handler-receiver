@echo off
REM 编译验证脚本: 在 MSVC 环境下配置并编译 handler-receiver
REM 用法: tools\build.bat
REM 退出码 0 = 成功

setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo [错误] 未找到 Visual Studio 安装
    exit /b 2
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [错误] vcvars64.bat 调用失败
    exit /b 3
)

cd /d "%~dp0\.."

if not exist out\CMakeCache.txt (
    echo [配置] 首次配置...
    cmake --preset vs
    if errorlevel 1 (
        echo [错误] cmake 配置失败
        exit /b 4
    )
)

echo [编译] 开始编译 Release...
cmake --build out --config Release
if errorlevel 1 (
    echo [错误] 编译失败
    exit /b 1
)

echo [成功] 编译完成: out\bin\Release\HandlerReceiver.exe
exit /b 0
