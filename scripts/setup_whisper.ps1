param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
$WhisperRoot = Join-Path $Root 'third_party\whisper'
$ModelsRoot = Join-Path $WhisperRoot 'models'
$M2MRoot = Join-Path $ModelsRoot 'translation-m2m100'

$WhisperRevision = '5359861c739e955e79d9a303bcbc70fb988958b1'
$M2MRevision = '18e406c615ef2991fa74d53734bf66b0a6b10cb4'

$Models = @(
    @{
        Name = 'whisper.cpp large-v3-turbo GGML model'
        Url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/$WhisperRevision/ggml-large-v3-turbo.bin?download=true"
        Path = Join-Path $ModelsRoot 'ggml-large-v3-turbo.bin'
        Size = 1624555275L
        Sha256 = '1FC70F774D38EB169993AC391EEA357EF47C88757EF72EE5943879B7E8E2BC69'
    },
    @{
        Name = 'M2M100 CTranslate2 config'
        Url = "https://huggingface.co/gn64/M2M100_418M_CTranslate2/resolve/$M2MRevision/config.json?download=true"
        Path = Join-Path $M2MRoot 'config.json'
        Size = 223L
        Sha256 = '8F6496ADFC930CBFECBE8281112197705C488FAB47D34B4829B06D7F478909AF'
    },
    @{
        Name = 'M2M100 CTranslate2 int8 model'
        Url = "https://huggingface.co/gn64/M2M100_418M_CTranslate2/resolve/$M2MRevision/model.bin?download=true"
        Path = Join-Path $M2MRoot 'model.bin'
        Size = 490667752L
        Sha256 = 'A1826980FC5C037E69C7AC94FCB56C03001A66F380EB71863CC0A3879E71421B'
    },
    @{
        Name = 'M2M100 SentencePiece model'
        Url = "https://huggingface.co/gn64/M2M100_418M_CTranslate2/resolve/$M2MRevision/sentencepiece.bpe.model?download=true"
        Path = Join-Path $M2MRoot 'sentencepiece.bpe.model'
        Size = 2423393L
        Sha256 = 'D8F7C76ED2A5E0822BE39F0A4F95A55EB19C78F4593CE609E2EDBC2AEA4D380A'
    },
    @{
        Name = 'M2M100 shared vocabulary'
        Url = "https://huggingface.co/gn64/M2M100_418M_CTranslate2/resolve/$M2MRevision/shared_vocabulary.json?download=true"
        Path = Join-Path $M2MRoot 'shared_vocabulary.json'
        Size = 2796509L
        Sha256 = '7EB5D0FF184C6095C7C10F9911C0AEA492250ABD12854F9C3D787C64B1C6397E'
    }
)

function Test-VerifiedModel {
    param([Parameter(Mandatory = $true)][hashtable]$Model)

    if (-not (Test-Path -LiteralPath $Model.Path -PathType Leaf)) {
        return $false
    }
    if ((Get-Item -LiteralPath $Model.Path).Length -ne $Model.Size) {
        return $false
    }
    return (Get-FileHash -LiteralPath $Model.Path -Algorithm SHA256).Hash -eq $Model.Sha256
}

function Install-VerifiedModel {
    param([Parameter(Mandatory = $true)][hashtable]$Model)

    if (-not $Force -and (Test-VerifiedModel -Model $Model)) {
        Write-Host "$($Model.Name) is already verified."
        return
    }

    $directory = Split-Path -Parent $Model.Path
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $temporary = $Model.Path + '.download'
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    Write-Host "Downloading $($Model.Name)..."
    & curl.exe -L --fail --retry 3 --retry-delay 2 --output $temporary $Model.Url
    if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        throw "Download failed: $($Model.Url)"
    }
    $item = Get-Item -LiteralPath $temporary
    if ($item.Length -ne $Model.Size) {
        Remove-Item -LiteralPath $temporary -Force
        throw "Size mismatch for $($Model.Name): expected $($Model.Size), received $($item.Length)"
    }
    $actualHash = (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash
    if ($actualHash -ne $Model.Sha256) {
        Remove-Item -LiteralPath $temporary -Force
        throw "SHA-256 mismatch for $($Model.Name): expected $($Model.Sha256), received $actualHash"
    }
    Move-Item -LiteralPath $temporary -Destination $Model.Path -Force
    Write-Host "Installed and verified: $($Model.Path)"
}

foreach ($model in $Models) {
    Install-VerifiedModel -Model $model
}

Write-Host ''
Write-Host 'Native AI subtitle models are ready.'
Write-Host "whisper.cpp revision: $WhisperRevision"
Write-Host "M2M100 CTranslate2 revision: $M2MRevision"
