param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipBuild
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
    'MoviePlayerSubtitleWorker.exe',
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

$copyMap = @{
    'install_ai_models.cmd' = 'install_ai_models.cmd'
    'install_japanese_translation_model.cmd' = 'install_japanese_translation_model.cmd'
    'README_DEPLOY.md' = 'README.md'
    'LICENSE' = 'licenses\MoviePlayer-LICENSE.txt'
    'THIRD_PARTY_NOTICES.md' = 'THIRD_PARTY_NOTICES.md'
    'scripts\setup_whisper.ps1' = 'scripts\setup_whisper.ps1'
    'scripts\setup_japanese_translation_model.ps1' = 'scripts\setup_japanese_translation_model.ps1'
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

$languageSource = Join-Path $Root 'languages'
if (-not (Test-Path -LiteralPath $languageSource -PathType Container)) {
    throw "Language resource directory was not found: $languageSource"
}
Copy-Item -LiteralPath $languageSource -Destination (Join-Path $Staging 'languages') -Recurse

$existingAiModels = Join-Path $Deploy 'third_party\whisper\models'
if (Test-Path -LiteralPath $existingAiModels) {
    $preservedDestination = Join-Path $Staging 'third_party\whisper\models'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $preservedDestination) | Out-Null
    Move-Item -LiteralPath $existingAiModels -Destination $preservedDestination
}

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
