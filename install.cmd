@echo off
rem Double-click entry point for the installer.
rem
rem It only forwards to tools\install.ps1 with the execution policy relaxed for
rem this one process, then pauses so the output stays readable. The installer
rem itself asks for administrator rights and for the install location.
rem
rem Extra arguments are forwarded, so `install.cmd -DryRun` prints the plan
rem without touching the system.

setlocal
cd /d "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install.ps1" -Action Install %*
set "code=%ERRORLEVEL%"

echo.
if not "%code%"=="0" (
  echo Installer exited with code %code%.
) else (
  echo Done.
)
pause
exit /b %code%
