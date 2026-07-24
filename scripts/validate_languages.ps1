param(
    [string]$LanguageDirectory = ''
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $LanguageDirectory) {
    $LanguageDirectory = Join-Path $Root 'languages'
}
$LanguageDirectory = (Resolve-Path $LanguageDirectory).Path
$ExpectedFiles = @(
    'en.lang', 'ja.lang', 'ko.lang', 'fr.lang', 'de.lang', 'zh-CN.lang', 'zh-TW.lang',
    'es.lang', 'pt.lang', 'hi.lang', 'id.lang', 'ar.lang'
)

function Read-LanguageFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    $values = @{}
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
        $lineNumber++
        if ($line -match '^\s*(#|;|$)') {
            continue
        }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) {
            throw "Invalid language entry at ${Path}:$lineNumber"
        }
        $key = $line.Substring(0, $separator).Trim()
        if ($values.ContainsKey($key)) {
            throw "Duplicate language key '$key' in $Path"
        }
        $values[$key] = $line.Substring($separator + 1)
    }
    return $values
}

$catalogs = @{}
foreach ($name in $ExpectedFiles) {
    $path = Join-Path $LanguageDirectory $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required language file was not found: $path"
    }
    $catalogs[$name] = Read-LanguageFile $path
}

$english = $catalogs['en.lang']
foreach ($name in $ExpectedFiles) {
    $catalog = $catalogs[$name]
    $missing = @($english.Keys | Where-Object { -not $catalog.ContainsKey($_) })
    $extra = @($catalog.Keys | Where-Object { -not $english.ContainsKey($_) })
    if ($missing.Count -or $extra.Count) {
        throw "$name key mismatch. Missing: $($missing -join ', '); extra: $($extra -join ', ')"
    }

    foreach ($key in $english.Keys) {
        $englishTokens = @([regex]::Matches($english[$key], '\{[a-z]+\}') |
            ForEach-Object Value | Sort-Object -Unique)
        $translatedTokens = @([regex]::Matches($catalog[$key], '\{[a-z]+\}') |
            ForEach-Object Value | Sort-Object -Unique)
        if (($englishTokens -join ',') -ne ($translatedTokens -join ',')) {
            throw "$name placeholder mismatch for '$key'"
        }
    }
    Write-Host ("Validated {0}: {1} keys" -f $name, $catalog.Count)
}

Write-Host 'All MoviePlayer language resources are valid.'
