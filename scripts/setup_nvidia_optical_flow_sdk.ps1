param(
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Destination = Join-Path $Root 'third_party\nvidia_optical_flow_sdk'
$Staging = Join-Path $Root '.nvidia-optical-flow-sdk-staging'

function Assert-WorkspaceChild {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if (-not $full.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the project: $full"
    }
    return $full
}

function Test-CompleteSdk {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Test-Path -LiteralPath (Join-Path $Path 'include\NvOFFRUC.h') -PathType Leaf) -and
           (Test-Path -LiteralPath (Join-Path $Path 'bin\Windows\x64\NvOFFRUC.dll') -PathType Leaf) -and
           (@(Get-ChildItem -LiteralPath (Join-Path $Path 'bin\Windows\x64') `
                 -Filter 'cudart64_*.dll' -File -ErrorAction SilentlyContinue).Count -gt 0)
}

$ArchivePath = (Resolve-Path -LiteralPath $ArchivePath).Path
if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf) -or
    [IO.Path]::GetExtension($ArchivePath) -ne '.zip') {
    throw "The NVIDIA Optical Flow SDK archive must be an existing ZIP file: $ArchivePath"
}

if (Test-CompleteSdk $Destination) {
    if (-not $Force) {
        Write-Host "NVIDIA Optical Flow SDK FRUC is already installed: $Destination"
        exit 0
    }
    $Destination = Assert-WorkspaceChild $Destination
    Get-ChildItem -LiteralPath $Destination -Force |
        Where-Object { $_.Name -ne '.gitkeep' } |
        Remove-Item -Recurse -Force
} elseif (Test-Path -LiteralPath $Destination) {
    $existingFiles = @(Get-ChildItem -LiteralPath $Destination -Recurse -File `
        -Force | Where-Object { $_.Name -ne '.gitkeep' })
    if ($existingFiles.Count -gt 0) {
        throw "An incomplete NVIDIA Optical Flow SDK directory exists: $Destination"
    }
}

$Staging = Assert-WorkspaceChild $Staging
if (Test-Path -LiteralPath $Staging) {
    Remove-Item -LiteralPath $Staging -Recurse -Force
}
New-Item -ItemType Directory -Path $Staging | Out-Null

try {
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $Staging

    $header = Get-ChildItem -LiteralPath $Staging -Recurse -File `
        -Filter 'NvOFFRUC.h' | Select-Object -First 1
    $runtime = Get-ChildItem -LiteralPath $Staging -Recurse -File `
        -Filter 'NvOFFRUC.dll' |
        Where-Object { $_.FullName -match '(?i)\\win64\\' } |
        Select-Object -First 1
    $cudaRuntimes = @(Get-ChildItem -LiteralPath $Staging -Recurse -File `
        -Filter 'cudart64_*.dll' |
        Where-Object { $_.FullName -match '(?i)\\win64\\' })

    if (-not $header -or -not $runtime -or $cudaRuntimes.Count -eq 0) {
        throw 'The archive does not contain NvOFFRUC.h, win64\NvOFFRUC.dll, and a win64 cudart64 runtime.'
    }

    $include = Join-Path $Destination 'include'
    $bin = Join-Path $Destination 'bin\Windows\x64'
    New-Item -ItemType Directory -Path $include -Force | Out-Null
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    Copy-Item -LiteralPath $header.FullName `
        -Destination (Join-Path $include 'NvOFFRUC.h')
    Copy-Item -LiteralPath $runtime.FullName `
        -Destination (Join-Path $bin 'NvOFFRUC.dll')
    foreach ($cudaRuntime in $cudaRuntimes) {
        Copy-Item -LiteralPath $cudaRuntime.FullName `
            -Destination (Join-Path $bin $cudaRuntime.Name)
    }

    $license = Get-ChildItem -LiteralPath $Staging -Recurse -File |
        Where-Object {
            $_.Name -match '(?i)(license|eula)' -and
            $_.Extension -match '(?i)^\.(pdf|txt)$' -and
            $_.Name -notmatch '^\._' -and
            $_.FullName -notmatch '(?i)(FreeImage|External|\\__MACOSX\\)'
        } | Sort-Object @{
            Expression = {
                if ($_.Name -match '(?i)(NVIDIA|DesignWorks|SDK)') { 0 } else { 1 }
            }
        }, FullName | Select-Object -First 1
    if ($license) {
        Copy-Item -LiteralPath $license.FullName -Destination `
            (Join-Path $Destination ("NVIDIA_Optical_Flow_SDK_License" +
                                     $license.Extension.ToLowerInvariant()))
    }

    if (-not (Test-CompleteSdk $Destination)) {
        throw 'The normalized NVIDIA Optical Flow SDK installation is incomplete.'
    }
    Write-Host "NVIDIA Optical Flow SDK FRUC installed: $Destination"
}
finally {
    if (Test-Path -LiteralPath $Staging) {
        Remove-Item -LiteralPath $Staging -Recurse -Force
    }
}
