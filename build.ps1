param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Split-Path -Parent $MyInvocation.MyCommand.Path)).Path
$BuildDirectory = Join-Path $Root 'build-vs2019'
$VsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'

$CMakeCache = Join-Path $BuildDirectory 'CMakeCache.txt'
if (-not $Clean -and (Test-Path -LiteralPath $CMakeCache -PathType Leaf)) {
    $homeEntry = Get-Content -LiteralPath $CMakeCache |
        Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' } |
        Select-Object -First 1
    if ($homeEntry) {
        $cachedRoot = $homeEntry.Substring($homeEntry.IndexOf('=') + 1)
        $cachedRoot = [IO.Path]::GetFullPath($cachedRoot).TrimEnd('\')
        if (-not $cachedRoot.Equals($Root, [StringComparison]::OrdinalIgnoreCase)) {
            Write-Host "CMake cache belongs to another checkout: $cachedRoot"
            Write-Host 'Regenerating the build directory for this checkout.'
            $Clean = $true
        }
    }
}

if (-not (Test-Path (Join-Path $Root 'third_party\rtx_video_sdk\lib\Windows\x64\nvsdk_ngx_d.lib'))) {
    & (Join-Path $Root 'scripts\setup_rtx_video_sdk.ps1')
}
$nativeAiRequired = @(
    'third_party\whisper_cpp\CMakeLists.txt',
    'third_party\ctranslate2\CMakeLists.txt',
    'third_party\sentencepiece\CMakeLists.txt'
)
if ($nativeAiRequired.Where({ -not (Test-Path -LiteralPath (Join-Path $Root $_) -PathType Leaf) }).Count -ne 0) {
    & (Join-Path $Root 'scripts\setup_native_ai_dependencies.ps1')
}

if (-not (Test-Path $VsWhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}
$VisualStudio = & $VsWhere -latest -version '[16.0,17.0)' -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $VisualStudio) {
    throw 'Visual Studio 2019 with the Desktop development with C++ workload was not found.'
}
$CMake = Join-Path $VisualStudio `
    'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $CMake)) {
    throw "The Visual Studio 2019 bundled CMake was not found: $CMake"
}

if ($Clean -and (Test-Path $BuildDirectory)) {
    $ResolvedBuild = (Resolve-Path $BuildDirectory).Path
    if (-not $ResolvedBuild.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a build directory outside the project: $ResolvedBuild"
    }
    Remove-Item -LiteralPath $ResolvedBuild -Recurse -Force
}

& $CMake -S $Root -B $BuildDirectory `
    -G 'Visual Studio 16 2019' -A x64 -T v142
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $CMake --build $BuildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Executable = Join-Path $BuildDirectory "$Configuration\MoviePlayer.exe"
& (Join-Path $Root 'scripts\verify_release.ps1') `
    -Executable $Executable -Configuration $Configuration

Write-Host ''
Write-Host "Build completed: $Executable"
