param(
    [string]$OutputPath = (Join-Path $PSScriptRoot "..\src\MoviePlayer.ico"),
    [string]$PreviewPath = (Join-Path $PSScriptRoot "..\artifacts\app-icon-preview.png")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

function New-RoundedRectanglePath {
    param(
        [System.Drawing.RectangleF]$Rectangle,
        [float]$Radius
    )

    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $Radius * 2.0
    $arc = [System.Drawing.RectangleF]::new(
        $Rectangle.X, $Rectangle.Y, $diameter, $diameter)
    $path.AddArc($arc, 180, 90)
    $arc.X = $Rectangle.Right - $diameter
    $path.AddArc($arc, 270, 90)
    $arc.Y = $Rectangle.Bottom - $diameter
    $path.AddArc($arc, 0, 90)
    $arc.X = $Rectangle.Left
    $path.AddArc($arc, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPngBytes {
    param([int]$Size)

    $supersample = 4
    $renderSize = $Size * $supersample
    $bitmap = [System.Drawing.Bitmap]::new(
        $renderSize, $renderSize,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $unit = [float]$renderSize / 256.0
    $backgroundRect = [System.Drawing.RectangleF]::new(
        8 * $unit, 8 * $unit, 240 * $unit, 240 * $unit)
    $backgroundPath = New-RoundedRectanglePath $backgroundRect (48 * $unit)
    $backgroundBrush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(255, 17, 18, 22))
    $graphics.FillPath($backgroundBrush, $backgroundPath)

    $borderPen = [System.Drawing.Pen]::new(
        [System.Drawing.Color]::FromArgb(255, 55, 57, 66), 5 * $unit)
    $graphics.DrawPath($borderPen, $backgroundPath)

    $ringRect = [System.Drawing.RectangleF]::new(
        48 * $unit, 48 * $unit, 160 * $unit, 160 * $unit)
    $accentBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        $ringRect,
        [System.Drawing.Color]::FromArgb(255, 255, 118, 49),
        [System.Drawing.Color]::FromArgb(255, 180, 76, 216),
        35.0)
    $ringPen = [System.Drawing.Pen]::new($accentBrush, 22 * $unit)
    $ringPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $ringPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $graphics.DrawArc($ringPen, $ringRect, -72, 306)

    $playPath = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $playPoints = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(108 * $unit, 88 * $unit),
        [System.Drawing.PointF]::new(108 * $unit, 168 * $unit),
        [System.Drawing.PointF]::new(173 * $unit, 128 * $unit)
    )
    $playPath.AddPolygon($playPoints)
    $playBrush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(255, 245, 246, 249))
    $graphics.FillPath($playBrush, $playPath)

    $outputBitmap = [System.Drawing.Bitmap]::new(
        $Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $outputGraphics = [System.Drawing.Graphics]::FromImage($outputBitmap)
    $outputGraphics.InterpolationMode =
        [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $outputGraphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $outputGraphics.DrawImage($bitmap, 0, 0, $Size, $Size)

    $memory = [System.IO.MemoryStream]::new()
    $outputBitmap.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
    [byte[]]$bytes = $memory.ToArray()

    $memory.Dispose()
    $outputGraphics.Dispose()
    $outputBitmap.Dispose()
    $playBrush.Dispose()
    $playPath.Dispose()
    $ringPen.Dispose()
    $accentBrush.Dispose()
    $borderPen.Dispose()
    $backgroundBrush.Dispose()
    $backgroundPath.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
    return ,$bytes
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$images = @()
foreach ($size in $sizes) {
    [byte[]]$png = New-IconPngBytes $size
    $images += [PSCustomObject]@{ Size = $size; Bytes = $png }
}

$outputDirectory = Split-Path -Parent $OutputPath
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$stream = [System.IO.File]::Create($OutputPath)
$writer = [System.IO.BinaryWriter]::new($stream)
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$images.Count)

$offset = 6 + (16 * $images.Count)
foreach ($image in $images) {
    $dimension = if ($image.Size -eq 256) { 0 } else { $image.Size }
    $writer.Write([byte]$dimension)
    $writer.Write([byte]$dimension)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$image.Bytes.Length)
    $writer.Write([uint32]$offset)
    $offset += $image.Bytes.Length
}
foreach ($image in $images) {
    $writer.Write([byte[]]$image.Bytes)
}
$writer.Dispose()
$stream.Dispose()

$previewDirectory = Split-Path -Parent $PreviewPath
[System.IO.Directory]::CreateDirectory($previewDirectory) | Out-Null
[System.IO.File]::WriteAllBytes($PreviewPath, [byte[]]$images[-1].Bytes)

Write-Host "Created $OutputPath"
