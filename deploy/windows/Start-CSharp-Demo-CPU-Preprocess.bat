@echo off
setlocal
cd /d "%~dp0"
set LWVK_EXPERIMENTAL_COOP=0
set LWVK_EXPERIMENTAL_COOP_DET_ONLY=0
set LWVK_EXPERIMENTAL_TILE64=0
set LWVK_GPU_PROFILE=0
set LWVK_HOST_PROFILE=0
set LWVK_GPU_DET_PREPROCESS=0
set LWVK_GPU_TEXT_PREPROCESS=0
set LWVK_GPU_CROP_PREPROCESS=0
echo lw.PPOCR.Vulkan - CPU image preprocessing compatibility mode
echo Neural networks still run on the selected Vulkan GPU, not on CPU.
if not exist "lw.PPOCR.Vulkan.WinFormsDemo.exe" (
 echo Extract the entire ZIP before running.
 pause
 exit /b 1
)
start "" "%~dp0lw.PPOCR.Vulkan.WinFormsDemo.exe"
