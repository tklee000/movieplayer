param(
    [string]$Version = '0.5',
    [switch]$SkipBuild,
    [string]$DeployDirectory
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
$Artifacts = Join-Path $Root 'artifacts'
$Deploy = Join-Path $Root 'deploy'
if (-not [string]::IsNullOrWhiteSpace($DeployDirectory)) {
    $Deploy = [IO.Path]::GetFullPath((Join-Path $Root $DeployDirectory))
    if (-not $Deploy.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to package a directory outside the project: $Deploy"
    }
}

if ($Version -ne '0.5') {
    throw "This source tree declares version 0.5; refusing to package version $Version."
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

New-Item -ItemType Directory -Force -Path $Artifacts | Out-Null
$Archive = Join-Path $Artifacts "MoviePlayer-v$Version-win64.zip"
if (Test-Path -LiteralPath $Archive) {
    Remove-Item -LiteralPath $Archive -Force
}
Compress-Archive -Path (Join-Path $Deploy '*') -DestinationPath $Archive -CompressionLevel Optimal

$Hash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
$Checksums = Join-Path $Artifacts 'SHA256SUMS.txt'
[IO.File]::WriteAllText(
    $Checksums,
    "$Hash  $([IO.Path]::GetFileName($Archive))`n",
    [Text.UTF8Encoding]::new($false))

Write-Host "Release archive: $Archive"
Write-Host "SHA-256: $Hash"
