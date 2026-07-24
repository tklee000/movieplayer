param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$VsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$ExpectedFileVersion = '0.2.0.0'

& (Join-Path $Root 'scripts\validate_languages.ps1')

$version = (Get-Item -LiteralPath $Executable).VersionInfo
if ($version.FileVersion -ne $ExpectedFileVersion -or
    $version.ProductVersion -ne $ExpectedFileVersion) {
    throw "Unexpected executable version. Expected $ExpectedFileVersion, got file=$($version.FileVersion), product=$($version.ProductVersion)"
}

if (-not (Test-Path -LiteralPath $VsWhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}
$VisualStudio = & $VsWhere -latest -version '[16.0,17.0)' -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
$Dumpbin = Get-ChildItem (Join-Path $VisualStudio 'VC\Tools\MSVC') `
    -Filter dumpbin.exe -File -Recurse |
    Where-Object { $_.FullName -match '\\bin\\Hostx64\\x64\\dumpbin\.exe$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $Dumpbin) {
    throw 'The Visual Studio x64 dumpbin.exe tool was not found.'
}

$Worker = Join-Path (Split-Path -Parent $Executable) 'MoviePlayerSubtitleWorker.exe'
if (-not (Test-Path -LiteralPath $Worker -PathType Leaf)) {
    throw "Native subtitle worker was not found: $Worker"
}
$workerVersion = (Get-Item -LiteralPath $Worker).VersionInfo
if ($workerVersion.FileVersion -ne $ExpectedFileVersion -or
    $workerVersion.ProductVersion -ne $ExpectedFileVersion) {
    throw "Unexpected worker version. Expected $ExpectedFileVersion, got file=$($workerVersion.FileVersion), product=$($workerVersion.ProductVersion)"
}
$CTranslate2 = Join-Path (Split-Path -Parent $Executable) 'ctranslate2.dll'
if (-not (Test-Path -LiteralPath $CTranslate2 -PathType Leaf)) {
    throw "Native CTranslate2 runtime was not found: $CTranslate2"
}

foreach ($binary in @($Executable, $Worker, $CTranslate2)) {
    $dependencies = (& $Dumpbin /dependents $binary | Out-String)
    if ($dependencies -match '(?im)^\s*(?:avcodec|avdevice|avfilter|avformat|avutil|postproc|swresample|swscale)(?:-\d+)?\.dll\s*$') {
        throw "An unsupported media runtime dependency was found in this release: $binary"
    }
    if ($Configuration -eq 'Release' -and $dependencies -match '(?im)^\s*ucrtbased\.dll\s*$') {
        throw "The Release binary imports the debug Universal CRT: $binary"
    }
    if ($Configuration -eq 'Release' -and
        $dependencies -match '(?im)^\s*(MSVCP14\d*D|VCRUNTIME14\d*D|CONCRT14\d*D)\.dll\s*$') {
        throw "A Release binary imports a non-redistributable debug MSVC runtime: $binary"
    }
    if ($Configuration -eq 'Release') {
        $runtimeImports = [regex]::Matches(
            $dependencies,
            '(?im)^\s*((?:MSVCP|VCRUNTIME|CONCRT)14\d*(?:_[A-Z0-9_]+)?\.dll)\s*$') |
            ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } |
            Sort-Object -Unique
        foreach ($runtime in $runtimeImports) {
            $localRuntime = Join-Path (Split-Path -Parent $binary) $runtime
            if (-not (Test-Path -LiteralPath $localRuntime -PathType Leaf)) {
                throw "App-local VC142 runtime is missing: $runtime"
            }
        }
    }
}

$releaseDirectory = Split-Path -Parent $Executable
$forbiddenRuntime = Get-ChildItem -LiteralPath $releaseDirectory -File |
    Where-Object {
        $_.Name -match '^(?i:(?:avcodec|avdevice|avfilter|avformat|avutil|postproc|swresample|swscale)(?:-\d+)?\.dll)$'
    } |
    Select-Object -First 1
if ($forbiddenRuntime) {
    throw "An unsupported media runtime file was found in this release: $($forbiddenRuntime.FullName)"
}

Write-Host "Verified MoviePlayer $ExpectedFileVersion ($Configuration): version resources, language catalogs, native AI worker, CTranslate2, app-local MSVC runtime, and media runtime dependencies"
