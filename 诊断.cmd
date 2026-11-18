@echo off
chcp 65001 >nul
cd /d "%~dp0"
if not exist kwifi.exe (
  echo kwifi.exe not found next to this script.
  pause
  exit /b 1
)
echo Running kwifi self test...
kwifi.exe --selftest
echo.
echo Report saved to %%LOCALAPPDATA%%\Kwifi\data\selftest.txt
echo (or next to the exe if that folder is not writable)
pause
