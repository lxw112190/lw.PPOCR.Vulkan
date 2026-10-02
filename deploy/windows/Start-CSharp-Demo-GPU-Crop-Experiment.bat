@echo off
setlocal
cd /d "%~dp0"
set LWVK_EXPERIMENTAL_COOP=0
set LWVK_EXPERIMENTAL_COOP_DET_ONLY=0
set LWVK_EXPERIMENTAL_TILE64=0
set LWVK_GPU_PROFILE=0
set LWVK_HOST_PROFILE=0
set LWVK_GPU_DET_PREPROCESS=1
set LWVK_GPU_TEXT_PREPROCESS=1
set LWVK_GPU_CROP_PREPROCESS=1
echo lw.PPOCR.Vulkan - explicitly required GPU preprocessing and crops
echo Upload original once; keep BGR8 crops on GPU for CLS/REC preprocessing.
echo Requires shaderFloat64. Networks: FP32. Normal launcher selects automatically.
if not exist "lw.PPOCR.Vulkan.WinFormsDemo.exe" (
 echo Extract the entire ZIP before running.
 pause
 exit /b 1
)
start "" "%~dp0lw.PPOCR.Vulkan.WinFormsDemo.exe"
