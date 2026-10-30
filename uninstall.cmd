@echo off
rem Double-click entry point for the uninstaller.
rem Removes files, the MSIX package, the classic menu registration, autostart and
rem the machine-level certificate. Note data under %LOCALAPPDATA%\PaperNotes is
rem kept on purpose.

setlocal
cd /d "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install.ps1" -Action Uninstall
set "code=%ERRORLEVEL%"

echo.
if not "%code%"=="0" (
  echo Uninstaller exited with code %code%.
) else (
  echo Done.
)
pause
exit /b %code%
