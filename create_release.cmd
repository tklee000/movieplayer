@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\create_release.ps1" %*
exit /b %ERRORLEVEL%
