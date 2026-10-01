@echo off
setlocal
cd /d "%~dp0"
set LWVK_EXPERIMENTAL_COOP=0
set LWVK_EXPERIMENTAL_COOP_DET_ONLY=0
set LWVK_EXPERIMENTAL_TILE64=0
set LWVK_GPU_PROFILE=0
set LWVK_GPU_DET_PREPROCESS=0
set LWVK_GPU_TEXT_PREPROCESS=0
echo lw.PPOCR.Vulkan - C# WinForms Demo
echo Author: TianTianDaiMaMaTianTian / QQ: 819069052
echo Select your GPU and model in the window, then Initialize and Run OCR.
if not exist "lw.PPOCR.Vulkan.WinFormsDemo.exe" (
 echo Missing WinForms EXE. Extract the entire ZIP before running.
 pause
 exit /b 1
)
start "" "%~dp0lw.PPOCR.Vulkan.WinFormsDemo.exe"
