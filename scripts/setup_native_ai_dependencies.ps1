param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
$ThirdParty = Join-Path $Root 'third_party'

$Packages = @(
    @{
        Name = 'whisper.cpp v1.9.1'
        Repository = 'https://github.com/ggml-org/whisper.cpp.git'
        Commit = 'f049fff95a089aa9969deb009cdd4892b3e74916'
        Destination = Join-Path $ThirdParty 'whisper_cpp'
        Required = 'include\whisper.h'
        Submodules = @()
    },
    @{
        Name = 'CTranslate2 v4.8.1'
        Repository = 'https://github.com/OpenNMT/CTranslate2.git'
        Commit = '0d8bcd362ac75ef860ef161d6f0efad0ae439ff0'
        Destination = Join-Path $ThirdParty 'ctranslate2'
        Required = 'include\ctranslate2\translator.h'
        Submodules = @('third_party/cpu_features', 'third_party/ruy', 'third_party/spdlog')
    },
    @{
        Name = 'SentencePiece v0.2.1'
        Repository = 'https://github.com/google/sentencepiece.git'
        Commit = '31646a467d2051eb904e0b45de3a73e91fe1c1e3'
        Destination = Join-Path $ThirdParty 'sentencepiece'
        Required = 'src\sentencepiece_processor.h'
        Submodules = @()
    }
)

function Assert-WorkspaceChild {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if (-not $full.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the project: $full"
    }
    return $full
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$WorkingDirectory = $Root
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
    if (-not ($registered | Where-Object {
                $_.TrimEnd('/') -eq $normalized
            })) {
        & git.exe config --global --add safe.directory $normalized
        if ($LASTEXITCODE -ne 0) {
            throw "Could not register the downloaded source directory with Git: $normalized"
        }
    }
}

function Install-PinnedRepository {
    param([Parameter(Mandatory = $true)][hashtable]$Package)

    $destination = Assert-WorkspaceChild $Package.Destination
    $required = Join-Path $destination $Package.Required
    $valid = $false
    if (Test-Path -LiteralPath $required -PathType Leaf) {
        $actual = (& git.exe -c core.longpaths=true -c "safe.directory=$destination" `
            -C $destination rev-parse HEAD 2>$null)
        $valid = $LASTEXITCODE -eq 0 -and $actual.Trim() -eq $Package.Commit
        if ($valid) {
            foreach ($submodule in $Package.Submodules) {
                if (-not (Test-Path -LiteralPath (Join-Path $destination "$submodule\CMakeLists.txt") -PathType Leaf)) {
                    $valid = $false
                    break
                }
                if ($submodule -eq 'third_party/ruy' -and
                    -not (Test-Path -LiteralPath (Join-Path $destination "$submodule\third_party\cpuinfo\CMakeLists.txt") -PathType Leaf)) {
                    $valid = $false
                    break
                }
            }
        }
    }
    if (-not $Force -and $valid) {
        Register-GitSafeDirectory $destination
        Write-Host "$($Package.Name) is already verified."
        return
    }

    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Recurse -Force
    }
    Write-Host "Cloning $($Package.Name)..."
    Invoke-Git @('clone', '--filter=blob:none', '--no-checkout', $Package.Repository, $destination)
    Invoke-Git @('checkout', '--detach', $Package.Commit) $destination
    foreach ($submodule in $Package.Submodules) {
        Invoke-Git @('submodule', 'update', '--init', '--depth', '1', '--', $submodule) $destination
        if ($submodule -eq 'third_party/ruy') {
            Invoke-Git @('submodule', 'update', '--init', '--depth', '1', '--', 'third_party/cpuinfo') `
                (Join-Path $destination $submodule)
        }
    }
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file was not installed for $($Package.Name): $required"
    }
    $actual = (& git.exe -c core.longpaths=true -c "safe.directory=$destination" `
        -C $destination rev-parse HEAD).Trim()
    if ($actual -ne $Package.Commit) {
        throw "Commit mismatch for $($Package.Name): expected $($Package.Commit), received $actual"
    }
    Register-GitSafeDirectory $destination
    New-Item -ItemType File -Force -Path (Join-Path $destination '.gitkeep') | Out-Null
    Write-Host "Installed $($Package.Name): $destination"
}

foreach ($package in $Packages) {
    Install-PinnedRepository $package
}

Write-Host 'Native AI build dependencies are ready.'
