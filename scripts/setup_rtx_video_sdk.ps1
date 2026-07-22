param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
$ThirdParty = Join-Path $Root 'third_party'
$Destination = Join-Path $ThirdParty 'rtx_video_sdk'
$Work = Join-Path $Root '.rtx-video-sdk-download'
$Archive = Join-Path $Work 'RTX_Video_SDK_v1.1.0.zip'
$Extracted = Join-Path $Work 'extracted'
$DownloadUrl = 'https://catalog.ngc.nvidia.com/orgs/nvidia/multimedia/models/dlpp/1.5/download-file?path=RTX_Video_SDK_v1.1.0.zip'
$ExpectedSha256 = 'ABF4F34E2B5A618E355B0D5A0365D8ECC3DB4396E756E4C850A867E1AE2ED69E'

$RequiredFiles = @(
    'include\nvsdk_ngx_helpers_vsr.h',
    'lib\Windows\x64\nvsdk_ngx_d.lib',
    'lib\Windows\x64\nvsdk_ngx_d_dbg.lib',
    'bin\Windows\x64\rel\nvngx_vsr.dll',
    'NVIDIA_RTX_Video_SDK_License.pdf'
)

function Test-CompleteSdk {
    param([Parameter(Mandatory = $true)][string]$Path)
    foreach ($relative in $RequiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relative) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

function Assert-WorkspaceChild {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if (-not $full.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the project: $full"
    }
    return $full
}

if (Test-CompleteSdk $Destination) {
    if (-not $Force) {
        Write-Host "RTX Video SDK 1.1 is already installed: $Destination"
        exit 0
    }
    $Destination = Assert-WorkspaceChild $Destination
    Remove-Item -LiteralPath $Destination -Recurse -Force
} elseif (Test-Path -LiteralPath $Destination) {
    throw "An incomplete RTX Video SDK directory already exists: $Destination"
}

$Work = Assert-WorkspaceChild $Work
if (Test-Path -LiteralPath $Work) {
    Remove-Item -LiteralPath $Work -Recurse -Force
}
New-Item -ItemType Directory -Path $Work | Out-Null
New-Item -ItemType Directory -Path $Extracted | Out-Null

try {
    Write-Host 'Downloading NVIDIA RTX Video SDK 1.1 from the public NGC catalog...'
    & curl.exe -L --fail --retry 3 --output $Archive $DownloadUrl
    if ($LASTEXITCODE -ne 0) {
        throw "RTX Video SDK download failed with exit code $LASTEXITCODE"
    }

    $actualHash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash
    if ($actualHash -ne $ExpectedSha256) {
        throw "RTX Video SDK SHA-256 mismatch. Expected $ExpectedSha256, got $actualHash"
    }

    Expand-Archive -LiteralPath $Archive -DestinationPath $Extracted
    if (-not (Test-CompleteSdk $Extracted)) {
        throw 'The downloaded RTX Video SDK archive is incomplete.'
    }

    New-Item -ItemType Directory -Path $ThirdParty -Force | Out-Null
    Move-Item -LiteralPath $Extracted -Destination $Destination
    Write-Host "RTX Video SDK 1.1 installed: $Destination"
}
finally {
    if (Test-Path -LiteralPath $Work) {
        Remove-Item -LiteralPath $Work -Recurse -Force
    }
}
