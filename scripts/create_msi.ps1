param(
    [string]$Version = '0.6',
    [switch]$SkipBuild,
    [switch]$SkipValidation
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
$Deploy = Join-Path $Root 'deploy'
$Artifacts = Join-Path $Root 'artifacts'
$MsiWork = Join-Path $Root '.msi-staging'
$PackageRoot = Join-Path $MsiWork 'package'
$ObjectRoot = Join-Path $MsiWork 'obj'
$InstallerSource = Join-Path $Root 'installer\MoviePlayer.wxs'
$LicenseRtf = Join-Path $Root 'installer\License.rtf'
$ToolCache = Join-Path $Root '.installer-tools'
$WixZip = Join-Path $ToolCache 'wix314-binaries.zip'
$WixRoot = Join-Path $ToolCache 'wix314'
$WixUrl = 'https://github.com/wixtoolset/wix3/releases/download/wix3141rtm/wix314-binaries.zip'
$WixSize = 41297555L
$WixSha256 = '6AC824E1642D6F7277D0ED7EA09411A508F6116BA6FAE0AA5F2C7DAA2FF43D31'

function Assert-WorkspaceChild {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if (-not $full.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the project: $full"
    }
    return $full
}

function Test-VerifiedWixArchive {
    if (-not (Test-Path -LiteralPath $WixZip -PathType Leaf)) {
        return $false
    }
    if ((Get-Item -LiteralPath $WixZip).Length -ne $WixSize) {
        return $false
    }
    return (Get-FileHash -LiteralPath $WixZip -Algorithm SHA256).Hash -eq $WixSha256
}

if ($Version -ne '0.6') {
    throw "This source tree declares version 0.6; refusing to package version $Version."
}

if (-not $SkipBuild) {
    & (Join-Path $Root 'scripts\create_deploy.ps1') -Configuration Release
    if ($LASTEXITCODE -ne 0) {
        throw "Deployment creation failed with exit code $LASTEXITCODE."
    }
}

$DeployExecutable = Join-Path $Deploy 'MoviePlayer.exe'
if (-not (Test-Path -LiteralPath $DeployExecutable -PathType Leaf)) {
    throw 'The deploy directory is incomplete. Run create_deploy.cmd first.'
}
& (Join-Path $Root 'scripts\verify_release.ps1') `
    -Executable $DeployExecutable -Configuration Release

$ToolCache = Assert-WorkspaceChild $ToolCache
New-Item -ItemType Directory -Force -Path $ToolCache | Out-Null
if (-not (Test-VerifiedWixArchive)) {
    $download = $WixZip + '.download'
    Remove-Item -LiteralPath $download -Force -ErrorAction SilentlyContinue
    Write-Host 'Downloading the pinned WiX Toolset 3.14.1 binaries...'
    & curl.exe -L --fail --retry 3 --retry-delay 2 --output $download $WixUrl
    if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath $download -Force -ErrorAction SilentlyContinue
        throw 'WiX Toolset download failed.'
    }
    if ((Get-Item -LiteralPath $download).Length -ne $WixSize) {
        Remove-Item -LiteralPath $download -Force
        throw 'The WiX Toolset archive size does not match the pinned release.'
    }
    $downloadHash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash
    if ($downloadHash -ne $WixSha256) {
        Remove-Item -LiteralPath $download -Force
        throw "The WiX Toolset archive SHA-256 does not match: $downloadHash"
    }
    Move-Item -LiteralPath $download -Destination $WixZip -Force
}

$Candle = Join-Path $WixRoot 'candle.exe'
$Heat = Join-Path $WixRoot 'heat.exe'
$Light = Join-Path $WixRoot 'light.exe'
$Smoke = Join-Path $WixRoot 'smoke.exe'
if (-not (Test-Path -LiteralPath $Candle -PathType Leaf) -or
    -not (Test-Path -LiteralPath $Heat -PathType Leaf) -or
    -not (Test-Path -LiteralPath $Light -PathType Leaf) -or
    -not (Test-Path -LiteralPath $Smoke -PathType Leaf)) {
    $WixRoot = Assert-WorkspaceChild $WixRoot
    if (Test-Path -LiteralPath $WixRoot) {
        Remove-Item -LiteralPath $WixRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $WixRoot | Out-Null
    Expand-Archive -LiteralPath $WixZip -DestinationPath $WixRoot -Force
}

$MsiWork = Assert-WorkspaceChild $MsiWork
if (Test-Path -LiteralPath $MsiWork) {
    Remove-Item -LiteralPath $MsiWork -Recurse -Force
}
New-Item -ItemType Directory -Path $MsiWork | Out-Null
Copy-Item -LiteralPath $Deploy -Destination $PackageRoot -Recurse
New-Item -ItemType Directory -Path $ObjectRoot | Out-Null

# The MSI installs only the standard Whisper and M2M100 download path. Keep the
# separately licensed Japanese model and all pre-existing model weights out of
# the package so installation always starts from the pinned verified downloads.
$excludedPaths = @(
    (Join-Path $PackageRoot 'install_japanese_translation_model.cmd'),
    (Join-Path $PackageRoot 'scripts\setup_japanese_translation_model.ps1'),
    (Join-Path $PackageRoot 'third_party\whisper\models')
)
foreach ($excluded in $excludedPaths) {
    $resolvedExcluded = [IO.Path]::GetFullPath($excluded)
    if (-not $resolvedExcluded.StartsWith($PackageRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an MSI staging path outside the package: $excluded"
    }
    if (Test-Path -LiteralPath $excluded) {
        Remove-Item -LiteralPath $excluded -Recurse -Force
    }
}

$HarvestedSource = Join-Path $MsiWork 'HarvestedFiles.wxs'
& $Heat dir $PackageRoot -nologo -ag -cg ApplicationFiles -dr INSTALLFOLDER `
    -scom -sfrag -srd -sreg -var var.DeployDir -out $HarvestedSource
if ($LASTEXITCODE -ne 0) {
    throw "WiX file harvesting failed with exit code $LASTEXITCODE."
}

& $Candle -nologo -arch x64 -ext WixUtilExtension `
    "-dDeployDir=$PackageRoot" `
    "-dLicenseRtf=$LicenseRtf" `
    '-dProductVersion=0.6.0' `
    -out ($ObjectRoot + '\') `
    $InstallerSource $HarvestedSource
if ($LASTEXITCODE -ne 0) {
    throw "WiX compilation failed with exit code $LASTEXITCODE."
}

New-Item -ItemType Directory -Force -Path $Artifacts | Out-Null
$Msi = Join-Path $Artifacts "MoviePlayer-v$Version-win64.msi"
Remove-Item -LiteralPath $Msi -Force -ErrorAction SilentlyContinue
$wixObjects = Get-ChildItem -LiteralPath $ObjectRoot -Filter '*.wixobj' -File |
    Select-Object -ExpandProperty FullName
& $Light -nologo -ext WixUIExtension -ext WixUtilExtension -cultures:ko-kr `
    -spdb -out $Msi $wixObjects
if ($LASTEXITCODE -ne 0) {
    throw "MSI linking failed with exit code $LASTEXITCODE."
}

if (-not $SkipValidation) {
    & $Smoke -nologo $Msi
    if ($LASTEXITCODE -ne 0) {
        throw "MSI validation failed with exit code $LASTEXITCODE."
    }
}

$Hash = (Get-FileHash -LiteralPath $Msi -Algorithm SHA256).Hash.ToLowerInvariant()
$Checksums = Join-Path $Artifacts 'SHA256SUMS.txt'
$releaseArtifacts = Get-ChildItem -LiteralPath $Artifacts -File |
    Where-Object {
        $_.BaseName -eq "MoviePlayer-v$Version-win64" -and
        $_.Extension -in @('.msi', '.zip')
    } |
    Sort-Object Name
$checksumLines = foreach ($artifact in $releaseArtifacts) {
    $artifactHash = (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$artifactHash  $($artifact.Name)"
}
[IO.File]::WriteAllText(
    $Checksums,
    (($checksumLines | Sort-Object) -join "`n") + "`n",
    [Text.UTF8Encoding]::new($false))

Write-Host ''
Write-Host "MSI package: $Msi"
Write-Host "SHA-256: $Hash"
Write-Host 'AI model behavior: downloads pinned Whisper and M2M100 models during installation.'
Write-Host 'Excluded: optional Japanese-to-Korean model and installer.'
Write-Host 'File associations: .mp4, .mkv, .avi, .ts, .m2ts, .mts'
