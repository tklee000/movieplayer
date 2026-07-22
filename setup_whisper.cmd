@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup_whisper.ps1" %*
exit /b %ERRORLEVEL%
