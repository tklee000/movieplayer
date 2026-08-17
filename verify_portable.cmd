@echo off
setlocal
set "MOVIEPLAYER_PACKAGE_DIR=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\verify_deploy.ps1" -DeployDirectory "%MOVIEPLAYER_PACKAGE_DIR:~0,-1%" %*
exit /b %ERRORLEVEL%
