param([string]$Root = ".ci/vulkan-sdk-1.4.350.0")
$ErrorActionPreference = 'Stop'
$version = '1.4.350.0'
$expected = '855b27ba05d2d8119c5114c5d4ff870ca38f2c632b11e1bb9923b9b7e6ecfe7b'
$rootPath = [IO.Path]::GetFullPath($Root)
$download = Join-Path $PSScriptRoot '../.ci/downloads'
New-Item -ItemType Directory -Force -Path $download | Out-Null
$installer = Join-Path $download "vulkansdk-windows-X64-$version.exe"
if (-not (Test-Path -LiteralPath $installer)) {
    Invoke-WebRequest "https://sdk.lunarg.com/sdk/download/$version/windows/vulkansdk-windows-X64-$version.exe" -OutFile $installer
}
if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'Vulkan SDK installer SHA-256 mismatch; do not install unverified content'
}
# Full CI installation includes the Vulkan loader. Caching only copied SDK files
# would not preserve runner registry/runtime integration. The download is cached.
$arguments = @('--root', ('"' + $rootPath + '"'), '--accept-licenses', '--default-answer', '--confirm-command', 'install')
$process = Start-Process -FilePath $installer -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
if ($process.ExitCode -ne 0) { throw "SDK installer exited $($process.ExitCode)" }
if (-not (Test-Path -LiteralPath "$rootPath/Bin/glslc.exe")) { throw 'SDK installation incomplete' }
"VULKAN_SDK=$rootPath" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
"$rootPath/Bin" | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
