@echo off
cd /d "%~dp0"
lw-ppocr-vulkan-http-service.exe --config "%~dp0http-service.json" %*
pause
