$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$entries = @(
    @('app','随行组件'), @('weather','天气'), @('todo','待办事项'), @('system','系统状态'), @('quick','常用入口'),
    @('bluetooth-battery','蓝牙电量'), @('note','便签'), @('text','文字卡片'), @('checklist','个人清单'), @('links','网站链接')
)
$canvas = [Drawing.Bitmap]::new(1600, 704)
$graphics = [Drawing.Graphics]::FromImage($canvas)
$font = [Drawing.Font]::new('Microsoft YaHei UI', 15)
$smallFont = [Drawing.Font]::new('Microsoft YaHei UI', 10)
$darkBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255, 32, 35, 43))
$textBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255, 43, 49, 60))
try {
    $graphics.Clear([Drawing.Color]::FromArgb(255, 237, 241, 247))
    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    for ($index = 0; $index -lt $entries.Count; $index++) {
        $x = 12 + ($index % 5) * 320; $y = 12 + [int][Math]::Floor($index / 5) * 352
        $graphics.FillRectangle([Drawing.Brushes]::White, $x, $y, 296, 328)
        $graphics.FillRectangle($darkBrush, ($x+148), ($y+42), 148, 166)
        $graphics.DrawString($entries[$index][1], $font, $textBrush, ($x+14), ($y+8))
        $image = [Drawing.Image]::FromFile((Join-Path $PSScriptRoot ($entries[$index][0]+'.png')))
        try {
            $graphics.DrawImage($image, [Drawing.Rectangle]::new(($x+10), ($y+58), 128, 128))
            $graphics.DrawImage($image, [Drawing.Rectangle]::new(($x+158), ($y+58), 128, 128))
            $position = $x + 24
            foreach ($size in @(16, 24, 32, 48)) {
                $graphics.DrawImage($image, [Drawing.Rectangle]::new($position, ($y+238-$size/2), $size, $size))
                $graphics.DrawString([string]$size, $smallFont, $textBrush, $position, ($y+276))
                $position += 64
            }
        } finally { $image.Dispose() }
    }
    $canvas.Save((Join-Path $PSScriptRoot 'preview.png'), [Drawing.Imaging.ImageFormat]::Png)
} finally { $textBrush.Dispose(); $darkBrush.Dispose(); $font.Dispose(); $smallFont.Dispose(); $graphics.Dispose(); $canvas.Dispose() }
