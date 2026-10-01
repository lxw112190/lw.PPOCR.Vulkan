@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"
echo Vulkan GPU device and capability check
echo The bundled loader does NOT replace the GPU vendor driver.
"%~dp0lw-ppocr-vulkan-probe.exe"
set RESULT=%ERRORLEVEL%
echo.
echo Exit code: %RESULT%
echo If no device is detected, install/update your NVIDIA, AMD or Intel driver.
pause
exit /b %RESULT%
