param(
    [ValidatePattern('^\d{1,5}\.\d{1,5}\.\d{1,5}(\.\d{1,5})?$')][string]$Version = '0.1.0',
    [switch]$SkipBuild,
    [string]$SourceDirectory = '',
    [string]$CompilerPath = ''
)
$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
foreach ($part in $Version.Split('.')) {
    if ([int]$part -gt 65535) { throw 'Version components must be at most 65535.' }
}
if (!$CompilerPath) {
    $compilerCandidates = @(
        (Join-Path $repo '.tools/inno-setup/ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 7/ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 7/ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 6/ISCC.exe')
    )
    $CompilerPath = $compilerCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (!$CompilerPath) {
        $compilerCommand = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($compilerCommand) { $CompilerPath = $compilerCommand.Source }
    }
}
if (!$CompilerPath -or !(Test-Path -LiteralPath $CompilerPath)) {
    throw 'Inno Setup compiler not found. Install Inno Setup or pass -CompilerPath. See installer/README.md.'
}
if ($SourceDirectory -and !$SkipBuild) { throw '-SourceDirectory requires -SkipBuild.' }
if (!$SkipBuild) {
    & (Join-Path $repo 'demo/NotificationCompanion/build.ps1') -Test -OutputName Installer
}
if (!$SourceDirectory) { $SourceDirectory = Join-Path $repo 'out/companion-demo/Installer' }
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
foreach ($required in @('WindowsWidget.exe','Microsoft.UI.Xaml.dll','vcruntime140.dll',
    'components/weather/widget.json','components/todo/widget.json','components/quick/widget.json',
    'components/system/widget.json','examples/bluetooth-battery/BluetoothBattery.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $source $required))) { throw "Distribution file missing: $required" }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stage = Join-Path $repo ('out/installer-stage/' + $stamp + '-' + [Guid]::NewGuid().ToString('N'))
$installerOutput = Join-Path $repo 'out/installers'
New-Item -ItemType Directory -Path $stage,$installerOutput -Force | Out-Null
function Copy-DistributionTree([string]$From, [string]$To) {
    foreach ($entry in Get-ChildItem -LiteralPath $From) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in distribution: $($entry.FullName)" }
        if ($entry.PSIsContainer) {
            if ($entry.Name -in @('test-results','backups','archive')) { continue }
            $target = Join-Path $To $entry.Name
            New-Item -ItemType Directory -Path $target -Force | Out-Null
            Copy-DistributionTree $entry.FullName $target
        } else {
            if ($entry.Extension -in @('.pdb','.ilk','.lib','.exp','.log','.jsonl')) { continue }
            if ($entry.Name -match '(?i)validation.*\.json$' -or $entry.Name -in @('DiagnosticRunner.exe','WidgetTests.exe')) { continue }
            Copy-Item -LiteralPath $entry.FullName -Destination $To
        }
    }
}
Copy-DistributionTree $source $stage
$docsStage = Join-Path $stage 'docs'
New-Item -ItemType Directory -Path $docsStage -Force | Out-Null
Copy-DistributionTree (Join-Path $repo 'docs') $docsStage
Copy-Item -LiteralPath (Join-Path $repo 'docs/THIRD_PARTY.md') -Destination $stage
$compilerOutput = & $CompilerPath "/DAppVersion=$Version" "/DSourceDir=$stage" "/DInstallerOutput=$installerOutput" (Join-Path $repo 'installer/WindowsWidget.iss') 2>&1
$compilerExit = $LASTEXITCODE
$compilerOutput | Set-Content -LiteralPath (Join-Path $stage 'compiler.log') -Encoding UTF8
if ($compilerExit -ne 0) { $compilerOutput | Write-Host; throw "Inno Setup compilation failed: $compilerExit" }
$installer = Join-Path $installerOutput "WindowsWidget-$Version-x64-Setup.exe"
if (!(Test-Path -LiteralPath $installer)) { throw 'Installer output missing.' }
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
"$hash  $([IO.Path]::GetFileName($installer))" | Set-Content -LiteralPath ($installer + '.sha256') -Encoding ASCII
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object Name -ne 'compiler.log')
[ordered]@{
    version = $Version
    time = [DateTime]::Now.ToString('o')
    source = $source
    sourceExecutableSha256 = (Get-FileHash -LiteralPath (Join-Path $source 'WindowsWidget.exe')).Hash
    stage = $stage
    compiler = (Resolve-Path -LiteralPath $CompilerPath).Path
    installer = $installer
    sha256 = $hash
    files = $files.Count
    uncompressedBytes = ($files | Measure-Object Length -Sum).Sum
    signed = (Get-AuthenticodeSignature -LiteralPath $installer).Status.ToString()
} | ConvertTo-Json | Set-Content -LiteralPath ($installer + '.json') -Encoding UTF8
Write-Host "Installer: $installer"
Write-Host "SHA256: $hash"
