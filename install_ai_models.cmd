@echo off
setlocal
title MoviePlayer AI Subtitle Engine Setup
echo ============================================================
echo  MoviePlayer AI Subtitle Engine Setup
echo ============================================================
echo.
echo This downloads the pinned whisper.cpp and M2M100 model files only.
echo Existing verified model files are reused without downloading.
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup_whisper.ps1"
set "RESULT=%ERRORLEVEL%"
echo.
if "%RESULT%"=="0" (
    echo AI subtitle engine installation completed successfully.
) else (
    echo AI subtitle engine installation failed with exit code %RESULT%.
)
if /I not "%~1"=="--from-app" pause
exit /b %RESULT%
