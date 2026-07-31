@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\create_msi.ps1" %*
exit /b %ERRORLEVEL%
