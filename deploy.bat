@echo off
rem Copy the freshly built stack into a client folder.
rem Usage: deploy.bat "F:\path\to\_classic_era_"
setlocal
if "%~1"=="" (
  echo Usage: deploy.bat "path\to\_classic_era_"
  echo Example: deploy.bat "C:\Games\RavenCraft\_classic_era_"
  exit /b 1
)
set "DEST=%~1"
set "SRC=%~dp0build\out"

if not exist "%DEST%\WowClassic.exe" (
  echo Destination does not contain WowClassic.exe: %DEST%
  exit /b 1
)
if not exist "%SRC%\version.dll" (
  echo Build first: build.bat
  exit /b 1
)

copy /Y "%SRC%\version.dll"  "%DEST%\" >nul
copy /Y "%SRC%\SlotPrep.dll" "%DEST%\" >nul
copy /Y "%SRC%\SuperWoW.dll" "%DEST%\" >nul
copy /Y "%SRC%\HearthDB.dll" "%DEST%\" >nul

if not exist "%DEST%\Interface\AddOns" mkdir "%DEST%\Interface\AddOns"
xcopy /E /I /Y "%~dp0addons\!RavenEraAPI" "%DEST%\Interface\AddOns\!RavenEraAPI" >nul
xcopy /E /I /Y "%~dp0addons\HearthDB"    "%DEST%\Interface\AddOns\HearthDB" >nul

echo Deployed to %DEST%
echo Restart the client (DLLs load only on process start).
endlocal
