@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if not defined VSPATH if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VSPATH (
  echo Visual Studio with the C++ toolset not found
  exit /b 1
)
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
if errorlevel 1 (
  echo VsDevCmd failed
  exit /b 1
)

cd /d "%~dp0"
if not exist build mkdir build
cd build
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ..
if errorlevel 1 exit /b 1
nmake
if errorlevel 1 exit /b 1
echo.
echo Built to build\out\
dir /b out
endlocal
