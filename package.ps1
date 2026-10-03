param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$source=Join-Path $PSScriptRoot "out\$Configuration"
if(!(Test-Path "$source\WidgetManager.exe")){throw 'Build the project first with build.ps1.'}
$destination=Join-Path $PSScriptRoot ('out\packages\WindowsWidget-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $destination | Out-Null
foreach($entry in Get-ChildItem -LiteralPath $source){
    # App languages are deliberately limited to English and Simplified Chinese.
    # Filter satellite directories while copying; never delete files in the build.
    if($entry.PSIsContainer -and $entry.Name -match '^[a-z]{2,3}(?:-[a-z0-9]{2,8})+$' -and $entry.Name -notin @('en-US','zh-CN')){continue}
    if($entry.Name -eq 'test-results'){continue}
    # The clock is installed from plugins\clock; the build output left beside the
    # executables is not used at runtime and must not ship as a stray DLL.
    if($entry.Name -in @('WidgetTests.exe','TestWidget.dll','BadAbiWidget.dll','ClockWidget.dll')){continue}
    if(!$entry.PSIsContainer -and $entry.Extension -in @('.pdb','.lib','.exp','.ilk')){continue}
    Copy-Item -LiteralPath $entry.FullName -Destination $destination -Recurse
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination $destination
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'docs') -Destination $destination -Recurse
Write-Output "Distribution: $destination"
