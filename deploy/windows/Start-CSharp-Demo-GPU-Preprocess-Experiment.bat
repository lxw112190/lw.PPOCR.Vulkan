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
echo lw.PPOCR.Vulkan - experimental GPU DET + CLS + REC preprocessing
echo Preprocessing: FP64. Networks: FP32. DET limit and REC padding: unchanged.
echo The selected GPU must support shaderFloat64; no automatic fallback.
echo Start-CSharp-Demo.bat: default. GPU-DET launcher: previous DET-only mode.
if not exist "lw.PPOCR.Vulkan.WinFormsDemo.exe" (
 echo Extract the entire ZIP before running.
 pause
 exit /b 1
)
start "" "%~dp0lw.PPOCR.Vulkan.WinFormsDemo.exe"
