param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
foreach ($source in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'originals') -Filter '*.png' -File) {
    $original = [Drawing.Image]::FromFile($source.FullName)
    $encoded = [Collections.Generic.List[byte[]]]::new()
    try {
        foreach ($size in $sizes) {
            $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $graphics = [Drawing.Graphics]::FromImage($bitmap)
            $stream = [IO.MemoryStream]::new()
            try {
                $graphics.Clear([Drawing.Color]::Transparent)
                $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
                $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
                $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $scale = $size / [Math]::Max($original.Width, $original.Height)
                $width = [Math]::Max(1, [int][Math]::Round($original.Width * $scale))
                $height = [Math]::Max(1, [int][Math]::Round($original.Height * $scale))
                $rectangle = [Drawing.Rectangle]::new([int](($size-$width)/2), [int](($size-$height)/2), $width, $height)
                $graphics.DrawImage($original, $rectangle)
                $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
                $encoded.Add($stream.ToArray())
                if ($size -eq 256) { $bitmap.Save((Join-Path $PSScriptRoot ($source.BaseName + '.png')), [Drawing.Imaging.ImageFormat]::Png) }
            } finally { $stream.Dispose(); $graphics.Dispose(); $bitmap.Dispose() }
        }
        $iconStream = [IO.File]::Create((Join-Path $PSScriptRoot ($source.BaseName + '.ico')))
        $writer = [IO.BinaryWriter]::new($iconStream)
        try {
            $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
            $offset = 6 + 16 * $sizes.Count
            for ($index = 0; $index -lt $sizes.Count; $index++) {
                $dimension = if ($sizes[$index] -eq 256) { 0 } else { $sizes[$index] }
                $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
                $writer.Write([byte]0); $writer.Write([byte]0)
                $writer.Write([uint16]1); $writer.Write([uint16]32)
                $writer.Write([uint32]$encoded[$index].Length); $writer.Write([uint32]$offset)
                $offset += $encoded[$index].Length
            }
            foreach ($bytes in $encoded) { $writer.Write([byte[]]$bytes) }
        } finally { $writer.Dispose(); $iconStream.Dispose() }
    } finally { $original.Dispose() }
    Write-Host ('Encoded PNG and nine-size ICO: ' + $source.BaseName)
}
