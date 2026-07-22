@echo off
setlocal
title MoviePlayer Optional Japanese Translation Model Setup
echo ============================================================
echo  MoviePlayer Optional Japanese-to-Korean Model Setup
echo ============================================================
echo.
echo This downloads and installs the pinned Japanese-to-Korean model.
echo No model directory is required.
echo.
echo The publisher declares the model package as CC BY-NC 4.0.
echo Review its model card, attribution, and non-commercial terms first.
echo.
choice /C YN /N /M "Do you accept the model's third-party terms? [Y/N] "
if errorlevel 2 exit /b 2

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup_japanese_translation_model.ps1" -AcceptThirdPartyTerms
set "RESULT=%ERRORLEVEL%"
echo.
if "%RESULT%"=="0" (
    echo Optional Japanese model installation completed successfully.
) else (
    echo Optional Japanese model installation failed with exit code %RESULT%.
)
pause
exit /b %RESULT%
