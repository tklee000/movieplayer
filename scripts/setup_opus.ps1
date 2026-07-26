param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
$Destination = Join-Path $ProjectRoot 'third_party\opus'
$Repository = 'https://github.com/xiph/opus.git'
$Commit = 'ddbe48383984d56acd9e1ab6a090c54ca6b735a6'

function Assert-WorkspaceChild {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if (-not $full.StartsWith($ProjectRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the project: $full"
    }
    return $full
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$WorkingDirectory = $ProjectRoot
    )
    & git.exe -c core.longpaths=true -c "safe.directory=$WorkingDirectory" `
        -C $WorkingDirectory @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git failed with exit code ${LASTEXITCODE}: git $($Arguments -join ' ')"
    }
}

function Register-GitSafeDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)
    $normalized = ([IO.Path]::GetFullPath($Path) -replace '\\', '/').TrimEnd('/')
    $registered = @(& git.exe config --global --get-all safe.directory 2>$null)
    if (-not ($registered | Where-Object { $_.TrimEnd('/') -eq $normalized })) {
        & git.exe config --global --add safe.directory $normalized
        if ($LASTEXITCODE -ne 0) {
            throw "Could not register the downloaded source directory: $normalized"
        }
    }
}

$Destination = Assert-WorkspaceChild $Destination
$requiredFiles = @('CMakeLists.txt', 'include\opus.h', 'COPYING')
$valid = -not $Force
foreach ($relativePath in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $Destination $relativePath) -PathType Leaf)) {
        $valid = $false
    }
}
if ($valid) {
    $actual = (& git.exe -c core.longpaths=true -c "safe.directory=$Destination" `
        -C $Destination rev-parse HEAD 2>$null)
    $valid = $LASTEXITCODE -eq 0 -and $actual.Trim() -eq $Commit
}
if ($valid) {
    Register-GitSafeDirectory $Destination
    Write-Host 'libopus 1.5.2 is already verified.'
    return
}

if (Test-Path -LiteralPath $Destination) {
    Remove-Item -LiteralPath $Destination -Recurse -Force
}
Write-Host 'Cloning libopus 1.5.2...'
Invoke-Git @('clone', '--filter=blob:none', '--no-checkout', $Repository, $Destination)
Invoke-Git @('checkout', '--detach', $Commit) $Destination
foreach ($relativePath in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $Destination $relativePath) -PathType Leaf)) {
        throw "Required libopus file was not installed: $relativePath"
    }
}
$actual = (& git.exe -c core.longpaths=true -c "safe.directory=$Destination" `
    -C $Destination rev-parse HEAD).Trim()
if ($actual -ne $Commit) {
    throw "libopus commit mismatch: expected $Commit, received $actual"
}
Register-GitSafeDirectory $Destination
Write-Host "Installed libopus 1.5.2: $Destination"
