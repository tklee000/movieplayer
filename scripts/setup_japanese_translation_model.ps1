param(
    [switch]$Force,
    [switch]$AcceptThirdPartyTerms
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
$Destination = Join-Path $Root 'third_party\whisper\models\translation-ja-ko-native'
$Staging = Join-Path $Root '.japanese-model-staging'

$ModelRepository = 'Hunhee/argos-ko-ja'
$ModelRevision = '15a9f14d22beefcd1cb4d45abc73f293ec2b56a8'
$PackageName = 'translate-ja_ko-1_2_1.argosmodel'
$PackageDirectory = 'translate-ja_ko-1_2_1'
$PackageUrl = "https://huggingface.co/$ModelRepository/resolve/$ModelRevision/${PackageName}?download=true"
$PackageSize = 76969020L
$PackageSha256 = 'B127AC3AEA6F1A4C3628A33740C54E6E793E0B646D925BDB2ED52E3C8F584DA8'

$InstalledFiles = @(
    @{
        Name = 'model.bin'
        Size = 80971353L
        Sha256 = '6BFA74C5B33082A3B87894AFF448EA841D6C4A10E374CFE13546294CA80D8F05'
    },
    @{
        Name = 'shared_vocabulary.txt'
        Size = 564419L
        Sha256 = '13D241D26BA859DBC4F168A0E6C05E7AE75F3EE284DC2926CF5A9A6F4C9C0CB3'
    },
    @{
        Name = 'source.spm'
        Size = 1202706L
        Sha256 = '99E28E51B51389ABAC6F776BD5AD2EA6708749DB157DC12D336F226BA9097502'
    },
    @{
        Name = 'target.spm'
        Size = 1202706L
        Sha256 = '99E28E51B51389ABAC6F776BD5AD2EA6708749DB157DC12D336F226BA9097502'
    },
    @{
        Name = 'config.json'
        Size = 3L
        Sha256 = 'CA3D163BAB055381827226140568F3BEF7EAAC187CEBD76878E0B63E9E442356'
    }
)

if (-not $AcceptThirdPartyTerms) {
    throw @'
The optional Japanese-to-Korean model package is declared CC BY-NC 4.0 by its
publisher. Review the model card, attribution requirements, and non-commercial
terms before installation, then rerun with -AcceptThirdPartyTerms.
'@
}

function Assert-WorkspaceChild {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if (-not $full.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the project: $full"
    }
    return $full
}

function Test-VerifiedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][long]$Size,
        [Parameter(Mandatory = $true)][string]$Sha256
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    if ((Get-Item -LiteralPath $Path).Length -ne $Size) {
        return $false
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Sha256
}

function Test-InstalledModel {
    foreach ($file in $InstalledFiles) {
        if (-not (Test-VerifiedFile -Path (Join-Path $Destination $file.Name) `
                -Size $file.Size -Sha256 $file.Sha256)) {
            return $false
        }
    }
    return $true
}

$Staging = Assert-WorkspaceChild $Staging
$Destination = Assert-WorkspaceChild $Destination

if (-not $Force -and (Test-InstalledModel)) {
    Write-Host 'The optional Japanese-to-Korean model is already installed and verified.'
    Write-Host "Model directory: $Destination"
    return
}

if (Test-Path -LiteralPath $Staging) {
    Remove-Item -LiteralPath $Staging -Recurse -Force
}
New-Item -ItemType Directory -Path $Staging | Out-Null

try {
    $archive = Join-Path $Staging ($PackageName + '.zip')
    Write-Host "Downloading pinned Japanese-to-Korean model package..."
    Write-Host "Source: $ModelRepository revision $ModelRevision"
    & curl.exe -L --fail --retry 3 --retry-delay 2 --output $archive $PackageUrl
    if ($LASTEXITCODE -ne 0) {
        throw "Download failed: $PackageUrl"
    }

    if (-not (Test-VerifiedFile -Path $archive -Size $PackageSize -Sha256 $PackageSha256)) {
        $actualSize = (Get-Item -LiteralPath $archive).Length
        $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        throw @"
Downloaded model package verification failed.
Expected: $PackageSize bytes, SHA-256 $PackageSha256
Received: $actualSize bytes, SHA-256 $actualHash
"@
    }

    $extracted = Join-Path $Staging 'extracted'
    New-Item -ItemType Directory -Path $extracted | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $extracted)

    $packageRoot = Join-Path $extracted $PackageDirectory
    $modelRoot = Join-Path $packageRoot 'model'
    $prepared = Join-Path $Staging 'prepared'
    New-Item -ItemType Directory -Path $prepared | Out-Null

    Copy-Item -LiteralPath (Join-Path $modelRoot 'model.bin') `
        -Destination (Join-Path $prepared 'model.bin')
    Copy-Item -LiteralPath (Join-Path $modelRoot 'shared_vocabulary.txt') `
        -Destination (Join-Path $prepared 'shared_vocabulary.txt')
    Copy-Item -LiteralPath (Join-Path $packageRoot 'sentencepiece.model') `
        -Destination (Join-Path $prepared 'source.spm')
    Copy-Item -LiteralPath (Join-Path $packageRoot 'sentencepiece.model') `
        -Destination (Join-Path $prepared 'target.spm')
    Copy-Item -LiteralPath (Join-Path $packageRoot 'README.md') `
        -Destination (Join-Path $prepared 'ARGOS_MODEL_README.md')
    Copy-Item -LiteralPath (Join-Path $packageRoot 'metadata.json') `
        -Destination (Join-Path $prepared 'ARGOS_MODEL_METADATA.json')

    [IO.File]::WriteAllText(
        (Join-Path $prepared 'config.json'), "{}`n",
        [Text.UTF8Encoding]::new($false))

    $sourceNotice = @"
MoviePlayer optional Japanese-to-Korean translation model

Repository: https://huggingface.co/$ModelRepository
Pinned revision: $ModelRevision
Package: $PackageName
Package SHA-256: $PackageSha256
License declared by publisher: CC BY-NC 4.0
License: https://creativecommons.org/licenses/by-nc/4.0/

The package metadata credits OPUS, Wiktionary/Wiktextract, and Stanza.
Review the publisher's model card and all applicable upstream terms before use.
"@
    [IO.File]::WriteAllText(
        (Join-Path $prepared 'MODEL_SOURCE.txt'), $sourceNotice,
        [Text.UTF8Encoding]::new($false))

    foreach ($file in $InstalledFiles) {
        $path = Join-Path $prepared $file.Name
        if (-not (Test-VerifiedFile -Path $path -Size $file.Size -Sha256 $file.Sha256)) {
            throw "Extracted model file verification failed: $path"
        }
    }

    $hashes = foreach ($name in @(
            'model.bin', 'shared_vocabulary.txt', 'source.spm', 'target.spm',
            'config.json', 'ARGOS_MODEL_README.md',
            'ARGOS_MODEL_METADATA.json', 'MODEL_SOURCE.txt')) {
        $path = Join-Path $prepared $name
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $name"
    }
    [IO.File]::WriteAllLines(
        (Join-Path $prepared 'SHA256SUMS.txt'), $hashes,
        [Text.UTF8Encoding]::new($false))

    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    Move-Item -LiteralPath $prepared -Destination $Destination
} finally {
    if (Test-Path -LiteralPath $Staging) {
        Remove-Item -LiteralPath $Staging -Recurse -Force
    }
}

Write-Host ''
Write-Host "Optional native Japanese-to-Korean model installed: $Destination"
Write-Host 'MoviePlayer will prefer it for Japanese speech and fall back to M2M100 if loading or inference fails.'
