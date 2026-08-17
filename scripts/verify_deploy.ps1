param(
    [Parameter(Mandatory = $true)]
    [string]$DeployDirectory,
    [switch]$AllowMissingFrameInterpolation
)

$ErrorActionPreference = 'Stop'
$DeployDirectory = (Resolve-Path -LiteralPath $DeployDirectory).Path.TrimEnd('\')

$requiredFiles = @(
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
    'vcruntime140_1.dll',
    'install_ai_models.cmd',
    'install_japanese_translation_model.cmd',
    'verify_portable.cmd',
    'README.md',
    'THIRD_PARTY_NOTICES.md',
    'scripts\setup_whisper.ps1',
    'scripts\setup_japanese_translation_model.ps1',
    'scripts\verify_deploy.ps1',
    'licenses\AI-RUNTIME-AND-MODELS.md',
    'licenses\MoviePlayer-LICENSE.txt',
    'licenses\NVIDIA-RTX-Video-SDK-License.pdf'
)
foreach ($relativePath in $requiredFiles) {
    $path = Join-Path $DeployDirectory $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required portable file is missing: $relativePath"
    }
    if ((Get-Item -LiteralPath $path).Length -eq 0) {
        throw "Required portable file is empty: $relativePath"
    }
}

$capabilityFile = Join-Path $DeployDirectory 'MoviePlayer.capabilities.ini'
$capabilities = Get-Content -LiteralPath $capabilityFile
$frameInterpolationEnabled =
    $capabilities -contains 'NvidiaFrameInterpolation=1'
$frameInterpolationDisabled =
    $capabilities -contains 'NvidiaFrameInterpolation=0'
if (-not $frameInterpolationEnabled -and -not $frameInterpolationDisabled) {
    throw 'MoviePlayer.capabilities.ini does not declare frame interpolation support.'
}
if (-not $AllowMissingFrameInterpolation -and -not $frameInterpolationEnabled) {
    throw 'This MoviePlayer build does not contain NVIDIA 2x frame interpolation support.'
}

$frucPath = Join-Path $DeployDirectory 'NvOFFRUC.dll'
$cudaRuntimes = @(Get-ChildItem -LiteralPath $DeployDirectory -File `
    -Filter 'cudart64_*.dll' -ErrorAction SilentlyContinue)
if ($frameInterpolationEnabled) {
    if (-not (Test-Path -LiteralPath $frucPath -PathType Leaf)) {
        throw 'Frame interpolation is enabled, but NvOFFRUC.dll is missing.'
    }
    if ($cudaRuntimes.Count -eq 0) {
        throw 'Frame interpolation is enabled, but its cudart64 runtime is missing.'
    }
} elseif ((Test-Path -LiteralPath $frucPath) -or $cudaRuntimes.Count -gt 0) {
    throw 'Stale NVIDIA frame interpolation DLLs were found beside a build without FRUC support.'
}

$expectedLanguages = @(
    'ar.lang', 'de.lang', 'en.lang', 'es.lang', 'fr.lang', 'hi.lang',
    'id.lang', 'ja.lang', 'ko.lang', 'pt.lang', 'zh-CN.lang', 'zh-TW.lang'
)
foreach ($language in $expectedLanguages) {
    $path = Join-Path $DeployDirectory "languages\$language"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Portable language catalog is missing: $language"
    }
}

$buildOnlyFile = Get-ChildItem -LiteralPath $DeployDirectory -Recurse -File |
    Where-Object { $_.Extension -in @('.exp', '.ilk', '.lib', '.obj', '.pdb') } |
    Select-Object -First 1
if ($buildOnlyFile) {
    throw "A build-only file was included in the portable directory: $($buildOnlyFile.FullName)"
}

$modelRoot = Join-Path $DeployDirectory 'third_party\whisper\models'
if (Test-Path -LiteralPath $modelRoot -PathType Container) {
    $standardModels = @(
        @{ Path = 'ggml-large-v3-turbo.bin'; Size = 1624555275L },
        @{ Path = 'translation-m2m100\config.json'; Size = 223L },
        @{ Path = 'translation-m2m100\model.bin'; Size = 490667752L },
        @{ Path = 'translation-m2m100\sentencepiece.bpe.model'; Size = 2423393L },
        @{ Path = 'translation-m2m100\shared_vocabulary.json'; Size = 2796509L }
    )
    foreach ($model in $standardModels) {
        $path = Join-Path $modelRoot $model.Path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Item -LiteralPath $path).Length -ne $model.Size) {
            throw "An incomplete standard AI model was found: $($model.Path)"
        }
    }
}

Write-Host ("Verified portable deployment: RTX VSR=yes, NVIDIA 2x frame interpolation={0}, setup.exe=yes, languages={1}" -f `
    $(if ($frameInterpolationEnabled) { 'yes' } else { 'no' }),
    $expectedLanguages.Count)
