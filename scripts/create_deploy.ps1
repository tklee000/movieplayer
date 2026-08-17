param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipBuild,
    [switch]$AllowMissingFrameInterpolation
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
$Deploy = Join-Path $Root 'deploy'
$Staging = Join-Path $Root '.deploy-staging'

function Assert-WorkspaceChild {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if (-not $full.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the project: $full"
    }
    return $full
}

function Test-FrameInterpolationSdk {
    $sdk = Join-Path $Root 'third_party\nvidia_optical_flow_sdk'
    return (Test-Path -LiteralPath (Join-Path $sdk 'include\NvOFFRUC.h') -PathType Leaf) -and
           (Test-Path -LiteralPath (Join-Path $sdk 'bin\Windows\x64\NvOFFRUC.dll') -PathType Leaf) -and
           (@(Get-ChildItem -LiteralPath (Join-Path $sdk 'bin\Windows\x64') -File `
               -Filter 'cudart64_*.dll' -ErrorAction SilentlyContinue).Count -gt 0)
}

if (-not $AllowMissingFrameInterpolation -and -not (Test-FrameInterpolationSdk)) {
    $archives = @(Get-ChildItem -LiteralPath $Root -File `
        -Filter 'Optical_Flow_SDK_*.zip')
    if ($archives.Count -eq 1) {
        Write-Host 'Preparing the locally supplied NVIDIA Optical Flow SDK...'
        & (Join-Path $Root 'scripts\setup_nvidia_optical_flow_sdk.ps1') `
            -ArchivePath $archives[0].FullName
        if ($LASTEXITCODE -ne 0) {
            throw "NVIDIA Optical Flow SDK setup failed with exit code $LASTEXITCODE."
        }
    } elseif ($archives.Count -eq 0) {
        throw 'A portable release requires the licensed NVIDIA Optical Flow SDK. Supply Optical_Flow_SDK_*.zip or use -AllowMissingFrameInterpolation explicitly.'
    } else {
        throw 'More than one Optical_Flow_SDK_*.zip archive was found. Keep only the intended SDK archive.'
    }
}

if (-not $SkipBuild) {
    & (Join-Path $Root 'build.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "MoviePlayer build failed with exit code $LASTEXITCODE."
    }
}

$BuildOutput = Join-Path $Root "build-vs2019\$Configuration"
$Executable = Join-Path $BuildOutput 'MoviePlayer.exe'
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "MoviePlayer build output was not found: $Executable"
}
& (Join-Path $Root 'scripts\verify_release.ps1') `
    -Executable $Executable -Configuration $Configuration

$Staging = Assert-WorkspaceChild $Staging
if (Test-Path -LiteralPath $Staging) {
    Remove-Item -LiteralPath $Staging -Recurse -Force
}
New-Item -ItemType Directory -Path $Staging | Out-Null

$runtimeFiles = @(
    'MoviePlayer.exe',
    'setup.exe',
    'MoviePlayerSubtitleWorker.exe',
    'MoviePlayer.capabilities.ini',
    'ctranslate2.dll',
    'nvngx_vsr.dll',
    'concrt140.dll',
    'msvcp140.dll',
    'msvcp140_1.dll',
    'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll',
    'msvcp140_codecvt_ids.dll',
    'vcruntime140.dll',
    'vcruntime140_1.dll'
)
foreach ($name in $runtimeFiles) {
    $source = Join-Path $BuildOutput $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required runtime file was not found: $name"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $Staging $name)
}

$capabilityFile = Join-Path $BuildOutput 'MoviePlayer.capabilities.ini'
$frameInterpolationEnabled =
    (Get-Content -LiteralPath $capabilityFile) -contains 'NvidiaFrameInterpolation=1'
if (-not $AllowMissingFrameInterpolation -and -not $frameInterpolationEnabled) {
    throw 'MoviePlayer.exe was built without NVIDIA 2x frame interpolation. Rebuild after installing the Optical Flow SDK.'
}

$frucRuntime = Join-Path $BuildOutput 'NvOFFRUC.dll'
if ($frameInterpolationEnabled) {
    if (-not (Test-Path -LiteralPath $frucRuntime -PathType Leaf)) {
        throw 'The FRUC-enabled build output is missing NvOFFRUC.dll.'
    }
    Copy-Item -LiteralPath $frucRuntime `
        -Destination (Join-Path $Staging 'NvOFFRUC.dll')
    $cudaRuntimes = @(Get-ChildItem -LiteralPath $BuildOutput -File `
        -Filter 'cudart64_*.dll')
    if ($cudaRuntimes.Count -eq 0) {
        throw 'The FRUC build output is missing its cudart64 runtime.'
    }
    foreach ($cudaRuntime in $cudaRuntimes) {
        Copy-Item -LiteralPath $cudaRuntime.FullName `
            -Destination (Join-Path $Staging $cudaRuntime.Name)
    }
}

$copyMap = @{
    'install_ai_models.cmd' = 'install_ai_models.cmd'
    'install_japanese_translation_model.cmd' = 'install_japanese_translation_model.cmd'
    'verify_portable.cmd' = 'verify_portable.cmd'
    'README_DEPLOY.md' = 'README.md'
    'LICENSE' = 'licenses\MoviePlayer-LICENSE.txt'
    'THIRD_PARTY_NOTICES.md' = 'THIRD_PARTY_NOTICES.md'
    'scripts\setup_whisper.ps1' = 'scripts\setup_whisper.ps1'
    'scripts\setup_japanese_translation_model.ps1' = 'scripts\setup_japanese_translation_model.ps1'
    'scripts\verify_deploy.ps1' = 'scripts\verify_deploy.ps1'
    'tools\whisper\README.md' = 'tools\whisper\README.md'
    'third_party\rtx_video_sdk\NVIDIA_RTX_Video_SDK_License.pdf' = 'licenses\NVIDIA-RTX-Video-SDK-License.pdf'
    'third_party\whisper\LICENSES.md' = 'licenses\AI-RUNTIME-AND-MODELS.md'
    'third_party\whisper_cpp\LICENSE' = 'licenses\whisper.cpp-LICENSE.txt'
    'third_party\ctranslate2\LICENSE' = 'licenses\CTranslate2-LICENSE.txt'
    'third_party\ctranslate2\third_party\ruy\LICENSE' = 'licenses\ruy-LICENSE.txt'
    'third_party\ctranslate2\third_party\ruy\third_party\cpuinfo\LICENSE' = 'licenses\cpuinfo-LICENSE.txt'
    'third_party\ctranslate2\third_party\ruy\third_party\cpuinfo\deps\clog\LICENSE' = 'licenses\clog-LICENSE.txt'
    'third_party\ctranslate2\third_party\cpu_features\LICENSE' = 'licenses\cpu_features-LICENSE.txt'
    'third_party\ctranslate2\third_party\spdlog\LICENSE' = 'licenses\spdlog-LICENSE.txt'
    'licenses\BS-thread-pool-LICENSE.txt' = 'licenses\BS-thread-pool-LICENSE.txt'
    'licenses\avx_mathfun-LICENSE.txt' = 'licenses\avx_mathfun-LICENSE.txt'
    'licenses\SIMD-Utils-LICENSE.txt' = 'licenses\SIMD-Utils-LICENSE.txt'
    'licenses\neon_mathfun-LICENSE.txt' = 'licenses\neon_mathfun-LICENSE.txt'
    'third_party\sentencepiece\LICENSE' = 'licenses\SentencePiece-LICENSE.txt'
    'third_party\sentencepiece\third_party\absl\LICENSE' = 'licenses\Abseil-LICENSE.txt'
    'third_party\sentencepiece\third_party\protobuf-lite\LICENSE' = 'licenses\protobuf-lite-LICENSE.txt'
    'third_party\sentencepiece\third_party\darts_clone\LICENSE' = 'licenses\Darts-clone-LICENSE.txt'
    'third_party\sentencepiece\third_party\esaxx\LICENSE' = 'licenses\esaxx-LICENSE.txt'
    'third_party\opus\COPYING' = 'licenses\libopus-LICENSE.txt'
}
foreach ($entry in $copyMap.GetEnumerator()) {
    $source = Join-Path $Root $entry.Key
    $destination = Join-Path $Staging $entry.Value
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Deployment source file was not found: $source"
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
}

if ($frameInterpolationEnabled) {
    $frucLicense = Get-ChildItem -LiteralPath `
        (Join-Path $Root 'third_party\nvidia_optical_flow_sdk') -File `
        -Filter 'NVIDIA_Optical_Flow_SDK_License.*' |
        Select-Object -First 1
    if (-not $frucLicense) {
        throw 'The NVIDIA Optical Flow SDK license is required for a FRUC deployment.'
    }
    Copy-Item -LiteralPath $frucLicense.FullName -Destination `
        (Join-Path $Staging ('licenses\' + $frucLicense.Name))
}

$languageSource = Join-Path $Root 'languages'
if (-not (Test-Path -LiteralPath $languageSource -PathType Container)) {
    throw "Language resource directory was not found: $languageSource"
}
Copy-Item -LiteralPath $languageSource -Destination (Join-Path $Staging 'languages') -Recurse

$verifyArguments = @{
    DeployDirectory = $Staging
}
if ($AllowMissingFrameInterpolation) {
    $verifyArguments.AllowMissingFrameInterpolation = $true
}
& (Join-Path $Root 'scripts\verify_deploy.ps1') @verifyArguments

$Deploy = Assert-WorkspaceChild $Deploy
if (Test-Path -LiteralPath $Deploy) {
    Remove-Item -LiteralPath $Deploy -Recurse -Force
}
Move-Item -LiteralPath $Staging -Destination $Deploy

$totalBytes = (Get-ChildItem -LiteralPath $Deploy -File -Recurse |
    Measure-Object -Property Length -Sum).Sum
Write-Host ''
Write-Host "Deployment ready: $Deploy"
Write-Host ("Package size without optional AI download: {0:N1} MiB" -f ($totalBytes / 1MB))
